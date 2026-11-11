#!/usr/bin/env python3
"""Compare derived SFC/native RAM-model interfaces on defined memory traffic.

The input designs are complete generated SystemVerilog. Models, port geometry
and their module closures are discovered from the RAM channel ABI. No target
module name, register-file dimensions or port count is supplied by this test.
All addresses are initialized through the command interface before response
data is compared. Random concurrent writes use distinct addresses; directed
collisions check SFC's defined highest-index active-writer priority with a
following read transaction. Ready/valid
signals are compared throughout randomized transactions and backpressure. If
SFC has optimized away target-reset inputs, the native-only inputs receive the
adapter's valid-one/reset-zero tieoff; target-reset coverage is reported false.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

COMMENTS = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/')
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[A-Za-z_$][\w$]*')


def clean(text):
    return COMMENTS.sub(lambda m: '' if m.group().startswith(('//', '/*')) else m.group(), text)


def modules(path):
    return {m[1]: m[0] for m in re.finditer(r'\bmodule\s+([A-Za-z_$][\w$]*)\b[\s\S]*?\bendmodule', clean(path.read_text()))}


def ports(source):
    header = source[:source.index(');')]
    header = header[header.index('(') + 1:]
    result = {}; direction = width = None
    for part in header.split(','):
        m = re.fullmatch(r'\s*(?:(input|output)\s+(?:(?:wire|logic|reg)\s+)?(?:\[\s*(\d+)\s*:\s*(\d+)\s*\]\s*)?)?([A-Za-z_$][\w$]*)\s*', part)
        if not m:
            raise ValueError('unsupported generated ANSI port: ' + part)
        if m[1]:
            direction = m[1]; width = abs(int(m[2]) - int(m[3])) + 1 if m[2] else 1
        if not direction or m[4] in result:
            raise ValueError('ambiguous port declaration')
        result[m[4]] = (direction, width)
    return result


def shape(p):
    reads = sorted({int(m[1]) for n in p if (m := re.fullmatch(r'channels_read_cmds_(\d+)_valid', n))})
    writes = sorted({int(m[1]) for n in p if (m := re.fullmatch(r'channels_write_cmds_(\d+)_valid', n))})
    if not reads or not writes:
        return None
    assert reads == list(range(len(reads))) and writes == list(range(len(writes))), 'noncontiguous channel indices'
    addresses = {p[f'channels_read_cmds_{i}_bits_addr'][1] for i in reads}
    addresses |= {p[f'channels_write_cmds_{i}_bits_addr'][1] for i in writes}
    data = {p[f'channels_read_resps_{i}_bits'][1] for i in reads}
    data |= {p[f'channels_write_cmds_{i}_bits_data'][1] for i in writes}
    assert len(addresses) == len(data) == 1, 'inconsistent memory geometry'
    return len(reads), len(writes), addresses.pop(), data.pop()


def models(definitions):
    result = {}
    for name, source in definitions.items():
        if 'channels_read_cmds_' not in source[:source.index(');')]:
            continue
        p = ports(source); s = shape(p)
        if s:
            result.setdefault(s, []).append((name, p))
    return result


def closure(definitions, root, prefix):
    selected = set()
    def visit(name):
        if name in selected:
            return
        selected.add(name)
        for m in re.finditer(r'\b([A-Za-z_$][\w$]*)\s+([A-Za-z_$][\w$]*)\s*\(', definitions[name]):
            if m[1] in definitions:
                visit(m[1])
    visit(root)
    names = {name: prefix + name for name in selected}
    return '\n'.join(TOKEN.sub(lambda m: names.get(m.group(), m.group()), definitions[n]) for n in sorted(selected)), names[root]


def driver(inputs, outputs, nreads, nwrites, aw, dw, transactions, target_reset):
    assign = []; compare = []; setup_read = []; setup_write = []; host_read = []; host_write = []; write_coverage = []
    for n, (_, width) in inputs.items():
        if n in ('clock', 'reset'):
            continue
        if n == 'channels_reset_valid':
            assign.append('d.i_' + n + '=1;')
        elif n == 'channels_reset_bits':
            assign.append('d.i_' + n + '=target_reset;')
        else:
            assign.append('fill(d.i_' + n + ',0,' + str(width) + ');')
    for n, (_, width) in outputs.items():
        is_bits = re.fullmatch(r'channels_read_resps_(\d+)_bits', n)
        guard = f'check_data && d.r_channels_read_resps_{is_bits[1]}_valid' if is_bits else 'true'
        compare.append(f'if ({guard}) {{require(equal(d.r_{n},d.c_{n}),"{n}");' + ('++data_checks;if(post_collision){require(equal(d.r_' + n + ',collision_value),"collision priority ' + n + '");++collision_checks;}' if is_bits else '') + '}')
    for i in range(nreads):
        stem = f'channels_read_cmds_{i}'
        setup_read.append(f'fill(d.i_{stem}_bits_addr,post_collision?collision_address:rng()%DEPTH,{aw});')
        if stem + '_bits_en' in inputs:
            setup_read.append(f'd.i_{stem}_bits_en=1;')
        host_read.append(f'd.i_{stem}_valid=!rdone[{i}] && (rng()%4!=0); d.i_channels_read_resps_{i}_ready=(rng()%4!=0);')
    for i in range(nwrites):
        stem = f'channels_write_cmds_{i}'
        setup_write += [f'fill(d.i_{stem}_bits_addr,initializing?txn:collision?address_base:(address_base+{i})%DEPTH,{aw});',
                        f'fill(d.i_{stem}_bits_data,collision?{i + 1}:rng(),{dw});',
                        f'd.i_{stem}_bits_en=initializing?({i}==0):post_collision?0:collision?1:({i}<DEPTH && rng()%4!=0);',
                        f'd.i_{stem}_bits_mask=initializing||collision?1:(rng()%3!=0);']
        host_write.append(f'd.i_{stem}_valid=!wdone[{i}] && (rng()%4!=0);')
        write_coverage.append(f'if(!initializing){{if(!d.i_{stem}_bits_en)++disabled;else if(!d.i_{stem}_bits_mask)++masked;else ++active_writes;}}')
    handshakes = []
    for i in range(nreads):
        handshakes += [f'if(d.i_channels_read_cmds_{i}_valid && d.r_channels_read_cmds_{i}_ready) rdone[{i}]=true;',
                       f'if(d.i_channels_read_resps_{i}_ready && d.r_channels_read_resps_{i}_valid) pdone[{i}]=true;']
    for i in range(nwrites):
        handshakes.append(f'if(d.i_channels_write_cmds_{i}_valid && d.r_channels_write_cmds_{i}_ready) wdone[{i}]=true;')
    return r'''#include "VDifferential.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <type_traits>
template<class T> void fill(T& v,uint64_t seed,unsigned width) {
  if constexpr(std::is_integral_v<T>) v=width>=64?seed:(seed&((uint64_t(1)<<width)-1));
  else { constexpr unsigned words=sizeof(v)/sizeof(v[0]);for(unsigned i=0;i<words;++i){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;v[i]=seed;} if(width%32)v[words-1]&=(uint32_t(1)<<(width%32))-1; }
}
template<class T> bool equal(const T& a,const T& b) {
  if constexpr(std::is_integral_v<T>) return a==b;
  else {for(unsigned i=0;i<sizeof(a)/sizeof(a[0]);++i)if(a[i]!=b[i])return false;return true;}
}
static uint64_t cycle=0,transaction=0,data_checks=0;
void require(bool b,const char* name){if(!b){std::fprintf(stderr,"mismatch %s cycle=%llu transaction=%llu\n",name,(unsigned long long)cycle,(unsigned long long)transaction);throw std::runtime_error(name);}}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VDifferential d;std::mt19937_64 rng(1967);
constexpr uint64_t DEPTH=uint64_t(1)<<@AW@;bool target_reset=false;
bool post_collision=false,check_collision_next=false;uint64_t collision_address=0,collision_checks=0,collision_transactions=0;
std::remove_reference_t<decltype(d.i_channels_write_cmds_@LAST_WRITE@_bits_data)> collision_value{};
@ASSIGN@
auto eval=[&](bool check_data){d.i_clock=0;d.eval();@COMPARE@};
auto edge=[&](){d.i_clock=1;d.eval();d.i_clock=0;d.eval();++cycle;};
d.i_reset=1;edge();edge();d.i_reset=0;
uint64_t masked=0,disabled=0,active_writes=0,backpressured=0;
for(uint64_t txn=0;txn<DEPTH+@TRANSACTIONS@;++txn){transaction=txn;bool initializing=txn<DEPTH;bool check_data=!initializing;
  post_collision=!initializing&&check_collision_next;
  bool collision=!initializing&&!post_collision&&@NWRITES@>1&&txn%7==0;
  if(!initializing && txn%73==0){d.i_reset=1;edge();edge();d.i_reset=0;}
  target_reset=@TARGET_RESET@;
@ASSIGN@
  uint64_t address_base=rng()%DEPTH;
@SETUP_READ@
@SETUP_WRITE@
  if(collision){collision_address=address_base;collision_value=d.i_channels_write_cmds_@LAST_WRITE@_bits_data;++collision_transactions;}
@WRITE_COVERAGE@
  std::array<bool,@NREADS@> rdone{},pdone{};std::array<bool,@NWRITES@> wdone{};bool completed=false;
  for(unsigned host=0;host<1000*(@NREADS@+@NWRITES@+1);++host){
@HOST_READ@
@HOST_WRITE@
    eval(check_data);
@HANDSHAKES@
    completed=true;for(auto b:rdone)completed&=b;for(auto b:pdone)completed&=b;for(auto b:wdone)completed&=b;
    if(!completed)++backpressured;
    edge();if(completed)break;
  }
  require(completed,"transaction timeout");
  check_collision_next=collision;
}
require(data_checks>0,"no initialized response data checked");
if(@NWRITES@>1 && @TRANSACTIONS@>=14)require(collision_transactions>0 && collision_checks>0,"no collision priority coverage");
std::printf("PASS %llu target transactions, %llu host cycles, initialized depth=%llu, %u read/%u write ports, address=%u data=%u bits, %llu backpressured cycles\n",(unsigned long long)(DEPTH+@TRANSACTIONS@),(unsigned long long)cycle,(unsigned long long)DEPTH,@NREADS@,@NWRITES@,@AW@,@DW@,(unsigned long long)backpressured);
std::printf("COVERAGE {\"response_data_checks\":%llu,\"masked_writes\":%llu,\"disabled_writes\":%llu,\"active_writes\":%llu,\"backpressured_cycles\":%llu,\"collision_transactions\":%llu,\"collision_post_write_checks\":%llu}\n",(unsigned long long)data_checks,(unsigned long long)masked,(unsigned long long)disabled,(unsigned long long)active_writes,(unsigned long long)backpressured,(unsigned long long)collision_transactions,(unsigned long long)collision_checks);
}
'''.replace('@AW@', str(aw)).replace('@DW@', str(dw)).replace('@NREADS@', str(nreads)).replace('@NWRITES@', str(nwrites)).replace('@LAST_WRITE@', str(nwrites - 1)).replace('@TRANSACTIONS@', str(transactions)).replace('@TARGET_RESET@', '!initializing && !collision && !post_collision && rng()%31==0' if target_reset else 'false').replace('@ASSIGN@', '\n'.join(assign)).replace('@COMPARE@', '\n'.join(compare)).replace('@SETUP_READ@', '\n'.join(setup_read)).replace('@SETUP_WRITE@', '\n'.join(setup_write)).replace('@WRITE_COVERAGE@', '\n'.join(write_coverage)).replace('@HOST_READ@', '\n'.join(host_read)).replace('@HOST_WRITE@', '\n'.join(host_write)).replace('@HANDSHAKES@', '\n'.join(handshakes))


def compare(reference, candidate, output, verilator, transactions):
    ref_defs, cand_defs = modules(reference), modules(candidate)
    refs, cands = models(ref_defs), models(cand_defs)
    assert refs and set(refs) == set(cands), 'different RAM model geometry inventories'
    results = []
    for index, s in enumerate(sorted(refs)):
        # Definitions with the same ABI share the model implementation contract.
        # Every native definition is still compiled/checked, preserving the full
        # discovered inventory without assuming one selected register file.
        assert len(refs[s]) == len(cands[s]), 'different RAM definition counts'
        for occurrence, ((rn, rp), (cn, cp)) in enumerate(zip(refs[s], cands[s])):
            work = output / f'model-{index}-{occurrence}'; work.mkdir(parents=True, exist_ok=True)
            nreads, nwrites, aw, dw = s
            common = set(rp) & set(cp)
            assert all(rp[n] == cp[n] for n in common), 'RAM port ABI mismatch'
            for side, other in ((rp, cp), (cp, rp)):
                for n in set(side) - set(other):
                    assert n in ('channels_reset_valid', 'channels_reset_bits', 'channels_reset_ready') or re.fullmatch(r'channels_read_cmds_\d+_bits_en', n), 'unexplained trimmed port: ' + n
            ins = {n: p for n, p in {**rp, **cp}.items() if p[0] == 'input'}
            outs = {n: rp[n] for n in rp if n in common and rp[n][0] == 'output'}
            rsource, rroot = closure(ref_defs, rn, 'Reference_'); csource, croot = closure(cand_defs, cn, 'Candidate_')
            decl = []
            for n, (_, w) in ins.items():
                decl.append(f'input {"" if w==1 else f"[{w-1}:0] "}i_{n}')
            for prefix in ('r', 'c'):
                for n, (_, w) in outs.items():
                    decl.append(f'output {"" if w==1 else f"[{w-1}:0] "}{prefix}_{n}')
            def instance(name, p, prefix):
                bindings = [f'.{n}(i_{n})' for n, v in p.items() if v[0] == 'input']
                bindings += [f'.{n}({prefix}_{n})' for n in outs]
                bindings += [f'.{n}()' for n, v in p.items() if v[0] == 'output' and n not in outs]
                return name + ' ' + prefix + '(\n' + ',\n'.join(bindings) + '\n);'
            sv = work / 'models.sv'; sv.write_text(rsource + '\n' + csource + '\nmodule Differential(\n' + ',\n'.join(decl) + '\n);\n' + instance(rroot, rp, 'r') + '\n' + instance(croot, cp, 'c') + '\nendmodule\n')
            cpp = work / 'driver.cpp'; cpp.write_text(driver(ins, outs, nreads, nwrites, aw, dw, transactions, 'channels_reset_bits' in rp and 'channels_reset_bits' in cp))
            cmd = [str(verilator), '--cc', '--exe', '--build', '-j', '4', '-Wno-fatal', '--top-module', 'Differential', '--Mdir', str(work / 'obj'), '--prefix', 'VDifferential', str(sv), str(cpp)]
            cmd += ['-CFLAGS', '-std=c++17']
            p = subprocess.run(cmd, capture_output=True, text=True)
            (work / 'build.log').write_text(p.stdout + p.stderr)
            assert p.returncode == 0, 'Verilator build failed; see ' + str(work / 'build.log')
            p = subprocess.run([str(work / 'obj/VDifferential')], capture_output=True, text=True)
            (work / 'runtime.log').write_text(p.stdout + p.stderr)
            assert p.returncode == 0, p.stdout + p.stderr
            print(p.stdout, end='', flush=True)
            coverage = next((json.loads(line.removeprefix('COVERAGE ')) for line in p.stdout.splitlines() if line.startswith('COVERAGE ')), {})
            results.append({'coverage': coverage, 'reference_module': rn, 'candidate_module': cn, 'reads': nreads, 'writes': nwrites, 'address_width': aw, 'data_width': dw, 'transactions': transactions, 'target_reset_tested': 'channels_reset_bits' in rp and 'channels_reset_bits' in cp, 'status': 'passed'})
    (output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('reference', type=Path); p.add_argument('candidate', type=Path); p.add_argument('output', type=Path)
    p.add_argument('--verilator', type=Path, required=True); p.add_argument('--transactions', type=int, default=2000)
    a = p.parse_args(); a.output = a.output.resolve(); a.output.mkdir(parents=True, exist_ok=True)
    assert a.transactions > 0
    compare(a.reference.resolve(), a.candidate.resolve(), a.output, a.verilator.resolve(), a.transactions)


if __name__ == '__main__':
    main()
