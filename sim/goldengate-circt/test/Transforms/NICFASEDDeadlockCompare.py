#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC ingress deadlock assertions with recorded SFC RTL.

Observe the actual assertion predicate and enable operands after native RTL
lowering. Exhaust all nine boundary inputs against the SFC IngressModule
expression cones. This checks ingress assertions, not complete NIC execution,
response buffering, timing/MMIO or host platform composition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
from NICMMIOBankCompare import modules
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, equations, module
from PrintSelectedFASEDIngressDeadlockCompare import compare, oracle, HELPER, MESSAGES

CHAIN = ['GGFASEDIngress' + n + 'Wrapper' for n in
         ('Issue', 'Order', 'Credits', 'ARQueue', 'WQueue', 'AW')] + ['GGFASEDIngressAW']


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    exported, native = out / 'helpers.mlir', out / 'helpers.sv'
    run([export_tool, candidate, exported], 'export')
    production, isolated = modules(candidate.read_text()), modules(exported.read_text())
    assert set(isolated) == {HELPER, HELPER + 'Observed'}
    def visibility(body):
        return body.replace('firrtl.module public @', 'firrtl.module @').replace(
            'firrtl.module private @', 'firrtl.module @')
    assert visibility(isolated[HELPER]) == visibility(production[HELPER]), 'export rebuilt assertions'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    sfc_body = module(clean(golden.read_text()), 'IngressModule')[1]
    nets = equations(sfc_body)
    cp = ports(module(clean(native.read_text()), HELPER + 'Observed')[0])
    bindings = {'reset': 'reset', 'io_relaxed': 'relaxed',
                'SatUpDownCounter_io_empty': 'wEmpty',
                'SatUpDownCounter_1_io_empty': 'awEmpty',
                'xaction_order_io_deq_valid': 'orderValid',
                'wQueue_io_enq_valid': 'wValid', 'wQueue_io_enq_ready': 'wReady',
                'awQueue_io_enq_valid': 'awValid', 'awQueue_io_enq_ready': 'awReady'}
    emitted = []
    token = r"\d+'[sS]?[bBoOdDhH][0-9a-fA-F_xzXZ]+|\b[A-Za-z_]\w*\b"
    def resolve(n):
        if n in bindings:
            return bindings[n]
        assert n in nets, 'unresolved SFC assertion dependency ' + n
        expression = re.sub(token, lambda m: m[0] if "'" in m[0] else resolve(m[0]), nets[n])
        symbol = 'sfc_' + n
        emitted.append('wire ' + symbol + ' = ' + expression + ';')
        bindings[n] = symbol
        return symbol
    # Read the actual SFC assertion guards and predicates, then recursively
    # expand their emitted wires. Do not replace them with expected formulas.
    assertions = re.findall(r'if \(([^\n]+)\) begin\s+assert\((\w+)\);', sfc_body)
    assert assertions == [('~reset', '_T_15'), ('_T_17', '_T_24')], assertions
    references = []
    for enable, predicate in assertions:
        guard = re.sub(token, lambda m: m[0] if "'" in m[0] else resolve(m[0]), enable)
        references.append('(' + guard + ') & ~' + resolve(predicate))
    inputs = ['reset', 'relaxed', 'awEmpty', 'wEmpty', 'orderValid',
              'awValid', 'awReady', 'wValid', 'wReady']
    assert cp == dict([('clock', ['input', 1])] + [(n, ['input', 1]) for n in inputs] +
                      [('wFailure', ['output', 1]), ('awFailure', ['output', 1])]), cp
    tb = ['module check; reg clock=0,assertionClock=0;', 'reg ' + ','.join(inputs) + ';',
          'wire wFailure,awFailure;', 'integer flags,wFailures=0,awFailures=0,resetDisabled=0;',
          *emitted, 'wire wReference=' + references[0] + ';',
          'wire awReference=' + references[1] + ';',
          HELPER + 'Observed observed(' + ','.join('.' + n + '(' + n + ')' for n in cp) + ');',
          HELPER + ' assertions(.clock(assertionClock),' + ','.join('.' + n + '(' + n + ')' for n in inputs) + ');',
          'initial begin ' + ';'.join(n + '=0' for n in inputs) + ';',
          # Exercise the unchanged production AssertOps as well as their
          # observable operands. Failed cases run in separate processes.
          'if($test$plusargs("fail_w"))begin relaxed=1;wEmpty=1;wValid=1;#1;assertionClock=1;#1;$fatal(1,"W assertion did not fire");end',
          'if($test$plusargs("fail_aw"))begin relaxed=1;awEmpty=1;awValid=1;#1;assertionClock=1;#1;$fatal(1,"AW assertion did not fire");end',
          'for(flags=0;flags<512;flags++)begin']
    tb += [n + '= (flags >> ' + str(j) + ') & 1;' for j, n in enumerate(inputs)]
    tb += ['#1;', 'if(wFailure!==wReference || awFailure!==awReference)$fatal(1,"SFC assertion mismatch flags=%0d",flags);',
           'clock=1;#1;clock=0;',
           'if(wFailure!==wReference || awFailure!==awReference)$fatal(1,"clock changed assertion predicate");',
           'if(!wReference&&!awReference)begin assertionClock=1;#1;assertionClock=0;end',
           'wFailures+=wFailure;awFailures+=awFailure;',
           'if(reset)begin if(wFailure||awFailure)$fatal(1,"reset guard lost");resetDisabled++;end',
           'end', 'if(wFailures!=32||awFailures!=32||resetDisabled!=256)$fatal(1,"coverage");',
           '$display("PASS 512 SFC assertion cases, 32 W and 32 AW failures, 256 reset-disabled cases");',
           '$finish;end endmodule']
    (out / 'check.sv').write_text('\n'.join(tb) + '\n')
    run(['verilator', '--binary', '-j', '4', '--timing', '--assert', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', native], 'verilator')
    run([out / 'obj/check'], 'simulation')
    assert 'PASS 512 SFC assertion cases' in (out / 'simulation.log').read_text()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for ch, message in zip(('w', 'aw'), MESSAGES):
        log_path = out / ('expected-failure-' + ch + '.log')
        with log_path.open('w') as log:
            failed = subprocess.run([str(out / 'obj/check'), '+fail_' + ch], stdout=log,
                                    stderr=subprocess.STDOUT)
        assert failed.returncode != 0 and message in log_path.read_text(), 'production assertion did not fail: ' + ch
    return native, dict(cases=512, failures_per_channel=32, reset_disabled_cases=256,
                        production_helper_cloned_verbatim=True, actual_sfc_expression_cones=True,
                        production_assertion_failure_runs=2, production_assertions_clocked_in_safe_cases=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    before = a.candidate_dir / 'post-fame-fased-ingress-issue.mlir'
    after = a.candidate_dir / 'post-fame-fased-ingress-deadlock.mlir'
    oracle(a.golden)
    rtl, behavior = differential(after, a.golden, a.export_tool, a.output_dir)
    result = compare(before, after, a.baseline.read_text(), chain=CHAIN, rtl_path=rtl)
    result.update(golden=str(a.golden.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(after.read_bytes()).hexdigest(),
                  rtl_differential=behavior, limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS NIC actual enqueue wiring, reset/clock, policy forwarding and exhaustive SFC RTL assertion differential')


if __name__ == '__main__':
    main()
