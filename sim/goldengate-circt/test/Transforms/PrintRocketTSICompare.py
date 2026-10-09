#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual expanded Rocket/Print TSI operations and nets to SFC U250."""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring


def main():
    evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    header, bank = module(reference, 'TSIBridgeModule')
    ref_nets = equations(module(reference, 'FPGATop')[1])
    bank_nets = equations(bank)
    names = ['in_bits', 'in_valid', 'in_ready', 'out_bits', 'out_valid',
             'out_ready', 'step_size', 'done', 'start']
    widths = dict((name.replace('start_1', 'start'), int(msb or 0) + 1)
                  for msb, name in re.findall(r'\breg(?:\s+\[(\d+):0\])?\s+(\w+)', bank)
                  if not name.startswith('_RAND_'))
    assert bank_nets['tFire'] == "hPort_toHost_hValid & hPort_fromHost_hReady & tokensToEnqueue != 32'h0"
    assert bank_nets['inBuf_reset'] == bank_nets['outBuf_reset'] == 'reset | targetReset'
    for i, name in enumerate(names):
        ref = 'start_1' if name == 'start' else name
        value = bank_nets[f'crFile_io_mcr_read_{i}_bits']
        assert value == (ref if widths[name] == 32 else "{{31'd0}, " + ref + '}')
    router = module(reference, 'NastiRouter')[1]
    ref_ranges = {}
    for channel in ('aw', 'ar'):
        ranges = [(0, 128)] + [(int(lo, 16), int(hi, 16)) for lo, hi in re.findall(
            r"25'h([0-9a-f]+) <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h([0-9a-f]+)", router)]
        assert len(ranges) == 11
        ref_ranges[channel] = ranges[3]
    assert ref_ranges['aw'] == ref_ranges['ar']
    baseline = (evidence / 'candidate/post-fame-tsi-bound.mlir').read_text()
    reports = []
    for suffix in ('rocket-tsi', 'rocket-tsi-reverse'):
        path = evidence / f'binding.mlir.{suffix}.mlir'
        text = path.read_text()
        bound = native_module(text, 'GGTSIBridgeBoundWrapper')
        assert 'goldengate.tsiSlave = 3 : i32' in bound
        regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', text)[1]
        region = re.search(r'name = "TSIBridgeModule_0", size = (\d+) : i64, slave = 3 : i32, start = (\d+) : i64', regions)
        assert region and (int(region[2]), int(region[2]) + int(region[1])) == ref_ranges['aw']
        expected_widths = dict(widths)
        actual_widths = {}
        for symbol in ('GGTSIMMIOBank', 'GGTSITokenEngine', 'GGTSIWordQueue16', 'GGTSIMCRFile'):
            native = native_module(text, symbol)
            assert native == native_module(baseline, symbol), f'{symbol} operations changed'
            if symbol in ('GGTSIMMIOBank', 'GGTSITokenEngine'):
                for line in native.splitlines():
                    if match := re.search(r'%(\w+) = firrtl.reg(?:reset)? ', line):
                        actual_widths[match[1]] = int(re.findall(r'!firrtl.uint<(\d+)>', line)[-1])
        assert actual_widths == expected_widths
        native = native_module(text, 'GGTSIMMIOBank')
        registry = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = true, writeable = true\}', native)
        assert registry == list(zip(names, [str(i * 4) for i in range(9)]))
        read_roots = set(re.findall(r'(%\w+) = firrtl.subfield %mcr\[read\]', native))
        for i, name in enumerate(names):
            slots = [slot for slot, root in re.findall(
                r'(%\w+) = firrtl.subindex (%\w+)\[' + str(i) + r'\]', native) if root in read_roots]
            assert len(slots) == 1
            bits = re.search(r'(%\w+) = firrtl.subfield ' + slots[0] + r'\[bits\]', native)[1]
            padded = re.search(r'(%\w+) = firrtl.pad %' + name + r', 32\b', native)[1]
            assert re.search(r'firrtl.strictconnect ' + bits + ', ' + padded + r'\b', native)
        nets = wiring(text, 'GGTSIBridgeBoundWrapper', aggregate=True)
        scalar = wiring(text, 'GGTSIBridgeBoundWrapper')
        assert len(scalar) == 32 and nets['sim.tsiBridge_ctrl.ar'] == 'sim.ctrl_read_dispatch_slave_3_ar'
        pins = []
        for pin in re.findall(r'\b(io_ctrl_\w+)\b', header):
            ch, leaf = pin[len('io_ctrl_'):].split('_', 1)
            mp = 'TSIBridgeModule_0_' + pin
            rp = 'ctrlInterconnect_io_slaves_3_' + ch + '_' + leaf
            np = 'sim.tsiBridge_ctrl.' + ch + '.' + leaf.replace('bits_', 'bits.')
            if ch == 'ar':
                assert ref_nets.get(mp) == rp or ref_nets.get(rp) == mp
            elif ch in ('aw', 'w'):
                bp = 'sim.ctrl_write_dispatch_slave_3_' + ch + '_' + leaf
                if leaf == 'ready': assert ref_nets[rp] == mp and scalar[bp] == np
                else: assert ref_nets[mp] == rp and scalar[np] == bp
            else:
                bp = 'sim.ctrl_' + ('read' if ch == 'r' else 'write') + '_arb_in_3_' + leaf
                if leaf == 'ready': assert ref_nets[mp] == rp and scalar[np] == bp
                else: assert ref_nets[rp] == mp and scalar[bp] == np
            pins.append(pin)
        assert len(pins) == 20
        attached = wiring(text, 'GGTSIMMIOWrapper', aggregate=True)
        for dest, src in {'tsiRegisters.clock': 'top.hostClock', 'tsiRegisters.reset': 'top.hostReset',
                          'sim.tsi_in_enq': 'tsiRegisters.inBuf', 'tsiRegisters.outBuf': 'sim.tsi_out_deq',
                          'tsiRegisters.control': 'sim.tsi_control'}.items(): assert attached[dest] == src
        queue = wiring(text, 'GGTSIWordQueuesWrapper', aggregate=True)
        for inst in ('inBuf', 'outBuf'):
            assert queue[inst + '.clock'] == 'top.hostClock'
            assert queue[inst + '.reset'] == 'sim.tsi_queue_reset'
        assert queue['sim.tsi_in_deq'] == 'inBuf.deq' and queue['outBuf.enq'] == 'sim.tsi_out_enq'
        assert ref_nets['TSIBridgeModule_0_clock'] == 'clock' and ref_nets['TSIBridgeModule_0_reset'] == 'reset'
        reports.append({'artifact': str(path), 'slave': 3, 'base': int(region[2]), 'size': int(region[1]),
                        'registers': names, 'scalar_bindings': 32, 'sfc_pins': pins,
                        'unchanged_bank_scheduler_queue_adapter_operations': True})
    result = {'golden': str(golden), 'orders': reports,
              'limitation': 'SFC disables Print; expanded platform/driver and FPGA execution remain pending'}
    (evidence / 'rocket-tsi-comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS Rocket/Print TSI: both orders, nine words, live scheduler/two queues and 20 SFC control pins')


if __name__ == '__main__':
    main()
