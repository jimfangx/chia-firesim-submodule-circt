#!/usr/bin/env python3
# See LICENSE for license details.
"""Check physical response packets containing fields from separate RAM reads.

The native join must hold every read response until the entire physical packet
is valid and accepted. A pinned actual SFC reference supplies the normal-traffic
comparison and a known premature-packet witness under skewed request arrivals.
The independent scoreboard checks initialized model RAM, input/output stalls,
repeated tokens, and in-flight host resets. No target DRAM is initialized, and
this finite model-level simulation does not establish full-design equivalence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from MultiCycleRAMRTLCompare import modules, ports


def run(command, log, timeout=120):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    return result


def harness(source, bad_join=False):
    definitions = modules(source)
    abi = ports(definitions['Grouped'])
    text, count = re.subn(r'\bmodule\s+Grouped\b', 'module Device', source.read_text())
    assert count == 1
    response_valids = []
    if bad_join:
        # Discover the real host-model instance/response ABI. The mutation
        # changes AND-valid to OR-valid without assuming source/read priority.
        hosts = []
        for module, instance in re.findall(r'\b(\w+)\s+(\w+)\s*\(', definitions['Grouped']):
            if module in definitions:
                valid_ports = [name for name in ports(definitions[module])
                               if re.fullmatch(r'channels_read_resps_\d+_valid', name)]
                if valid_ports:
                    hosts.append((instance, valid_ports))
        assert len(hosts) == 1 and len(hosts[0][1]) == 2
        response_valids = ['device.' + hosts[0][0] + '.' + name for name in hosts[0][1]]
    declarations, wires, connections, assignments = [], [], [], []
    for name, (direction, width) in abi.items():
        size = '' if width == 1 else f'[{width - 1}:0] '
        declarations.append(f'{direction} {size}{name}')
        if direction == 'input':
            connections.append(f'.{name}({name})')
        else:
            wires.append(f'wire {size}device_{name};')
            connections.append(f'.{name}(device_{name})')
            expression = ('written_tokens >= 4 ? (' + ' | '.join(response_valids) + ') : device_responses_valid'
                          if bad_join and name == 'responses_valid' else 'device_' + name)
            assignments.append(f'assign {name} = {expression};')
    counter = '''reg [31:0] written_tokens;
  always @(posedge hostClock)
    if (hostReset) written_tokens <= 0;
    else if (write_valid && write_ready) written_tokens <= written_tokens + 1;
''' if bad_join else ''
    return text + '\nmodule Harness(\n' + ',\n'.join(declarations) + '\n);\n' + '\n'.join(wires) + '\n' + counter + 'Device device(\n' + ',\n'.join(connections) + '\n);\n' + '\n'.join(assignments) + '\nendmodule\n'


def driver(skew):
    return r'''#include "VHarness.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
static unsigned transaction=0,host_cycle=0;
void fail(const char*message){std::fprintf(stderr,"FAIL %s transaction=%u host=%u\n",message,transaction,host_cycle);std::exit(2);}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VHarness d;std::mt19937 rng(1967);
constexpr unsigned DEPTH=4,TRANSACTIONS=128;std::array<unsigned,DEPTH>memory{};
uint64_t fields_checked=0,packets=0,resets=0,stalled=0,trace=1469598103934665603ULL;
auto edge=[&](){d.hostClock=1;d.eval();d.hostClock=0;d.eval();};
auto reset=[&](){d.rq0_valid=0;d.rq1_valid=0;d.write_valid=0;d.responses_ready=0;d.hostReset=1;edge();edge();d.hostReset=0;++resets;};
d.hostClock=0;reset();
for(unsigned txn=0;txn<DEPTH+TRANSACTIONS;++txn){transaction=txn;
  unsigned a0=txn<DEPTH?0:(txn/2)%DEPTH,a1=txn<DEPTH?0:(a0+1)%DEPTH;
  unsigned wa=txn<DEPTH?txn:(txn/3)%DEPTH,wd=(0x20+0x35*wa+7*(txn/2))&255;
  bool inject_reset=txn>=DEPTH && txn%31==9,write_enabled=!inject_reset,complete=false;
  if(txn>=DEPTH && txn%29==0)reset();
  d.rq0_bits_addr=a0;d.rq1_bits_addr=a1;d.rq0_bits_en=1;d.rq1_bits_en=1;
  d.write_bits_addr=wa;d.write_bits_data=wd;d.write_bits_en=write_enabled;d.write_bits_mask=1;
  for(unsigned attempt=0;attempt<2 && !complete;++attempt){
    std::array<bool,3>sent{};bool got=false,held=false,interrupted=false;unsigned held0=0,held1=0;
    unsigned delayed=txn%2,delay=@SKEW@ && txn>=DEPTH?18:0;
    for(unsigned host=0;host<160;++host){host_cycle=host;
      if(inject_reset && attempt==0 && host==1){reset();interrupted=true;break;}
      d.rq0_valid=!sent[0] && (delayed!=0 || host>=delay);
      d.rq1_valid=!sent[1] && (delayed!=1 || host>=delay);
      d.write_valid=!sent[2];
      // Normal traffic holds both model responses until they are available;
      // skewed traffic allows a ready consumer before the other field exists.
      d.responses_ready=(@SKEW@ && txn>=DEPTH?true:host>=12) && rng()%4!=0;
      d.hostClock=0;d.eval();
      if(d.rq0_valid && d.rq0_ready)sent[0]=true;
      if(d.rq1_valid && d.rq1_ready)sent[1]=true;
      if(d.write_valid && d.write_ready)sent[2]=true;
      if(d.responses_valid){
        if(!sent[0] || !sent[1])fail("early grouped response / stale payload field");
        if(held && (held0!=d.responses_bits_d0 || held1!=d.responses_bits_d1))fail("unstable grouped response under backpressure");
        held=!d.responses_ready;held0=d.responses_bits_d0;held1=d.responses_bits_d1;
        if(held)++stalled;
        if(d.responses_ready){
          if(got)fail("duplicate grouped response");got=true;++packets;
          if(txn>=DEPTH){
            if(d.responses_bits_d0!=memory[a0] || d.responses_bits_d1!=memory[a1])fail("stale or misrouted grouped payload");
            fields_checked+=2;trace=trace*1099511628211ULL+d.responses_bits_d0;trace=trace*1099511628211ULL+d.responses_bits_d1;
          }
        }
      }else if(held)fail("grouped response valid dropped under backpressure");
      complete=sent[0]&&sent[1]&&sent[2]&&got;edge();if(complete)break;
    }
    if(!complete && !interrupted)fail("grouped response timeout / lost field");
  }
  if(!complete)fail("in-flight reset did not permit replay");
  if(write_enabled)memory[wa]=wd;
  d.rq0_valid=0;d.rq1_valid=0;d.write_valid=0;d.responses_ready=1;d.hostClock=0;d.eval();
  if(d.responses_valid)fail("extra grouped packet after completed transaction");edge();
}
if(fields_checked!=2*TRANSACTIONS || !stalled || resets<2)fail("missing initialized-data, backpressure, or reset coverage");
std::printf("PASS transactions=%u fields_checked=%llu packets=%llu resets=%llu stalled=%llu trace=%llu\n",DEPTH+TRANSACTIONS,(unsigned long long)fields_checked,(unsigned long long)packets,(unsigned long long)resets,(unsigned long long)stalled,(unsigned long long)trace);
}
'''.replace('@SKEW@', 'true' if skew else 'false')


def simulate(source, out, verilator, skew=False, reference=False, bad_join=False):
    out.mkdir(parents=True, exist_ok=True)
    sv, cpp = out / 'harness.sv', out / 'driver.cpp'
    sv.write_text(harness(source, bad_join))
    cpp.write_text(driver(skew))
    command = [verilator, '--cc', '--exe', '--build', '-j', '2', '-Wno-fatal',
               '--top-module', 'Harness', '--prefix', 'VHarness', '--Mdir', out / 'obj',
               sv, cpp, '-CFLAGS', '-std=c++17']
    result = run(command, out / 'build.log')
    assert result.returncode == 0, f'Verilator failed: {out / "build.log"}'
    result = run([out / 'obj/VHarness'], out / 'runtime.log', timeout=15)
    expected_failure = bad_join or (reference and skew)
    if expected_failure:
        assert result.returncode == 2 and any(message in result.stderr for message in
            ('early grouped response / stale payload field', 'stale or misrouted grouped payload')), result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return {'skew': skew, 'reference': reference, 'bad_join_mutation': bad_join,
            'expected_failure': expected_failure, 'returncode': result.returncode,
            'runtime': result.stdout + result.stderr,
            'coverage': {name: int(value) for name, value in re.findall(r'(\w+)=(\d+)', result.stdout)},
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}


def exercise(args, root):
    fixtures = Path(__file__).with_name('MultiCycleRAMResponseJoin')
    provenance = json.loads((fixtures / 'provenance.json').read_text())
    for name, digest in provenance['sha256'].items():
        assert hashlib.sha256((fixtures / name).read_bytes()).hexdigest() == digest, 'changed SFC fixture: ' + name
    inputs = [Path(__file__), Path(__file__).with_name('MultiCycleRAMRTLCompare.py'),
              fixtures / 'provenance.json', args.compiler, args.firtool, args.verilator]
    (root / 'run-info.json').write_text(json.dumps({
        'scope': 'Finite selected RAM response-join model RTL regression; not full-design equivalence.',
        'inputs': [{'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                   for path in inputs]}, indent=2) + '\n')
    result = run([args.compiler, fixtures / 'input.fir', '--annotation-file', fixtures / 'annotations.json',
                  '--output-dir', root / 'materialized', '--materialize-ram-model', 'Grouped'], root / 'compiler.log')
    assert result.returncode == 0, result.stdout + result.stderr
    fir = root / 'native.fir'
    fir.write_text((root / 'materialized/post-ram-model.fir').read_text().replace('public module ', 'module '))
    native = root / 'native.sv'
    result = run([args.firtool, fir, '--disable-all-randomization', '-o', native], root / 'firtool.log')
    assert result.returncode == 0, result.stdout + result.stderr
    actual = simulate(native, root / 'native-normal', args.verilator)
    reference = simulate(fixtures / 'sfc.sv', root / 'sfc-normal', args.verilator, reference=True)
    assert actual['coverage']['trace'] == reference['coverage']['trace'], 'normal SFC/native packets differ'
    print('PASS normal response join: matched actual SFC packets and initialized-data scoreboard', flush=True)
    skew = simulate(native, root / 'native-skew', args.verilator, skew=True)
    assert skew['coverage']['trace'] == actual['coverage']['trace'], 'native request skew changed packet data'
    witness = simulate(fixtures / 'sfc.sv', root / 'sfc-skew-witness', args.verilator, skew=True, reference=True)
    mutant = simulate(native, root / 'mutation-or-valid', args.verilator, skew=True, bad_join=True)
    # Distinct fields have a join contract; two independent producers driving
    # the same field need arbitration and must fail instead of last-connect.
    annotations = json.loads((fixtures / 'annotations.json').read_text())
    reads = [annotation for annotation in annotations if annotation['class'].endswith('.ModelReadPort')]
    assert len(reads) == 2
    reads[1]['data'] = reads[0]['data']
    invalid = root / 'same-field.annotations.json'
    invalid.write_text(json.dumps(annotations, indent=2) + '\n')
    rejected = run([args.compiler, fixtures / 'input.fir', '--annotation-file', invalid,
                    '--output-dir', root / 'same-field-materialized', '--materialize-ram-model', 'Grouped'],
                   root / 'same-field-rejection.log')
    assert rejected.returncode != 0 and 'payload field shared by different producers' in rejected.stderr, rejected.stdout + rejected.stderr
    print('PASS skewed requests, backpressure, repeated tokens, and in-flight host resets', flush=True)
    print('PASS negative controls: SFC premature packet and OR-valid join expose stale/incomplete payload', flush=True)
    print('PASS same-field response producers rejected without arbitration', flush=True)
    rejection = {'case': 'same-field-producers', 'expected_failure': True,
                 'returncode': rejected.returncode, 'runtime': rejected.stdout + rejected.stderr,
                 'annotation_sha256': hashlib.sha256(invalid.read_bytes()).hexdigest()}
    (root / 'result.json').write_text(json.dumps([actual, reference, skew, witness, mutant, rejection], indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ('compiler', 'firtool', 'verilator'):
        parser.add_argument('--' + tool, type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--output-dir', type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        exercise(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix='multicycle-ram-response-join-') as directory:
            exercise(args, Path(directory))


if __name__ == '__main__':
    main()
