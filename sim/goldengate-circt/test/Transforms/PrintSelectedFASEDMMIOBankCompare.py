#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print FASED MMIO assembly to the immutable 21-word SFC bank."""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDMMIOBankCompare import FRAGMENTS, NAMES, wiring

OLD, NEW = 'GGSelectedPrintFASEDHistogramsWrapper', 'GGSelectedPrintFASEDMMIOWrapper'


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
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


    baseline = wiring(native_module(baseline_path.read_text(), 'GGFASEDMMIOWrapper'))
    rows_pattern = r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = (true|false)'
    expected_rows = [(n, str(i*4), str(i < 4 or i == 18).lower()) for i, n in enumerate(NAMES)]
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration164-print-fased-histograms' / allocation
        before = (root / 'post-print-fased-histograms.mlir').read_text()
        text = (root / 'post-print-fased-mmio-bank.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-histograms' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding histogram boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated header changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {NEW}
        assert all(after[n] == body for n, body in prior.items()), 'existing module body changed'
        fragments = {f for f, _, _ in FRAGMENTS}
        copied = [p for p in native_ports(native_module(before, OLD)) if p[1] not in fragments]
        body = native_module(text, NEW)
        added = native_ports(body)
        assert added[:-1] == copied and added[-1][:2] == ('out', 'fasedBridge_mcr')
        assert re.findall(rows_pattern, body.splitlines()[0]) == expected_rows
        token = 'vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 21>'
        assert added[-1][2] == '!firrtl.bundle<read: ' + token + ', write flip: ' + token + ', wstrb flip: uint<4>>'
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
        routes = {}
        for fragment, start, count in FRAGMENTS:
            routes['sim.' + fragment + '.wstrb'] = 'top.fasedBridge_mcr.wstrb'
            for k in range(count):
                routes[f'top.fasedBridge_mcr.read[{start+k}]'] = f'sim.{fragment}.read[{k}]'
                routes[f'sim.{fragment}.write[{k}]'] = f'top.fasedBridge_mcr.write[{start+k}]'
        assert all(baseline[k] == v for k, v in routes.items())
        nets.update(routes); assert wiring(body) == nets
        assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', body), 'assembly duplicated state'
        annos = json.loads((root / 'post-print-fased-histograms-all.json').read_text())
        def transfer(v):
            if isinstance(v, dict): return {k: transfer(x) for k, x in v.items()}
            if isinstance(v, list): return [transfer(x) for x in v]
            if not isinstance(v, str): return v
            if v == '~' + OLD: return '~' + NEW
            if not v.startswith('~' + OLD + '|'): return v
            suffix = v[len('~' + OLD):]
            prefix = '|' + OLD + '>'
            if suffix.startswith(prefix):
                ref = suffix[len(prefix):]
                local = re.split(r'[.\[]', ref)[0]
                if local in {n for _, n, _ in copied}: suffix = '|' + NEW + '>' + ref
                for fragment, start, count in FRAGMENTS:
                    if local != fragment: continue
                    tail = ref[len(local):]
                    if tail == '.wstrb': suffix = '|' + NEW + '>fasedBridge_mcr.wstrb'
                    elif lane := re.fullmatch(r'\.(read|write)\[(\d+)\](.*)', tail):
                        group, index, rest = lane.groups()
                        if int(index) < count: suffix = f'|{NEW}>fasedBridge_mcr.{group}[{start+int(index)}]{rest}'
            return '~' + NEW + suffix
        actual = json.loads((root / 'post-print-fased-mmio-bank-all.json').read_text())
        assert actual == transfer(annos), 'annotation targets differ'
        rtl = clean((root / 'post-print-fased-mmio-bank.sv').read_text())
        rtl_top = module(rtl, NEW)[1]; eqs = equations(rtl_top)
        def source(net):
            seen = set()
            while net in eqs and net not in seen:
                seen.add(net); net = eqs[net]
            return net
        match = re.search(r'\b' + OLD + r'\s+sim\s*\((.*?)\);', rtl_top, re.S)
        assert match
        pins = {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', match[1])}
        child_eq = equations(module(rtl, OLD)[1])
        compared = 0
        for fragment, start, count in FRAGMENTS:
            assert source(pins[fragment + '_wstrb']) == 'fasedBridge_mcr_wstrb'
            compared += 1
            for k in range(count):
                for group in ('read', 'write'):
                    for leaf in ('ready', 'valid', 'bits'):
                        external = source(f'fasedBridge_mcr_{group}_{start+k}_{leaf}')
                        inner = source(pins[f'{fragment}_{group}_{k}_{leaf}'])
                        # CIRCT can fold a child's constant output in the parent
                        # while retaining a named unused instance output pin.
                        constant = child_eq.get(f'{fragment}_{group}_{k}_{leaf}', '')
                        if inner and external == inner: pass
                        else:
                            assert re.fullmatch(r"\d+'[hbd][0-9a-f]+", constant) and external == constant, (
                                allocation, fragment, group, k, leaf, external, inner, constant)
                        compared += 1
        assert compared == 132
        reports.append(dict(allocation=allocation, unchanged_module_bodies=len(prior), copied_typed_ports=len(copied),
                            wrapper_connections=len(nets), MMIO_words=21, strobe_routes=6,
                            RTL_fragment_pins_compared=compared, annotations_compared=len(actual), atomic_rejections=13))
    report = dict(golden=str(golden), SFC_modules=['FASEDMemoryTimingModel', 'MCRFile_5'],
                  register_map=[dict(name=n, offset=i*4, readable=True, writeable=i<4 or i==18) for i, n in enumerate(NAMES)],
                  allocations=reports, reference_has_print_host=False,
                  scope='selected six-fragment MMIO assembly; MCRFile/control attachment follows')
    (evidence / 'selected-fased-mmio-bank-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 21-word FASED map/permissions, typed lane/strobe/SV wiring and annotation transfer in both Print allocations')


if __name__ == '__main__': main()
