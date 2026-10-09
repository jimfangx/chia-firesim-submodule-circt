#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Rocket/Print Master wiring with immutable SFC U250 RTL.

The reference disables printf: normalize its Master slave 8 to expanded 10.
Inspect actual native SSA nets and the unchanged imported register bank; no
claim of complete Print platform or driver equivalence follows from this test.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketResponsesCompare import wiring


def native_module(text, name):
    match = re.search(r'^    firrtl.module @' + name + r'\(.*?^    }', text, re.M | re.S)
    assert match, f'missing native {name}'
    return match[0]


def main():
    evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    ref_header, ref_bank = module(reference, 'SimulationMaster')
    ref_nets = equations(module(reference, 'FPGATop')[1])
    bank_nets = equations(ref_bank)
    ref_slots = ['INIT_DONE', 'PRESENCE_READ', 'PRESENCE_WRITE']
    for i, name in enumerate(ref_slots):
        assert bank_nets[f'crFile_io_mcr_read_{i}_bits'] == name
    widths = dict((name, int(msb) + 1) for msb, name in re.findall(r'\breg\s+\[(\d+):0\]\s+(\w+)', ref_bank)
                  if not name.startswith('_RAND_'))
    assert widths == {'initDelay': 7, 'INIT_DONE': 32, 'rFingerprint': 32,
                      'PRESENCE_READ': 32, 'PRESENCE_WRITE': 32}
    resets = dict(re.findall(r'if \(reset\) begin\s*(\w+) <= (\d+\x27h[0-9a-f]+);', ref_bank))
    assert resets == {'initDelay': "7'h40", 'INIT_DONE': "32'h0",
                      'rFingerprint': "32'h46697265", 'PRESENCE_WRITE': "32'h46697265"}
    router = module(reference, 'NastiRouter')[1]
    for channel in ('aw', 'ar'):
        ranges = [(0, 128)] + [(int(lo, 16), int(hi, 16)) for lo, hi in re.findall(
            r"25'h([0-9a-f]+) <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h([0-9a-f]+)", router)]
        assert len(ranges) == 11 and ranges[8] == (544, 560)
    baseline = (evidence / 'candidate/post-fame-simulation-master-bound.mlir').read_text()
    reports = []
    for suffix in ('rocket-master', 'rocket-master-reverse'):
        path = evidence / f'binding.mlir.{suffix}.mlir'
        text = path.read_text()
        master = native_module(text, 'GGSimulationMasterBoundWrapper')
        assert 'goldengate.simulationMasterSlave = 10 : i32' in master
        regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', text)[1]
        assert re.search(r'name = "SimulationMaster_0", size = 16 : i64, slave = 10 : i32, start = 608 : i64', regions)
        bank = native_module(text, 'GGSimulationMasterBank')
        assert bank == native_module(baseline, 'GGSimulationMasterBank'), 'imported Master bank operations changed'
        native_widths, native_resets = {}, {}
        constants = dict((value, (int(width), int(number))) for value, number, width in re.findall(
            r'(%\w+) = firrtl.constant (\d+) : !firrtl.uint<(\d+)>', bank))
        for line in bank.splitlines():
            if match := re.search(r'%(\w+) = firrtl.reg(reset)? ', line):
                name, reset = match.groups()
                native_widths[name] = int(re.findall(r'!firrtl.uint<(\d+)>', line)[-1])
                if reset:
                    value = re.search(r'firrtl.regreset %clock, %reset, (%\w+)', line)[1]
                    native_resets[name] = constants[value]
        assert native_widths == widths
        assert native_resets == {name: (int(value.split("'h")[0]), int(value.split("'h")[1], 16))
                                 for name, value in resets.items()}
        regs = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = true, writeable = true\}', bank)
        assert regs == list(zip(ref_slots, ['0', '4', '8']))
        read_roots = set(re.findall(r'(%\w+) = firrtl.subfield %mcr\[read\]', bank))
        for i, name in enumerate(ref_slots):
            slots = [slot for slot, root in re.findall(
                r'(%\w+) = firrtl.subindex (%\w+)\[' + str(i) + r'\]', bank) if root in read_roots]
            assert len(slots) == 1, f'Master read word {i} is missing or duplicated'
            slot = slots[0]
            bits = re.search(r'(%\w+) = firrtl.subfield ' + slot + r'\[bits\]', bank)[1]
            assert re.search(r'firrtl.strictconnect ' + bits + ', %' + name + r'\b', bank)
        # Compare every SFC surviving control pin to actual native binding nets.
        # Other AXI fields remain unspecialized in native IR and are checked by
        # the C++ fixture's complete 32-field contract.
        nets = wiring(text, 'GGSimulationMasterBoundWrapper', aggregate=True)
        assert nets['sim.simulationMaster_ctrl.ar'] == 'sim.ctrl_read_dispatch_slave_10_ar'
        scalar = wiring(text, 'GGSimulationMasterBoundWrapper')
        assert len(scalar) == 32
        compared = []
        for pin in re.findall(r'\b(io_ctrl_\w+)\b', ref_header):
            channel, leaf = pin[len('io_ctrl_'):].split('_', 1)
            mp = 'SimulationMaster_0_' + pin
            rp = 'ctrlInterconnect_io_slaves_8_' + channel + '_' + leaf
            native_pin = 'sim.simulationMaster_ctrl.' + channel + '.' + leaf.replace('bits_', 'bits.')
            if channel == 'ar':
                assert ref_nets.get(mp) == rp or ref_nets.get(rp) == mp
            elif channel in ('aw', 'w'):
                boundary = 'sim.ctrl_write_dispatch_slave_10_' + channel + '_' + leaf
                if leaf == 'ready':
                    assert ref_nets[rp] == mp and scalar[boundary] == native_pin
                else:
                    assert ref_nets[mp] == rp and scalar[native_pin] == boundary
            else:
                boundary = 'sim.ctrl_' + ('read' if channel == 'r' else 'write') + '_arb_in_10_' + leaf
                if leaf == 'ready':
                    assert ref_nets[mp] == rp and scalar[native_pin] == boundary
                else:
                    assert ref_nets[rp] == mp and scalar[boundary] == native_pin
            compared.append(pin)
        assert len(compared) == 20
        attached = wiring(text, 'GGSimulationMasterWrapper', aggregate=True)
        assert attached['simulationMaster.clock'] == 'top.hostClock' and attached['simulationMaster.reset'] == 'top.hostReset'
        assert ref_nets['SimulationMaster_0_clock'] == 'clock' and ref_nets['SimulationMaster_0_reset'] == 'reset'
        for name in ref_slots:
            assert f'%{name} = firrtl.reg' in bank
        reports.append({'artifact': str(path), 'slave': 10, 'register_addresses': [608, 612, 616],
                        'scalar_bindings': len(scalar), 'sfc_pins_compared': compared,
                        'bank_operations_unchanged': True, 'host_clock_reset': True})
    result = {'golden': str(golden), 'sfc_slave': 8, 'sfc_register_addresses': [544, 548, 552],
              'orders': reports, 'limitation': 'SFC disables Print; full expanded platform/driver assembly remains pending'}
    (evidence / 'rocket-master-comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS Rocket/Print SimulationMaster: both orders, 32 scalar + AR bindings, 20 SFC pins, three shifted registers, host clock/reset')


if __name__ == '__main__':
    main()
