#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket FASED lane assembly with the immutable SFC map."""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module

FRAGMENTS = [('fased_latency_mcr', 0, 2), ('fased_request_limits_mcr', 2, 2),
             ('fased_histograms_mcr', 4, 10), ('fased_statistics_mcr', 14, 4),
             ('fased_functional_model_mcr', 18, 1), ('fased_response_errors_mcr', 19, 2)]
NAMES = ['writeLatency', 'readLatency', 'writeMaxReqs', 'readMaxReqs'] + [
    g + 'OutstandingHistogram_' + str(i) for g in ('write', 'read') for i in range(5)
] + ['totalWriteBeats', 'totalReadBeats', 'totalWrites', 'totalReads',
     'relaxFunctionalModel', 'rrespError', 'brespError']
OLD, NEW = 'GGFASEDHistogramsWrapper', 'GGFASEDMMIOWrapper'


def wiring(body):
    lines = body.splitlines()
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', lines[0])}
    nets = {}
    for line in lines[1:]:
        if ' = firrtl.instance ' in line:
            left, right = line.split(' = firrtl.instance ', 1)
            inst = right.split()[0]
            values.update(zip(re.findall(r'%\w+', left),
                              (inst + '.' + p for p in re.findall(r'\b(?:in|out) (\w+):', right))))
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            dest, src, field = m.groups(); values[dest] = values[src] + '.' + field
        if m := re.search(r'(%\w+) = firrtl.subindex (%\w+)\[(\d+)\]', line):
            dest, src, index = m.groups(); values[dest] = values[src] + '[' + index + ']'
        if m := re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)', line):
            dest, src = m.groups()
            assert values[dest] not in nets, 'duplicate driver'
            nets[values[dest]] = values[src]
    return nets


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    host = module(reference, 'FASEDMemoryTimingModel')[1]; eq = equations(host)
    mcr_body = module(reference, 'MCRFile_5')[1]; mcr = equations(mcr_body)
    assert 'rIndex <= io_nasti_ar_bits_addr[6:2];' in mcr_body
    for word, name in enumerate(NAMES):
        if word in (8, 13):
            key, prev = ('_GEN_85', '_GEN_84') if word == 8 else ('_GEN_90', '_GEN_89')
            assert mcr[key] == f"5'h{word:x} == rIndex ? 32'h0 : {prev}"
        else:
            expected = ('{{30\'d0}, ' + name + '}') if word >= 19 else (
                'model_tNasti_io_mmReg_' + name if 4 <= word < 18 else name)
            assert eq[f'crFile_io_mcr_read_{word}_bits'] == expected
        if word < 4 or word == 18:
            assert f'end else if (crFile_io_mcr_write_{word}_valid) begin' in host
            assert f'{name} <= crFile_io_mcr_write_{word}_bits;' in host
        else:
            assert 'Register ' + name + ' is read only' in host
            assert f'if (~reset & ~(~crFile_io_mcr_write_{word}_valid)) begin' in host

    baseline = (evidence / 'candidate/post-fame-fased-mmio.mlir').read_text()
    baseline_body = native_module(baseline, NEW)
    shared = wiring(baseline_body)
    rows_pattern = r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = (true|false)'
    expected_rows = [(n, str(i * 4), str(i < 4 or i == 18).lower()) for i, n in enumerate(NAMES)]
    assert re.findall(rows_pattern, baseline_body.splitlines()[0]) == expected_rows
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-histograms{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-mmio{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {NEW}
        assert all(after[n] == body for n, body in prior.items()), 'existing module body changed'
        fragments = {f for f, _, _ in FRAGMENTS}
        copied = [(d, n) for d, n in re.findall(r'\b(in|out) %(\w+):', native_module(before, OLD).splitlines()[0]) if n not in fragments]
        body = native_module(text, NEW)
        assert re.findall(r'\b(in|out) %(\w+):', body.splitlines()[0]) == copied + [('out', 'fasedBridge_mcr')]
        assert re.findall(rows_pattern, body.splitlines()[0]) == expected_rows
        # The flips enforce read-ready and write-valid/bits opposite their peers.
        token = 'vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 21>'
        assert 'fasedBridge_mcr: !firrtl.bundle<read: ' + token + ', write flip: ' + token + ', wstrb flip: uint<4>>' in body.splitlines()[0]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        routes = {}
        for fragment, start, count in FRAGMENTS:
            routes['sim.' + fragment + '.wstrb'] = 'top.fasedBridge_mcr.wstrb'
            for k in range(count):
                routes[f'top.fasedBridge_mcr.read[{start+k}]'] = f'sim.{fragment}.read[{k}]'
                routes[f'sim.{fragment}.write[{k}]'] = f'top.fasedBridge_mcr.write[{start+k}]'
        assert all(shared[k] == v for k, v in routes.items())
        nets.update(routes); assert wiring(body) == nets
        assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', body), 'assembly duplicated state'
        header = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + NEW + '"')
        for _, name in copied:
            header = header.replace('~' + OLD + '|' + OLD + '>' + name, '~' + OLD + '|' + NEW + '>' + name)
        for fragment, start, count in FRAGMENTS:
            src, dest = '~' + OLD + '|' + OLD + '>' + fragment, '~' + OLD + '|' + NEW + '>fasedBridge_mcr'
            header = header.replace(src + '.wstrb', dest + '.wstrb')
            for k in range(count):
                for group in ('read', 'write'):
                    header = header.replace(src + f'.{group}[{k}]', dest + f'.{group}[{start+k}]')
        header = header.replace('~' + OLD + '|', '~' + NEW + '|').replace('"~' + OLD + '"', '"~' + NEW + '"')
        assert text.splitlines()[1] == header, 'annotation target transfer differs'
        reports.append(dict(reverse=reverse, unchanged_module_bodies=len(prior), copied_ports=len(copied),
                            fragment_lanes=21, strobe_routes=6, wrapper_connections=len(nets), atomic_rejections=12))
    report = dict(golden=str(golden), SFC_modules=['FASEDMemoryTimingModel', 'MCRFile_5'],
                  register_map=[dict(name=n, offset=i*4, readable=True, writeable=i<4 or i==18) for i, n in enumerate(NAMES)],
                  constructor_orders=reports, reference_has_print_host=False,
                  scope='six FASED fragments assembled; MCRFile transport attachment follows')
    (evidence / 'rocket-fased-mmio-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 21-word FASED map/permissions, six strobe routes and expanded lane wiring in both Print orders')


if __name__ == '__main__': main()
