#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual expanded Rocket/Print BlockDev operations and nets to SFC U250."""
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
    header, bank = module(reference, 'BlockDevBridgeModule')
    ref_nets = equations(module(reference, 'FPGATop')[1])
    bank_nets = equations(bank)
    names = ['read_latency', 'write_latency', 'bdev_nsectors', 'bdev_max_req_len',
             'bdev_req_valid', 'bdev_req_write', 'bdev_req_offset', 'bdev_req_len', 'bdev_req_tag', 'bdev_req_ready',
             'bdev_data_valid', 'bdev_data_data_upper', 'bdev_data_data_lower', 'bdev_data_tag', 'bdev_data_ready',
             'bdev_rresp_data_upper', 'bdev_rresp_data_lower', 'bdev_rresp_tag', 'bdev_rresp_valid', 'bdev_rresp_ready',
             'bdev_wack_tag', 'bdev_wack_valid', 'bdev_wack_ready', 'bdev_reqs_pending', 'bdev_wack_stalled', 'bdev_rresp_stalled']
    bank_names = ['nsectorReg' if n == 'bdev_nsectors' else 'max_req_lenReg' if n == 'bdev_max_req_len' else n for n in names]
    widths = dict((name, int(msb or 0) + 1)
                  for msb, name in re.findall(r'\breg(?:\s+\[(\d+):0\])?\s+(\w+)', bank)
                  if not name.startswith('_RAND_'))
    assert re.sub(r'\s+', ' ', bank_nets['tFire']) == ('hPort_toHost_hValid & hPort_fromHost_hReady & '
        'reqBuf_io_enq_ready & dataBuf_io_enq_ready & rRespStallN & wAckStallN')
    assert bank_nets['targetReset'] == '_tFire_T & hPort_hBits_reset'
    for instance in ('reqBuf', 'dataBuf', 'rRespBuf', 'wAckBuf'):
        assert bank_nets[instance + '_reset'] == 'reset | targetReset'
    for i, name in enumerate(bank_names):
        if i in (2, 3):
            assert f'crFile_io_mcr_read_{i}_bits' not in bank_nets
            continue
        value = bank_nets[f'crFile_io_mcr_read_{i}_bits']
        assert value == (name if widths[name] == 32 else "{{" + str(32 - widths[name]) + "'d0}, " + name + '}')
    queues = [('Queue_1', 'reqBuf', 10), ('Queue_2', 'dataBuf', 32),
              ('Queue_2', 'rRespBuf', 32), ('Queue_4', 'wAckBuf', 4)]
    queue_shapes = {}
    for symbol, instance, depth in queues:
        assert re.search(r'\b' + symbol + r'\s+' + instance + r'\b', bank)
        queue = module(reference, symbol)[1]
        memories = re.findall(r'\breg\s+(?:\[(\d+):0\]\s+)?ram(?:_\w+)?\s*\[0:(\d+)\]', queue)
        assert memories and all(int(last) + 1 == depth for _, last in memories)
        queue_shapes[instance] = (depth, sum(int(msb or 0) + 1 for msb, _ in memories))
    router = module(reference, 'NastiRouter')[1]
    ref_ranges = {}
    for channel in ('aw', 'ar'):
        ranges = [(0, 128)] + [(int(lo, 16), int(hi, 16)) for lo, hi in re.findall(
            r"25'h([0-9a-f]+) <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h([0-9a-f]+)", router)]
        assert len(ranges) == 11
        ref_ranges[channel] = ranges[0]
    assert ref_ranges['aw'] == ref_ranges['ar']
    baseline = (evidence / 'candidate/post-fame-blockdev-response-scheduler.mlir').read_text()
    reports = []
    for suffix in ('rocket-blockdev', 'rocket-blockdev-reverse'):
        path = evidence / f'binding.mlir.{suffix}.mlir'
        text = path.read_text()
        bound = native_module(text, 'GGBlockDevBridgeBoundWrapper')
        assert 'goldengate.blockdevSlave = 0 : i32' in bound
        regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', text)[1]
        region = re.search(r'name = "BlockDevBridgeModule_0", size = (\d+) : i64, slave = 0 : i32, start = (\d+) : i64', regions)
        assert region and (int(region[2]), int(region[2]) + int(region[1])) == ref_ranges['aw']
        expected_widths = dict(widths)
        actual_widths = {}
        for symbol in ('GGBlockDevMMIOBank', 'GGBlockDevTokenEngine', 'GGBlockDevRequestQueue10',
                       'GGBlockDevDataQueue32', 'GGBlockDevWriteAckQueue4', 'GGBlockDevMCRFile',
                       'GGBlockDevWriteLatencyPipe', 'GGBlockDevReadLatencyPipe', 'GGBlockDevResponseScheduler'):
            native = native_module(text, symbol)
            assert native == native_module(baseline, symbol), f'{symbol} operations changed'
            if symbol in ('GGBlockDevMMIOBank', 'GGBlockDevTokenEngine', 'GGBlockDevResponseScheduler'):
                for line in native.splitlines():
                    if match := re.search(r'%(\w+) = firrtl.reg(?:reset)? ', line):
                        actual_widths[match[1]] = int(re.findall(r'!firrtl.uint<(\d+)>', line)[-1])
        actual_widths = {n.replace("returnWrite_0", "returnWrite"): w for n, w in actual_widths.items()}
        assert actual_widths == expected_widths, (actual_widths, expected_widths)
        for instance, symbol in [('reqBuf', 'GGBlockDevRequestQueue10'), ('dataBuf', 'GGBlockDevDataQueue32'),
                                 ('rRespBuf', 'GGBlockDevDataQueue32'), ('wAckBuf', 'GGBlockDevWriteAckQueue4')]:
            queue = native_module(text, symbol)
            depth, width = queue_shapes[instance]
            assert f'depth = {depth} : i64' in queue and f'data flip: uint<{width}>' in queue
            assert 'readLatency = 0 : i32, writeLatency = 1 : i32' in queue
        native = native_module(text, 'GGBlockDevMMIOBank')
        registry = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = (true|false), writeable = true\}', native)
        assert registry == [(n, str(i * 4), 'false' if i in (2, 3) else 'true') for i, n in enumerate(names)]
        read_roots = set(re.findall(r'(%\w+) = firrtl.subfield %mcr\[read\]', native))
        for i, name in enumerate(bank_names):
            if i in (2, 3): continue
            slots = [slot for slot, root in re.findall(
                r'(%\w+) = firrtl.subindex (%\w+)\[' + str(i) + r'\]', native) if root in read_roots]
            assert len(slots) == 1
            bits = re.search(r'(%\w+) = firrtl.subfield ' + slots[0] + r'\[bits\]', native)[1]
            padded = re.search(r'(%\w+) = firrtl.pad %' + name + r', 32\b', native)[1]
            assert re.search(r'firrtl.strictconnect ' + bits + ', ' + padded + r'\b', native)
        nets = wiring(text, 'GGBlockDevBridgeBoundWrapper', aggregate=True)
        scalar = wiring(text, 'GGBlockDevBridgeBoundWrapper')
        assert len(scalar) == 32 and nets['sim.blockdevBridge_ctrl.ar'] == 'sim.ctrl_read_dispatch_slave_0_ar'
        pins = []
        for pin in re.findall(r'\b(io_ctrl_\w+)\b', header):
            ch, leaf = pin[len('io_ctrl_'):].split('_', 1)
            mp = 'BlockDevBridgeModule_0_' + pin
            rp = 'ctrlInterconnect_io_slaves_0_' + ch + '_' + leaf
            np = 'sim.blockdevBridge_ctrl.' + ch + '.' + leaf.replace('bits_', 'bits.')
            if ch == 'ar':
                assert ref_nets.get(mp) == rp or ref_nets.get(rp) == mp
            elif ch in ('aw', 'w'):
                bp = 'sim.ctrl_write_dispatch_slave_0_' + ch + '_' + leaf
                if leaf == 'ready': assert ref_nets[rp] == mp and scalar[bp] == np
                else: assert ref_nets[mp] == rp and scalar[np] == bp
            else:
                bp = 'sim.ctrl_' + ('read' if ch == 'r' else 'write') + '_arb_in_0_' + leaf
                if leaf == 'ready': assert ref_nets[mp] == rp and scalar[np] == bp
                else: assert ref_nets[rp] == mp and scalar[bp] == np
            pins.append(pin)
        assert len(pins) == 20
        attached = wiring(text, 'GGBlockDevMMIOWrapper', aggregate=True)
        for dest, src in {'blockdevRegisters.clock': 'top.hostClock', 'blockdevRegisters.reset': 'top.hostReset',
                          'blockdevRegisters.reqBuf': 'sim.blockdev_req_deq', 'blockdevRegisters.dataBuf': 'sim.blockdev_data_deq',
                          'sim.blockdev_rresp_enq': 'blockdevRegisters.rRespBuf', 'sim.blockdev_wack_enq': 'blockdevRegisters.wAckBuf',
                          'sim.blockdev_info': 'blockdevRegisters.info', 'top.blockdev_latency': 'blockdevRegisters.latency'}.items():
            assert attached[dest] == src, (dest, attached.get(dest))
        for wrapper, instance in [('GGBlockDevRequestQueueWrapper', 'reqBuf'), ('GGBlockDevDataQueueWrapper', 'dataBuf'),
                                  ('GGBlockDevReadResponseQueueWrapper', 'rRespBuf'), ('GGBlockDevWriteAckQueueWrapper', 'wAckBuf')]:
            queue = wiring(text, wrapper, aggregate=True)
            assert queue[instance + '.clock'] == 'top.hostClock'
            assert queue[instance + '.reset'] == 'sim.blockdev_queue_reset'
        scheduler = wiring(text, 'GGBlockDevResponseSchedulerWrapper', aggregate=True)
        timing_nets = {'responseScheduler.clock': 'top.hostClock',
                       'responseScheduler.reset': 'sim.blockdev_queue_reset',
                       'responseScheduler.tFire': 'sim.blockdev_tfire',
                       'responseScheduler.resp_ready': 'sim.blockdev_resp_ready',
                       'responseScheduler.write_valid': 'sim.blockdev_write_latency_deq.valid',
                       'responseScheduler.read_valid': 'sim.blockdev_read_latency_deq.valid',
                       'responseScheduler.read_bits': 'sim.blockdev_read_latency_deq.bits',
                       'sim.blockdev_write_latency_deq.ready': 'responseScheduler.write_ready',
                       'sim.blockdev_read_latency_deq.ready': 'responseScheduler.read_ready',
                       'sim.blockdev_timing.returnWrite': 'responseScheduler.returnWrite',
                       'sim.blockdev_timing.readRespBusy': 'responseScheduler.readRespBusy'}
        for dest, src in timing_nets.items(): assert scheduler[dest] == src
        for kind in ('Read', 'Write'):
            timing = wiring(text, 'GGBlockDev' + kind + 'LatencyWrapper', aggregate=True)
            instance = kind.lower() + 'LatencyPipe'
            for dest, src in {instance + '.clock': 'top.hostClock',
                              instance + '.reset': 'sim.blockdev_queue_reset',
                              instance + '.tCycle': 'sim.blockdev_timing.tCycle',
                              instance + '.latency': 'sim.blockdev_latency.' + kind.lower() + '_latency'}.items():
                assert timing[dest] == src
        assert ref_nets['BlockDevBridgeModule_0_clock'] == 'clock' and ref_nets['BlockDevBridgeModule_0_reset'] == 'reset'
        reports.append({'artifact': str(path), 'slave': 0, 'base': int(region[2]), 'size': int(region[1]),
                        'registers': names, 'queues_depth_payload_bits': queue_shapes, 'scalar_bindings': 32, 'sfc_pins': pins,
                        'unchanged_bank_timing_scheduler_queue_adapter_operations': True})
    result = {'golden': str(golden), 'orders': reports,
              'limitation': 'SFC disables Print; expanded platform/driver and FPGA execution remain pending'}
    (evidence / 'rocket-blockdev-comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS Rocket/Print BlockDev: both orders, 26 words, nine channels, four queues/timing scheduler and 20 SFC control pins')


if __name__ == '__main__':
    main()
