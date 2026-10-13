#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print read latency with immutable SFC Queue_16.

Interpret actual CIRCT queue SSA against the pinned SFC flow/deadline equations,
including target-qualified reset, non-reset RAM writes, and the Scala len field
optimized away in SFC RTL. Retain prior modules, annotations and allocated
banks in both layouts.
"""
import itertools
import json
import random
import re
import sys
from collections import deque
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintRocketFASEDIssueCompare import expressions, evaluate
from PrintSelectedFASEDTimingCycleCompare import transfer
from PrintRocketControlMasterCompare import native_ports


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    queue_body = module(reference, 'Queue_16')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(queue_body).items()}
    expected = {'ptr_match': 'enq_ptr_value == deq_ptr_value', 'empty': 'ptr_match & ~maybe_full',
                'full': 'ptr_match & maybe_full', 'io_enq_ready': '~full',
                'io_deq_valid': 'io_enq_valid | ~empty', '_do_enq_T': 'io_enq_ready & io_enq_valid',
                '_do_deq_T': 'io_deq_ready & io_deq_valid', '_GEN_16': "io_deq_ready ? 1'h0 : _do_enq_T",
                'do_enq': 'empty ? _GEN_16 : _do_enq_T', 'do_deq': "empty ? 1'h0 : _do_deq_T",
                'wrap': "enq_ptr_value == 4'h9", 'wrap_1': "deq_ptr_value == 4'h9",
                '_value_T_1': "enq_ptr_value + 4'h1", '_value_T_3': "deq_ptr_value + 4'h1"}
    for name, expression in expected.items(): assert eq[name] == expression, name
    for payload, width, port in [('releaseCycle', 64, 'releaseCycle'), ('xaction_id', 4, 'xaction_id')]:
        assert re.search(r'reg \[' + str(width - 1) + r':0\] ram_' + payload + r' \[0:9\];', queue_body)
        assert eq['io_deq_bits_' + payload] == 'empty ? io_enq_bits_' + port + ' : ram_' + payload + '_io_deq_bits_MPORT_data'
        for signal, source in [('en', 'do_enq'), ('mask', "1'h1"), ('addr', 'enq_ptr_value'), ('data', 'io_enq_bits_' + port)]:
            # SFC inlines the do_enq expression for the RAM enable.
            assert eq['ram_' + payload + '_MPORT_' + signal] == (expected['do_enq'] if signal == 'en' else source)
        assert re.search(r'if \(ram_' + payload + r'_MPORT_en & ram_' + payload + r'_MPORT_mask\) begin\s*'
                         r'ram_' + payload + r'\[ram_' + payload + r'_MPORT_addr\] <= ram_' + payload + r'_MPORT_data;', queue_body)
    for name, width, advance in [('enq_ptr_value', 4, 'do_enq'), ('deq_ptr_value', 4, 'do_deq')]:
        assert re.search(r'if \(reset\) begin\s*' + name + r" <= 4'h0;\s*end else if \(" + advance + r'\)', queue_body)
    assert re.search(r"if \(reset\) begin\s*maybe_full <= 1'h0;\s*end else if \(do_enq != do_deq\)", queue_body)
    pipe_body = module(reference, 'LatencyPipe')[1]
    assert re.search(r'Queue_16 readPipe \(', pipe_body)
    assert re.search(r'if \(_T_16 & ~\(readPipe_io_enq_ready \| ~_readPipe_io_enq_valid_T\)\)', pipe_body)
    pipe = equations(pipe_body)
    assert pipe['_T_16'] == '~reset'
    assert pipe['readDone'] == 'readPipe_io_deq_bits_releaseCycle <= tCycle'
    assert pipe['readPipe_io_enq_valid'] == 'nastiReqIden_io_out_ar_ready & nastiReqIden_io_out_ar_valid'
    assert pipe['readPipe_io_enq_bits_xaction_id'] == 'nastiReqIden_io_out_ar_bits_id'
    assert pipe['xactionRelease_io_nextRead_valid'] == 'readPipe_io_deq_valid & readDone'
    assert pipe['readPipe_io_deq_ready'] == 'rResp_ready & readDone'
    assert pipe['readPipe_clock'] == 'clock' and pipe['readPipe_reset'] == 'reset'
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['model_clock'] == 'gate_O' and host['gate_I'] == 'clock'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    assert host['model_reset'] == 'hPort_hBits_reset'
    baseline = baseline_path.read_text()
    reports = []
    mask = (1 << 64) - 1
    pack = lambda item: (item[0] << 12) | (item[1] << 8) | item[2]
    for allocation in ('candidate', 'compatibility'):
        output = evidence / allocation
        text = (output / 'post-print-fased-read-latency.mlir').read_text()
        before = (output / 'post-print-fased-timing-cycle.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDReadLatency10', 'GGSelectedPrintFASEDReadLatencyWrapper'}
        old, new = 'GGSelectedPrintFASEDTimingCycleWrapper', 'GGSelectedPrintFASEDReadLatencyWrapper'
        consumed = {'fased_next_read', 'fased_read_release_cycle'}
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        previous_annotations = json.loads((output / 'post-print-fased-timing-cycle-all.json').read_text())
        annotations = json.loads((output / 'post-print-fased-read-latency-all.json').read_text())
        copied = {n for _, n in old_ports} - consumed
        assert annotations == transfer(previous_annotations, old, new, copied), 'annotation identity differs'
        helper = native_module(text, 'GGFASEDReadLatency10')
        assert helper == native_module(baseline, 'GGFASEDReadLatency10')
        memories = [line for line in helper.splitlines() if ' = firrtl.mem ' in line]
        assert len(memories) == 1 and all(term in memories[0] for term in
            ['depth = 10 : i64', 'readLatency = 0 : i32', 'writeLatency = 1 : i32', 'uint<76>'])
        assert len(re.findall(' = firrtl.regreset ', helper)) == 3 and ' = firrtl.reg ' not in helper
        for name, width in [('enq_ptr_value', 4), ('deq_ptr_value', 4), ('maybe_full', 1)]:
            reg = re.search('%' + name + r' = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
            assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<' + str(width) + '>', helper)
        nets, assertions = expressions(helper, 'GGFASEDReadLatency10')
        assert len(assertions) == 1 and assertions[0][2] == 'LBP read latency pipe would overflow.'
        assert nets['ram_read.clk'] == nets['ram_write.clk'] == ('leaf', 'clock')
        counts = dict(transitions=0, flow=0, full_pops=0, stalls=0, stalled_resets=0,
                      reset_writes=0, pointer_wraps=0, overflow=0, deadline_equal=0)

        def sample(ep, dp, mf, memory, entries, reset, fire, cycle, ev, ready, incoming):
            empty, full = not entries, len(entries) == 10
            head = incoming if empty else entries[0]
            due = head[0] <= cycle
            valid = (not empty or ev) and due
            push = not full and ev and not (empty and ready and due)
            pop = not empty and valid and ready
            inputs = dict(clock=1, reset=reset, targetFire=fire, tCycle=cycle, enq_ptr_value=ep,
                          deq_ptr_value=dp, maybe_full=mf)
            inputs.update({'enq.valid': ev, 'deq.ready': ready, 'enq.bits.releaseCycle': incoming[0],
                           'enq.bits.id': incoming[1], 'enq.bits.len': incoming[2], 'ram_read.data': memory[dp]})
            value = lambda name: evaluate(nets[name], inputs, nets)
            assert value('enq.ready') == (not full)
            assert value('deq.valid') == valid
            assert value('deq.bits.id') == head[1] and value('deq.bits.len') == head[2]
            assert value('ram_read.en') == value('ram_write.mask') == 1
            assert value('ram_read.addr') == dp and value('ram_write.addr') == ep
            assert value('ram_write.en') == (fire and push)
            assert value('ram_write.data') == pack(incoming)
            assert evaluate(assertions[0][0], inputs, nets) == (not full or not ev)
            assert evaluate(assertions[0][1], inputs, nets) == (fire and not reset)
            next_ep = 0 if fire and reset else (ep + 1) % 10 if fire and push else ep
            next_dp = 0 if fire and reset else (dp + 1) % 10 if fire and pop else dp
            next_mf = False if fire and reset else push if fire and push != pop else mf
            for name, expected_value in [('enq_ptr_value', next_ep), ('deq_ptr_value', next_dp), ('maybe_full', next_mf)]:
                assert (0 if fire and reset else value(name)) == expected_value
            if fire and push: memory[ep] = value('ram_write.data')  # RAM writes survive reset.
            if fire:
                if reset: entries.clear()
                else:
                    if pop: entries.popleft()
                    if push: entries.append(incoming)
            counts['transitions'] += 1
            counts['flow'] += fire and empty and ev and ready and due
            counts['full_pops'] += fire and full and pop
            counts['stalls'] += not fire
            counts['stalled_resets'] += reset and not fire
            counts['reset_writes'] += reset and fire and push
            counts['pointer_wraps'] += fire and not reset and ((push and ep == 9) or (pop and dp == 9))
            counts['overflow'] += fire and not reset and full and ev
            counts['deadline_equal'] += head[0] == cycle
            return next_ep, next_dp, next_mf

        pairs = [(0, 0), (0, 1), (1, 0), (mask, mask), (mask, 0), (0, mask),
                 (1 << 63, (1 << 63) - 1), ((1 << 63) - 1, 1 << 63), (42, 42), (42, 41)]
        for dp, occupancy, flags, (deadline, cycle) in itertools.product(range(10), range(11), range(16), pairs):
            entries = deque((deadline, i % 16, i * 23 % 256) for i in range(occupancy))
            memory = [0] * 10
            for i, item in enumerate(entries): memory[(dp + i) % 10] = pack(item)
            sample((dp + occupancy) % 10, dp, occupancy == 10, memory, entries,
                   flags & 1, (flags >> 1) & 1, cycle, (flags >> 2) & 1, (flags >> 3) & 1, (deadline, 15, 255))
        rng = random.Random(221)
        ep = dp = 0; mf = False; memory = [0] * 10; entries = deque()
        for i in range(30000):
            cycle = mask if i % 17 == 0 else i
            deadline = mask if i % 11 == 0 else 0 if i % 13 == 0 else (cycle + rng.randrange(8) - 3) & mask
            ep, dp, mf = sample(ep, dp, mf, memory, entries, i % 997 == 0 or rng.randrange(113) == 0,
                               rng.randrange(3) != 0, cycle, rng.randrange(4) != 0, rng.randrange(2),
                               (deadline, rng.randrange(16), rng.randrange(256)))
        assert all(counts.values()), 'missing queue coverage'
        connections = {'readLatency.clock': 'top.hostClock', 'readLatency.reset': 'sim.fased_model_reset',
                       'readLatency.targetFire': 'sim.fased_tfire', 'readLatency.tCycle': 'sim.fased_timing_cycle',
                       'readLatency.enq.valid': frozenset(['sim.fased_timing_requests.ar.ready', 'sim.fased_timing_requests.ar.valid']),
                       'readLatency.enq.bits.releaseCycle': 'sim.fased_read_release_cycle',
                       'readLatency.enq.bits.id': 'sim.fased_timing_requests.ar.bits.id',
                       'readLatency.enq.bits.len': 'sim.fased_timing_requests.ar.bits.len',
                       'sim.fased_next_read': 'readLatency.deq'}
        for direction, name in old_ports:
            if name not in consumed:
                connections[('sim.' if direction == 'in' else 'top.') + name] = ('top.' if direction == 'in' else 'sim.') + name
        assert wiring(text, new, aggregate=True) == connections, 'read acceptance/completion wrapper differs'
        top_ports = re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0])
        assert top_ports == [(d, n) for d, n in old_ports if n not in consumed]
        assert native_ports(native_module(text, new)) == [p for p in native_ports(native_module(before, old))
                                                        if p[1] not in consumed], 'port metadata differs'
        decoder = native_module(text, 'GGControlAddressDecode')
        print_slave, print_start, count_size = (1, 64, 8) if allocation == 'candidate' else (0, 0, 4)
        assert f'name = "PrintBridgeModule_0", size = 32 : i64, slave = {print_slave} : i32, start = {print_start} : i64' in decoder
        assert f'name = "CPUManagedStreamEngine_0", size = {count_size} : i64, slave = {print_slave + 1} : i32, start = {print_start + 32} : i64' in decoder
        rtl = clean((output / 'post-print-fased-read-latency.sv').read_text())
        queue_rtl = module(rtl, 'GGFASEDReadLatency10')[1]
        # CIRCT lowers the FIRRTL MemOp into a separate memory module.
        memory_rtl = module(rtl, 'ram_10x76')[1]
        assert re.search(r'reg \[75:0\] Memory\[0:9\];', memory_rtl), 'emitted RAM geometry differs'
        assert re.search(r'always @\(posedge W0_clk\) begin\s*if \(W0_en & 1.h1\)\s*'
                         r'Memory\[W0_addr\] <= W0_data;', memory_rtl)
        assert equations(memory_rtl)['R0_data'] == "R0_en ? Memory[R0_addr] : 76'bx"
        rtl_eq = equations(queue_rtl)
        assert rtl_eq['_GEN_3'] == '_GEN_2 <= tCycle'
        assert rtl_eq['_GEN_5'] == 'targetFire & _GEN_4'
        memory_instance = re.search(r'ram_10x76 ram_ext \((.*?)\);', queue_rtl, re.S)
        assert memory_instance
        memory_ports = {n: ' '.join(v.split()) for n, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', memory_instance[1])}
        assert memory_ports == {'R0_addr': 'deq_ptr_value', 'R0_en': "1'h1", 'R0_clk': 'clock',
                                'R0_data': '_ram_ext_R0_data', 'W0_addr': 'enq_ptr_value',
                                'W0_en': '_GEN_5', 'W0_clk': 'clock',
                                'W0_data': '{enq_bits_releaseCycle, enq_bits_id, enq_bits_len}'}
        assert 'enq_bits_releaseCycle' in queue_rtl and 'deq_bits_len' in queue_rtl
        assert re.search(r'if \(reset & targetFire\)', queue_rtl), 'emitted reset qualification differs'
        assert 'LBP read latency pipe would overflow.' in queue_rtl
        top_body = module(rtl, new)[1]
        queue_instance = re.search(r'GGFASEDReadLatency10 readLatency \((.*?)\);', top_body, re.S)
        assert queue_instance
        queue_ports = {n: ' '.join(v.split()) for n, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', queue_instance[1])}
        sim_instance = re.search(r'GGSelectedPrintFASEDTimingCycleWrapper sim \((.*?)\);', top_body, re.S)
        assert sim_instance
        sim_ports = {n: ' '.join(v.split()) for n, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', sim_instance[1])}
        expected_ports = {'clock': 'hostClock', 'reset': sim_ports['fased_model_reset'],
                          'targetFire': sim_ports['fased_tfire'], 'tCycle': sim_ports['fased_timing_cycle'],
                          'enq_ready': 'readLatency_unused_enq_ready',
                          'enq_valid': 'fased_timing_requests_ar_ready & ' + sim_ports['fased_timing_requests_ar_valid'],
                          'enq_bits_releaseCycle': sim_ports['fased_read_release_cycle'],
                          'enq_bits_id': sim_ports['fased_timing_requests_ar_bits_id'],
                          'enq_bits_len': sim_ports['fased_timing_requests_ar_bits_len'],
                          'deq_ready': sim_ports['fased_next_read_ready'], 'deq_valid': sim_ports['fased_next_read_valid'],
                          'deq_bits_id': sim_ports['fased_next_read_bits_id'], 'deq_bits_len': sim_ports['fased_next_read_bits_len']}
        assert queue_ports == expected_ports, 'emitted read queue binding differs'
        assert 'fased_next_read' not in module(rtl, new)[0] and 'fased_read_release_cycle' not in module(rtl, new)[0]
        reports.append(dict(allocation=allocation, existing_modules_retained=len(prior),
                            annotation_records_preserved=len(annotations),
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['Queue_16', 'LatencyPipe', 'FASEDMemoryTimingModel'],
                  allocations=reports, reference_has_print_host=False,
                  scope='read deadline queue; native len preserves Scala metadata optimized away in SFC RTL; write latency and MMIO attachment remain external')
    (evidence / 'read-latency-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC ten-entry read deadline queue, target-qualified state/RAM and preserved Print banks in both allocations')


if __name__ == '__main__': main()
