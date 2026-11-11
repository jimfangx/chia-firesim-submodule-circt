#!/usr/bin/env python3
# See LICENSE for license details.
"""Generated-RTL regression for selected RAM models with shared input tokens.

Safe cases are checked against both actual SFC RTL and a transaction scoreboard.
The partial-read case uses a separate enable token for each reader: SFC loses a
shared address when the later reader is ready first. Native must handle that
legal arrival order. Host-cycle schedules are not compared: SFC's annotation-set
iteration does not preserve physical read priority. Only the eight model RAM
addresses are initialized; this test does not initialize simulated target DRAM.

Example: python MultiCycleRAMSharingTest.py --compiler goldengate-circt \
  --firtool firtool --verilator verilator --output-dir sharing-results
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from MultiCycleRAMRTLCompare import modules, ports

CASES = ('read-full-shared', 'read-addr-shared', 'rw-addr-shared', 'rw-full-shared')


def run(command, log, timeout=120):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    return result


def wrapper(source, mutation=None):
    """Expose only physical wrapper ports; never depend on RAM vector indices."""
    definitions = modules(source)
    abi = ports(definitions['Probe'])
    text = source.read_text()
    text, count = re.subn(r'\bmodule\s+Probe\b', 'module Device', text)
    assert count == 1
    declarations, wires, connections, assignments = [], [], [], []
    for name, (direction, width) in abi.items():
        size = '' if width == 1 else f'[{width - 1}:0] '
        declarations.append(f'{direction} {size}{name}')
        if direction == 'input':
            connections.append(f'.{name}({name})')
        else:
            wires.append(f'wire {size}device_{name};')
            connections.append(f'.{name}(device_{name})')
            expression = 'device_' + name
            if mutation == 'drop' and name == 'response0_valid':
                expression = "1'b0"
            if mutation == 'duplicate' and name == 'response0_valid':
                expression += ' | duplicate_response'
            if mutation == 'early-ack' and name == 'shared_ready':
                expression = 'shared_valid'
            assignments.append(f'assign {name} = {expression};')
    sticky = '''reg duplicate_response;
  always @(posedge hostClock)
    if (hostReset) duplicate_response <= 1'b0;
    else if (device_response0_valid && response0_ready) duplicate_response <= 1'b1;
''' if mutation == 'duplicate' else ''
    return text + '\nmodule Harness(\n' + ',\n'.join(declarations) + '\n);\n' + '\n'.join(wires) + '\n' + sticky + 'Device device(\n' + ',\n'.join(connections) + '\n);\n' + '\n'.join(assignments) + '\nendmodule\n'


def driver(case, reference=False, transactions=128):
    two_reads, full = case.startswith('read-'), 'full' in case
    nreads = 2 if two_reads else 1
    channels = ['shared', 'write'] + ([] if full else [f'en{i}' for i in range(nreads)])
    setup = ['d.shared_bits_addr=read_address;d.shared_bits_en=1;' if full
             else 'd.shared_bits=read_address;',
             'd.write_bits_data=write_value;d.write_bits_mask=1;']
    if two_reads:
        setup += ['d.write_bits_addr=write_address;d.write_bits_en=write_enabled;']
    elif not full:
        setup += ['d.write_bits_en=write_enabled;']
    else:
        # One shared enable supplies both read and write; read enable is ignored.
        setup[0] = 'd.shared_bits_addr=read_address;d.shared_bits_en=write_enabled;'
    if not full:
        setup += [f'd.en{i}_bits=1;' for i in range(nreads)]
    present, sample, reset, drain = [], [], [], []
    for i, name in enumerate(channels):
        present.append(f'd.{name}_valid=!sent[{i}] && host>=release[{i}];')
        sample.append(f'if(d.{name}_valid && d.{name}_ready)sent[{i}]=true;')
        reset.append(f'd.{name}_valid=0;')
        drain.append(f'd.{name}_valid=0;')
    for i in range(nreads):
        # Hold one response back to expose duplicate responses and stalled data.
        present.append(f'd.response{i}_ready=(host>={12 + i * 5}) && (rng()%4!=0);')
        sample.append(f'''if(d.response{i}_valid){{
          if(held[{i}] && held_data[{i}]!=d.response{i}_bits)fail("unstable response under backpressure");
          held[{i}]=!d.response{i}_ready;held_data[{i}]=d.response{i}_bits;
          if(d.response{i}_ready){{if(got[{i}])fail("duplicate response");got[{i}]=true;
            if(txn>=DEPTH){{if(d.response{i}_bits!=memory[read_address])fail("response data mismatch");++data_checks;trace=trace*1099511628211ULL+d.response{i}_bits;}}
          }}
        }}else if(held[{i}])fail("response valid dropped under backpressure");''')
        reset.append(f'd.response{i}_ready=0;')
        drain.append(f'd.response{i}_ready=1;')
        drain.append(f'if(d.response{i}_valid)fail("extra response after completed transaction");')
    # Alternate delayed reader enables; legal producers hold valid/bits until fire.
    release = '\n'.join(f'release[{i}]=rng()%5;' for i in range(len(channels)))
    if case == 'read-addr-shared':
        release += '\nrelease[0]=0;release[1]=0;release[2+(txn%2)]=0;release[2+((txn+1)%2)]=18;'
    # SFC failure witness is intentionally not randomized or reset away.
    if reference and case == 'read-addr-shared':
        release = 'release.fill(0);release[2+(txn%2)]=18;'
    return r'''#include "VHarness.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
static unsigned current_transaction=0,current_host=0;
void fail(const char*message){std::fprintf(stderr,"FAIL %s transaction=%u host=%u\n",message,current_transaction,current_host);std::exit(2);}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VHarness d;std::mt19937 rng(1967);
constexpr unsigned DEPTH=8;std::array<unsigned,DEPTH>memory{};uint64_t data_checks=0,reset_checks=0,trace=1469598103934665603ULL;
auto edge=[&](){d.hostClock=1;d.eval();d.hostClock=0;d.eval();};
auto reset=[&](){@RESET@d.hostReset=1;edge();edge();d.hostReset=0;++reset_checks;};
d.hostClock=0;reset();
for(unsigned txn=0;txn<DEPTH+@TRANSACTIONS@;++txn){current_transaction=txn;
  // Repeated addresses and payloads deliberately exercise distinct token identity.
  unsigned read_address=txn<DEPTH?txn:(txn/3)%DEPTH;
  unsigned write_address=@WRITE_ADDRESS@,write_value=0x100+(txn/2);
  bool inject_reset=!@WITNESS@ && txn>=DEPTH && txn%31==9;
  bool write_enabled=!inject_reset;bool finished=false;
  if(!@WITNESS@ && txn>=DEPTH && txn%29==0)reset();
  for(unsigned attempt=0;attempt<2 && !finished;++attempt){
    std::array<bool,@CHANNELS@>sent{};std::array<bool,@READS@>got{},held{};
    std::array<unsigned,@READS@>held_data{};std::array<unsigned,@CHANNELS@>release{};
    @SETUP@
    @RELEASE@
    if(inject_reset && attempt==0)release.fill(0);
    bool interrupted=false;
    for(unsigned host=0;host<160;++host){current_host=host;
      // Reset after a consumer has fired, before the shared token is acknowledged.
      if(inject_reset && attempt==0 && host==1){reset();interrupted=true;break;}
      @PRESENT@
      d.hostClock=0;d.eval();
      @SAMPLE@
      finished=true;for(bool b:sent)finished&=b;for(bool b:got)finished&=b;
      edge();if(finished)break;
    }
    if(!finished && !interrupted)fail("transaction timeout / dropped or early-acknowledged token");
  }
  if(!finished)fail("reset did not permit token replay");
  if(write_enabled)memory[write_address]=write_value;
  d.hostClock=0;
  @DRAIN_INPUTS@
  d.eval();
  @DRAIN_CHECKS@
  edge();
}
if(!data_checks)fail("no initialized data compared");
std::printf("PASS transactions=%u data_checks=%llu resets=%llu trace=%llu\n",DEPTH+@TRANSACTIONS@,(unsigned long long)data_checks,(unsigned long long)reset_checks,(unsigned long long)trace);
}
'''.replace('@RESET@', '\n'.join(reset)).replace('@TRANSACTIONS@', str(transactions)).replace('@CHANNELS@', str(len(channels))).replace('@READS@', str(nreads)).replace('@SETUP@', '\n'.join(setup)).replace('@RELEASE@', release).replace('@PRESENT@', '\n'.join(present)).replace('@SAMPLE@', '\n'.join(sample)).replace('@WRITE_ADDRESS@', '(txn<DEPTH?txn:(txn/3)%DEPTH)' if two_reads else 'read_address').replace('@WITNESS@', 'true' if reference and case == 'read-addr-shared' else 'false').replace('@DRAIN_INPUTS@', '\n'.join(line for line in drain if not line.startswith('if('))).replace('@DRAIN_CHECKS@', '\n'.join(line for line in drain if line.startswith('if(')))


def simulate(source, case, out, verilator, reference=False, mutation=None):
    out.mkdir(parents=True, exist_ok=True)
    sv, cpp = out / 'harness.sv', out / 'driver.cpp'
    sv.write_text(wrapper(source, mutation))
    cpp.write_text(driver(case, reference))
    command = [verilator, '--cc', '--exe', '--build', '-j', '2', '-Wno-fatal',
               '--top-module', 'Harness', '--prefix', 'VHarness', '--Mdir', out / 'obj',
               sv, cpp, '-CFLAGS', '-std=c++17']
    built = run(command, out / 'build.log')
    assert built.returncode == 0, f'Verilator failed: {out / "build.log"}'
    result = run([out / 'obj/VHarness'], out / 'runtime.log', timeout=15)
    failed_as_expected = mutation is not None or (reference and case == 'read-addr-shared')
    if failed_as_expected:
        expected = ('duplicate response', 'extra response') if mutation == 'duplicate' else ('transaction timeout / dropped or early-acknowledged token',)
        assert result.returncode == 2 and any(message in result.stderr for message in expected), f'negative control did not detect the intended failure: {out}: {result.stderr}'
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return {'case': case, 'reference': reference, 'mutation': mutation,
            'expected_failure': failed_as_expected, 'returncode': result.returncode,
            'runtime': result.stdout + result.stderr,
            'coverage': {name: int(value) for name, value in re.findall(r'(\w+)=(\d+)', result.stdout)},
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}


def exercise(args, root):
    fixtures = Path(__file__).with_name('MultiCycleRAMSharing')
    provenance = json.loads((fixtures / 'provenance.json').read_text())
    inputs = [Path(__file__), Path(__file__).with_name('MultiCycleRAMRTLCompare.py'),
              fixtures / 'provenance.json', args.compiler, args.firtool, args.verilator]
    (root / 'run-info.json').write_text(json.dumps({
        'scope': 'Selected RAM adapter/model RTL protocol and data regression; not full-design equivalence.',
        'inputs': [{'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                   for path in inputs]}, indent=2) + '\n')
    for name, digest in provenance['sha256'].items():
        assert hashlib.sha256((fixtures / name).read_bytes()).hexdigest() == digest, 'changed SFC fixture: ' + name
    results, native = [], {}
    for case in CASES:
        directory = root / case
        directory.mkdir(parents=True, exist_ok=True)
        materialized = directory / 'materialized'
        command = [args.compiler, fixtures / (case + '.input.fir'), '--annotation-file',
                   fixtures / (case + '.annotations.json'), '--output-dir', materialized,
                   '--materialize-ram-model', 'Probe']
        result = run(command, directory / 'compiler.log')
        assert result.returncode == 0, result.stdout + result.stderr
        # Match the standalone FIRRTL parser's accepted visibility spelling.
        fir = directory / 'native.fir'
        fir.write_text((materialized / 'post-ram-model.fir').read_text().replace('public module ', 'module '))
        sv = directory / 'native.sv'
        result = run([args.firtool, fir, '--disable-all-randomization', '-o', sv], directory / 'firtool.log')
        assert result.returncode == 0, result.stdout + result.stderr
        native[case] = sv
        actual = simulate(sv, case, directory / 'native', args.verilator)
        reference = simulate(fixtures / (case + '.sfc.sv'), case, directory / 'sfc', args.verilator, reference=True)
        # Both sides use an independent initialized-memory scoreboard. Their
        # read identities and transaction counts must agree even when cycle
        # arbitration order and transport schedule differ.
        if case != 'read-addr-shared':
            trace = lambda r: re.search(r'trace=(\d+)', r['runtime'])[1]
            assert trace(actual) == trace(reference), f'{case}: SFC/native data trace differs'
        results += [actual, reference]
        print(f'PASS {case}: native scoreboard; ' + ('SFC known token-loss witness' if reference['expected_failure'] else 'matched SFC data trace'), flush=True)
    for mutation in ('drop', 'duplicate', 'early-ack'):
        result = simulate(native['read-addr-shared'], 'read-addr-shared', root / ('mutation-' + mutation),
                          args.verilator, mutation=mutation)
        results.append(result)
        print(f'PASS negative control {mutation}: detected', flush=True)
    (root / 'result.json').write_text(json.dumps(results, indent=2) + '\n')


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
        with tempfile.TemporaryDirectory(prefix='multicycle-ram-sharing-') as directory:
            exercise(args, Path(directory))


if __name__ == '__main__':
    main()
