#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket FASED transport/binding with immutable SFC.

Validate actual FIRRTL SSA and all five channels. SFC disables Print, so its
FASED local transport is the oracle; global allocation comes from the expanded
decoder catalog. This check does not claim Print-enabled RTL/runtime parity.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketFASEDIssueCompare import expressions
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

OLD = 'GGFASEDMMIOWrapper'
CONTROL = 'GGFASEDBridgeControlWrapper'
BOUND = 'GGFASEDBridgeBoundWrapper'
MCR = 'GGFASEDMCRFile'
ADDRESS = ['addr', 'len', 'size', 'burst', 'lock', 'cache', 'prot', 'qos', 'region', 'id', 'user']
DATA = ['data', 'last', 'id', 'strb', 'user']
RESPONSE = ['resp', 'id', 'user']
READ = ['resp', 'data', 'last', 'id', 'user']


def ports(body):
    return re.findall(r'\b(in|out) %(\w+):', body.splitlines()[0])


def transfer(header, old, new, copied):
    header = header.replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
    for _, name in copied:
        header = re.sub(re.escape('~' + old + '|' + old + '>' + name) + r'(?=[.\["\\])',
                        '~' + old + '|' + new + '>' + name, header)
    return header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')


def mcr_semantics(text):
    body = native_module(text, MCR)
    # The shared extractor treats register SSA values as state leaves. RegOp
    # needs the same leaf handling as RegResetOp; the IR itself stays intact.
    nets, checks = expressions(text.replace(' = firrtl.reg ', ' = firrtl.regreset '), MCR)
    leaf = lambda n: ('leaf', n)
    const = lambda v: ('const', v)
    and2 = lambda a, b: ('and', a, b)
    mux = lambda a, b, c: ('mux', a, b, c)
    fire = lambda ch: and2(leaf('nasti.' + ch + '.ready'), leaf('nasti.' + ch + '.valid'))
    for ch in ('aw', 'w', 'ar'):
        assert nets['nasti.' + ch + '.ready'] == ('not', leaf(ch + 'Fired'))
    for reg, ch, field in [('bId', 'aw', 'id'), ('rId', 'ar', 'id'), ('wData', 'w', 'data')]:
        assert nets[reg] == mux(fire(ch), leaf('nasti.' + ch + '.bits.' + field), leaf(reg))
    for reg, ch in [('wIndex', 'aw'), ('rIndex', 'ar')]:
        assert nets[reg] == mux(fire(ch), ('bits', leaf('nasti.' + ch + '.bits.addr'), 6, 2), leaf(reg))
    write_valid = and2(and2(leaf('awFired'), leaf('wFired')), ('not', leaf('wCommited')))
    write_fire = const(0)
    read_bits, read_valid = None, None
    for word in range(21):
        wr, rd = f'mcr.write[{word}]', f'mcr.read[{word}]'
        sw, sr = ('eq', leaf('wIndex'), const(word)), ('eq', leaf('rIndex'), const(word))
        valid = and2(sw, write_valid)
        assert nets[wr + '.valid'] == valid and nets[wr + '.bits'] == leaf('wData')
        assert nets[rd + '.ready'] == and2(and2(sr, leaf('arFired')), leaf('nasti.r.ready'))
        write_fire = ('or', write_fire, and2(valid, leaf(wr + '.ready')))
        read_bits = mux(sr, leaf(rd + '.bits'), read_bits) if word else leaf(rd + '.bits')
        read_valid = mux(sr, leaf(rd + '.valid'), read_valid) if word else leaf(rd + '.valid')
    assert nets['nasti.r.bits.data'] == read_bits
    assert nets['nasti.r.valid'] == and2(leaf('arFired'), read_valid)
    assert nets['nasti.b.valid'] == and2(and2(leaf('awFired'), leaf('wFired')), leaf('wCommited'))
    assert nets['nasti.r.bits.id'] == leaf('rId') and nets['nasti.b.bits.id'] == leaf('bId')
    for ch in ('r', 'b'):
        assert nets['nasti.' + ch + '.bits.resp'] == const(0)
        assert nets['nasti.' + ch + '.bits.user'] == const(0)
    assert nets['nasti.r.bits.last'] == const(1) and nets['mcr.wstrb'] == const(0)
    for reg, ch, done in [('arFired', 'ar', 'r'), ('awFired', 'aw', 'b'), ('wFired', 'w', 'b')]:
        assert nets[reg] == mux(fire(done), const(0), mux(fire(ch), const(1), leaf(reg)))
    assert nets['wCommited'] == mux(write_fire, const(1), mux(fire('b'), const(0), leaf('wCommited')))
    assert len(checks) == 2
    for ch, (predicate, enabled, _) in zip(('aw', 'ar'), checks):
        assert predicate == ('eq', leaf('nasti.' + ch + '.bits.len'), const(0))
        assert enabled == and2(fire(ch), ('not', leaf('reset')))
    reset = re.findall(r'%(\w+) = firrtl.regreset %clock, %reset, %c0_ui1', body)
    regs = re.findall(r'%(\w+) = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<(\d+)>', body)
    assert reset == ['arFired', 'awFired', 'wFired', 'wCommited']
    assert regs == [('bId', '12'), ('rId', '12'), ('wData', '32'), ('wIndex', '5'), ('rIndex', '5')]


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
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
    control_baseline = (evidence / 'candidate/post-fame-fased-control.mlir').read_text()
    bound_baseline = (evidence / 'candidate/post-fame-fased-bound.mlir').read_text()
    mcr_semantics(control_baseline)
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-mmio{suffix}.mlir').read_text()
        path = evidence / f'binding.mlir.rocket-fased-control{suffix}.mlir'
        text = path.read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {MCR, CONTROL, BOUND}
        assert all(after[n] == body for n, body in prior.items()), 'existing module body changed'
        assert native_module(text, MCR) == native_module(control_baseline, MCR)
        old, controlled, bound = (native_module(text, n) for n in (OLD, CONTROL, BOUND))
        copied = [(d, n) for d, n in ports(old) if n != 'fasedBridge_mcr']
        assert ports(controlled) == copied + [('in', 'fasedBridge_ctrl')]
        attached = wiring(controlled)
        expected = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        expected.update({'crFile.clock': 'top.hostClock', 'crFile.reset': 'top.hostReset',
                         'crFile.nasti': 'top.fasedBridge_ctrl', 'crFile.mcr': 'sim.fasedBridge_mcr'})
        assert attached == expected
        regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', text)[1]
        region = re.search(r'name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64', regions)
        assert region and int(region[1]) == 128
        slave = int(region[2])
        assert f'goldengate.fasedSlave = {slave} : i32' in bound
        bound_ports = ports(bound)
        consumed = {n for _, n in ports(controlled)} - {n for _, n in bound_ports}
        assert len(consumed) == 23
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in bound_ports}
        nets['sim.fasedBridge_ctrl.ar'] = f'sim.ctrl_read_dispatch_slave_{slave}_ar'
        for ch, fields in [('aw', ADDRESS), ('w', DATA)]:
            prefix = f'sim.ctrl_write_dispatch_slave_{slave}_{ch}'
            nets[prefix + '_ready'] = f'sim.fasedBridge_ctrl.{ch}.ready'
            nets[f'sim.fasedBridge_ctrl.{ch}.valid'] = prefix + '_valid'
            for field in fields:
                local = field in (['addr', 'len', 'id'] if ch == 'aw' else ['data', 'last'])
                source = prefix if local else 'sim.ctrl_write_dispatch_master_' + ch
                nets[f'sim.fasedBridge_ctrl.{ch}.bits.{field}'] = source + '_bits_' + field
        for ch, fields in [('r', READ), ('b', RESPONSE)]:
            prefix = f'sim.ctrl_{"read" if ch == "r" else "write"}_arb_in_{slave}'
            nets[f'sim.fasedBridge_ctrl.{ch}.ready'] = prefix + '_ready'
            nets[prefix + '_valid'] = f'sim.fasedBridge_ctrl.{ch}.valid'
            for field in fields:
                nets[prefix + '_bits_' + field] = f'sim.fasedBridge_ctrl.{ch}.bits.{field}'
        assert wiring(bound) == nets
        # Shared baseline also selects by catalog; compare consumed channel
        # nets independently of the additional Print ports on expanded tops.
        shared = wiring(native_module(bound_baseline, BOUND))
        for key, value in nets.items():
            if key.startswith('top.') or value.startswith('top.'): continue
            assert shared.get(key) == value
        pins = re.findall(r'\b(io_ctrl_\w+)\b', host_header)
        assert len(pins) == 20
        for pin in pins:
            ch, field = pin[len('io_ctrl_'):].split('_', 1)
            host_pin, router_pin = 'FASEDMemoryTimingModel_0_' + pin, 'ctrlInterconnect_io_slaves_1_' + ch + '_' + field
            assert platform_eq.get(host_pin) == router_pin or platform_eq.get(router_pin) == host_pin
        assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', controlled + bound)
        header = transfer(before.splitlines()[1], OLD, CONTROL, copied)
        header = transfer(header, CONTROL, BOUND, bound_ports)
        assert text.splitlines()[1] == header, 'annotation archive transfer differs'
        reports.append(dict(reverse=reverse, artifact=str(path), slave=slave, start=int(region[3]), size=128,
                            unchanged_module_bodies=len(prior), attachment_connections=len(expected),
                            binding_connections=len(nets), consumed_ports=len(consumed), sfc_control_pins=20))
    report = dict(golden=str(golden), SFC_modules=['MCRFile_5', 'FASEDMemoryTimingModel', 'NastiRouter', 'FPGATop'],
                  transport_registers=9, reset_flags=4, words=21, local_index_bits='addr[6:2]',
                  orders=reports, strobe='native zero matches Lib.scala; SFC optimizes unused strobe and W.strb capture away',
                  limitation='SFC disables Print; expanded driver/platform and runtime parity remain pending')
    (evidence / 'rocket-fased-control-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS FASED MCRFile state/decode, catalog-selected five-channel binding and 20 SFC control pins in both Print orders')


if __name__ == '__main__': main()
