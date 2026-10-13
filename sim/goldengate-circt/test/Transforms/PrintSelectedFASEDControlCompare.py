#!/usr/bin/env python3
# See LICENSE for license details.
"""Check selected Print FASED local control transport against SFC.

The fixture disables Print: compare shared transport semantics and connections,
and preserve the live selected catalog until FASED allocation is implemented.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketFASEDControlCompare import mcr_semantics, ports, transfer, ADDRESS, DATA, READ, RESPONSE
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

OLD = 'GGSelectedPrintFASEDMMIOWrapper'
CONTROL = 'GGSelectedPrintFASEDBridgeControlWrapper'
MCR = 'GGFASEDMCRFile'


def oracle(golden):
    reference = clean(golden.read_text())
    mcr = module(reference, 'MCRFile_5')[1]
    eq = equations(mcr)
    for ch in ('aw', 'w', 'ar'):
        assert eq['io_nasti_' + ch + '_ready'] == '~' + ch + 'Fired'
    handshakes = {'aw': '_T', 'w': '_T_5', 'ar': '_T_6', 'r': '_T_11', 'b': '_T_12'}
    for ch, handshake in handshakes.items():
        assert eq[handshake] == f'io_nasti_{ch}_ready & io_nasti_{ch}_valid'
    for register, ch, field in [('bId', 'aw', 'id'), ('rId', 'ar', 'id'), ('wData', 'w', 'data')]:
        assert re.search(r'if \(' + handshakes[ch] + r'\) begin\s+' + register +
                         r' <= io_nasti_' + ch + '_bits_' + field + r';\s+end', mcr)
    assert eq['io_nasti_b_valid'] == '_io_mcr_write_valid_T & wCommited'
    assert eq['_io_mcr_write_valid_T'] == 'awFired & wFired'
    assert eq['io_nasti_r_valid'] == 'arFired'
    assert eq['io_nasti_b_bits_id'] == 'bId' and eq['io_nasti_r_bits_id'] == 'rId'
    assert eq['_GEN_1'] == "_T ? io_nasti_aw_bits_addr[24:2] : {{18'd0}, wIndex}"
    assert 'wIndex <= _GEN_1[4:0];' in mcr
    assert re.search(r'if \(_T_6\) begin\s+rIndex <= io_nasti_ar_bits_addr\[6:2\];', mcr)
    assert eq['_GEN_12'] == "_T_12 ? 1'h0 : wCommited"
    assert eq['_GEN_55'] == '_GEN_54 | _GEN_12'
    assert re.search(r"if \(reset\) begin\s+wCommited <= 1'h0;\s+end else begin\s+wCommited <= _GEN_55;", mcr)
    for flag, capture, retire in [('arFired', '_T_6', '_T_11'),
                                  ('awFired', '_T', '_T_12'), ('wFired', '_T_5', '_T_12')]:
        assert re.search(r'if \(reset\) begin\s+' + flag + r" <= 1'h0;\s+end else if \(" + retire + r'\)', mcr)
        gate = {'arFired': '_GEN_6', 'awFired': '_GEN_0', 'wFired': '_GEN_3'}[flag]
        assert eq[gate] == capture + ' | ' + flag
    for word in range(21):
        assert eq[f'io_mcr_write_{word}_valid'] == f"5'h{word:x} == wIndex & (awFired & wFired & ~wCommited)"
        if word:
            write_previous = 'io_mcr_write_0_valid' if word == 1 else '_GEN_' + str(33 + word)
            assert eq['_GEN_' + str(34 + word)] == f"5'h{word:x} == wIndex ? io_mcr_write_{word}_valid : {write_previous}"
            key = 'io_nasti_r_bits_data' if word == 20 else '_GEN_' + str(77 + word)
            value = "32'h0" if word in (8, 13) else f'io_mcr_read_{word}_bits'
            previous_read = 'io_mcr_read_0_bits' if word == 1 else '_GEN_' + str(76 + word)
            assert eq[key] == f"5'h{word:x} == rIndex ? {value} : {previous_read}"
    assert [n for n in re.findall(r'\breg(?:\s+\[\d+:0\])?\s+(\w+);', mcr) if not n.startswith('_RAND_')] == [
        'arFired', 'awFired', 'wFired', 'wCommited', 'bId', 'rId', 'wData', 'wIndex', 'rIndex']
    host_header, host = module(reference, 'FASEDMemoryTimingModel')
    host_eq, platform_eq = equations(host), equations(module(reference, 'FPGATop')[1])
    assert host_eq['crFile_clock'] == 'clock' and host_eq['crFile_reset'] == 'reset'
    assert len(instance(host, 'MCRFile_5', 'crFile')) > 60
    router = module(reference, 'NastiRouter')[1]
    for ch in ('aw', 'ar'):
        assert f"25'h80 <= io_master_{ch}_bits_addr & io_master_{ch}_bits_addr < 25'h100" in router
    pins = re.findall(r'\b(io_ctrl_\w+)\b', host_header)
    assert len(pins) == 20
    for pin in pins:
        ch, field = pin[len('io_ctrl_'):].split('_', 1)
        a = 'FASEDMemoryTimingModel_0_' + pin
        b = 'ctrlInterconnect_io_slaves_1_' + ch + '_' + field
        assert platform_eq.get(a) == b or platform_eq.get(b) == a


def main():
    evidence, golden, baseline = (Path(arg).resolve() for arg in sys.argv[1:])
    oracle(golden)
    shared = (baseline / 'post-fame-fased-control.mlir').read_text()
    mcr_semantics(shared)
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration165-print-fased-mmio-bank' / allocation
        for name in ('post-print-fased-mmio-bank.mlir', 'post-print-fased-mmio-bank.sv',
                     'post-print-fased-mmio-bank-all.json', 'print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding bank/header changed: ' + name
        before = (root / 'post-print-fased-mmio-bank.mlir').read_text()
        text = (root / 'post-print-fased-control.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {MCR, CONTROL}
        assert all(after[n] == body for n, body in prior.items()), 'existing body changed'
        assert native_module(text, MCR) == native_module(shared, MCR)
        mcr_semantics(text)
        old, controlled = (native_module(text, n) for n in (OLD, CONTROL))
        copied = [(d, n) for d, n in ports(old) if n != 'fasedBridge_mcr']
        assert ports(controlled) == copied + [('in', 'fasedBridge_ctrl')]
        attached = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        attached.update({'crFile.clock': 'top.hostClock', 'crFile.reset': 'top.hostReset',
                         'crFile.nasti': 'top.fasedBridge_ctrl', 'crFile.mcr': 'sim.fasedBridge_mcr'})
        assert wiring(controlled) == attached
        assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', controlled)
        assert text.splitlines()[1] == transfer(before.splitlines()[1], OLD, CONTROL, copied), 'archive transfer differs'
        # FASED is absent from the selected catalog at this milestone. Do not
        # assign a fabricated slave or mutate existing addresses/dispatchers.
        assert 'name = "FASEDMemoryTimingModel_0", size =' not in text
        assert native_module(text, 'GGControlAddressDecode') == native_module(before, 'GGControlAddressDecode')
        annos = json.loads((root / 'post-print-fased-mmio-bank-all.json').read_text())
        def retarget(v, old, new, copied):
            if isinstance(v, dict): return {k: retarget(x, old, new, copied) for k, x in v.items()}
            if isinstance(v, list): return [retarget(x, old, new, copied) for x in v]
            if not isinstance(v, str): return v
            if v == '~' + old: return '~' + new
            if not v.startswith('~' + old + '|'): return v
            suffix = v[len('~' + old):]
            prefix = '|' + old + '>'
            if suffix.startswith(prefix) and re.split(r'[.\[]', suffix[len(prefix):])[0] in {n for _, n in copied}:
                suffix = '|' + new + '>' + suffix[len(prefix):]
            return '~' + new + suffix
        annos = retarget(annos, OLD, CONTROL, copied)
        actual = json.loads((root / 'post-print-fased-control-all.json').read_text())
        assert annos == actual
        rtl = clean((root / 'post-print-fased-control.sv').read_text())
        control_rtl = module(rtl, CONTROL)[1]
        cr = instance(control_rtl, MCR, 'crFile')
        assert cr['clock'] == 'hostClock' and cr['reset'] == 'hostReset'
        # Remaining exported request/response pins survive optimization; use
        # equations for aliases and constant folding through the child MCRFile.
        eqs = equations(control_rtl)
        def resolve(v):
            seen = set()
            while v in eqs and v not in seen:
                seen.add(v); v = eqs[v]
            return v
        child_eq = equations(module(rtl, MCR)[1]); pins_compared = 0
        for pin, net in cr.items():
            if not pin.startswith('nasti_'): continue
            external = resolve('fasedBridge_ctrl_' + pin[len('nasti_'):])
            internal = resolve(net)
            constant = child_eq.get(pin, '')
            assert external == internal or (re.fullmatch(r"\d+'[hbd][0-9a-f]+", constant) and external == constant), (pin, external, internal)
            pins_compared += 1
        assert pins_compared >= 20
        reports.append(dict(allocation=allocation, unchanged_module_bodies=len(prior),
                            transport_registers=9, reset_flags=4, copied_typed_ports=len(copied),
                            attachment_connections=len(attached), exposed_control_channels=5,
                            annotations_compared=len(actual), RTL_control_pins_compared=pins_compared,
                            atomic_rejections=19, catalog_FASED_slave=None))
    report = dict(golden=str(golden), SFC_modules=['MCRFile_5', 'FASEDMemoryTimingModel', 'NastiRouter', 'FPGATop'],
                  allocations=reports, words=21, local_index='addr[6:2]', legacy_strobe=0,
                  limitation='SFC disables Print; FASED is absent from the selected catalog; allocate its control region/dispatchers next; enabled-Print runtime parity remains pending')
    (evidence / 'selected-fased-control-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS selected FASED MCRFile state/decode, local AXI RTL pins and annotation/collateral identity')


if __name__ == '__main__': main()
