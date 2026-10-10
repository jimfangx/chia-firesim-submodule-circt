#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare composed Print/Rocket timing AW queue with immutable SFC Queue_14.

Interpret actual CIRCT queue SSA against the pinned SFC flow/FIFO equations,
including target-qualified reset and non-reset RAM writes. Retain prior modules
and annotations in both orders. AW/W pairing remains an explicit completion input.
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


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    queue_body = module(reference, 'Queue_14')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(queue_body).items()}
    expected = {'ptr_match': 'enq_ptr_value == deq_ptr_value', 'empty': 'ptr_match & ~maybe_full',
                'full': 'ptr_match & maybe_full', 'io_enq_ready': '~full',
                'io_deq_valid': 'io_enq_valid | ~empty', '_do_enq_T': 'io_enq_ready & io_enq_valid',
                '_do_deq_T': 'io_deq_ready & io_deq_valid', '_GEN_24': "io_deq_ready ? 1'h0 : _do_enq_T",
                'do_enq': 'empty ? _GEN_24 : _do_enq_T', 'do_deq': "empty ? 1'h0 : _do_deq_T",
                'wrap': "enq_ptr_value == 4'h9", 'wrap_1': "deq_ptr_value == 4'h9",
                '_value_T_1': "enq_ptr_value + 4'h1", '_value_T_3': "deq_ptr_value + 4'h1"}
    for name, expression in expected.items(): assert eq[name] == expression, name
    for payload, width, port in [('id', 4, 'id')]:
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
    assert re.search(r'Queue_14 awQueue \(', pipe_body)
    assert re.search(r'if \(_T_16 & ~\(awQueue_io_enq_ready \| ~_awQueue_io_enq_valid_T\)\)', pipe_body)
    pipe = equations(pipe_body)
    assert pipe['_T_16'] == '~reset'
    assert pipe['awQueue_io_enq_valid'] == 'nastiReqIden_io_out_aw_ready & nastiReqIden_io_out_aw_valid'
    assert pipe['awQueue_io_enq_bits_id'] == 'nastiReqIden_io_out_aw_bits_id'
    assert pipe['awQueue_io_deq_ready'] == pipe['newWReq']
    assert pipe['writePipe_io_enq_valid'] == pipe['newWReq']
    assert pipe['writePipe_io_enq_bits_xaction_id'] == 'awQueue_io_deq_bits_id'
    assert pipe['awQueue_clock'] == 'clock' and pipe['awQueue_reset'] == 'reset'
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['model_clock'] == 'gate_O' and host['gate_I'] == 'clock'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    assert host['model_reset'] == 'hPort_hBits_reset'
    baseline = (evidence / 'candidate/post-fame-fased-timing-aw-queue.mlir').read_text()
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-timing-aw-queue{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-write-latency{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDTimingAWQueue10', 'GGFASEDTimingAWQueueWrapper'}
        old, new = 'GGFASEDWriteLatencyWrapper', 'GGFASEDTimingAWQueueWrapper'
        consumed = {'fased_write_completion'}
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in old_ports:
            if name not in consumed:
                header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDTimingAWQueue10')
        assert helper == native_module(baseline, 'GGFASEDTimingAWQueue10')
        memories = [line for line in helper.splitlines() if ' = firrtl.mem ' in line]
        assert len(memories) == 1 and all(term in memories[0] for term in
            ['depth = 10 : i64', 'readLatency = 0 : i32', 'writeLatency = 1 : i32', 'uint<4>'])
        assert len(re.findall(' = firrtl.regreset ', helper)) == 3 and ' = firrtl.reg ' not in helper
        for name, width in [('enq_ptr_value', 4), ('deq_ptr_value', 4), ('maybe_full', 1)]:
            reg = re.search('%' + name + r' = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
            assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<' + str(width) + '>', helper)
        nets, assertions = expressions(helper, 'GGFASEDTimingAWQueue10')
        assert len(assertions) == 1 and assertions[0][2] == 'AW queue in SplitTransaction timing model would overflow.'
        assert nets['ram_read.clk'] == nets['ram_write.clk'] == ('leaf', 'clock')
        counts = dict(transitions=0, flow=0, full_pops=0, stalls=0, stalled_resets=0,
                      reset_writes=0, pointer_wraps=0, overflow=0)

        def sample(ep, dp, mf, memory, entries, reset, fire, ev, ready, incoming):
            empty, full = not entries, len(entries) == 10
            head = incoming if empty else entries[0]
            valid = not empty or ev
            push = not full and ev and not (empty and ready)
            pop = not empty and valid and ready
            inputs = dict(clock=1, reset=reset, targetFire=fire, enq_ptr_value=ep,
                          deq_ptr_value=dp, maybe_full=mf)
            inputs.update({'enq.valid': ev, 'deq.ready': ready, 'enq.bits.id': incoming, 'ram_read.data': memory[dp]})
            value = lambda name: evaluate(nets[name], inputs, nets)
            assert value('enq.ready') == (not full)
            assert value('deq.valid') == valid
            assert value('deq.bits.id') == head
            assert value('ram_read.en') == value('ram_write.mask') == 1
            assert value('ram_read.addr') == dp and value('ram_write.addr') == ep
            assert value('ram_write.en') == (fire and push)
            assert value('ram_write.data') == incoming
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
            counts['flow'] += fire and empty and ev and ready
            counts['full_pops'] += fire and full and pop
            counts['stalls'] += not fire
            counts['stalled_resets'] += reset and not fire
            counts['reset_writes'] += reset and fire and push
            counts['pointer_wraps'] += fire and not reset and ((push and ep == 9) or (pop and dp == 9))
            counts['overflow'] += fire and not reset and full and ev
            return next_ep, next_dp, next_mf

        for dp, occupancy, flags, incoming in itertools.product(range(10), range(11), range(16), range(16)):
            entries = deque(i % 16 for i in range(occupancy))
            memory = [0] * 10
            for i, item in enumerate(entries): memory[(dp + i) % 10] = item
            sample((dp + occupancy) % 10, dp, occupancy == 10, memory, entries,
                   flags & 1, (flags >> 1) & 1, (flags >> 2) & 1, (flags >> 3) & 1, incoming)
        rng = random.Random(223)
        ep = dp = 0; mf = False; memory = [0] * 10; entries = deque()
        for i in range(30000):
            ep, dp, mf = sample(ep, dp, mf, memory, entries, i % 997 == 0 or rng.randrange(113) == 0,
                               rng.randrange(3) != 0, rng.randrange(4) != 0, rng.randrange(2), rng.randrange(16))
        assert all(counts.values()), 'missing queue coverage'
        connections = {'timingAW.clock': 'top.hostClock', 'timingAW.reset': 'sim.fased_model_reset',
                       'timingAW.targetFire': 'sim.fased_tfire',
                       'timingAW.enq.valid': frozenset(['sim.fased_timing_requests.aw.ready', 'sim.fased_timing_requests.aw.valid']),
                       'timingAW.enq.bits.id': 'sim.fased_timing_requests.aw.bits.id',
                       'timingAW.deq.ready': 'top.fased_write_pair_complete',
                       'sim.fased_write_completion.valid': 'top.fased_write_pair_complete',
                       'sim.fased_write_completion.bits.id': 'timingAW.deq.bits.id',
                       'top.fased_timing_aw_queue_ready': 'timingAW.enq.ready'}
        for direction, name in old_ports:
            if name not in consumed:
                connections[('sim.' if direction == 'in' else 'top.') + name] = ('top.' if direction == 'in' else 'sim.') + name
        assert wiring(text, new, aggregate=True) == connections, 'AW ordering wrapper differs'
        top_ports = re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0])
        assert top_ports == [(d, n) for d, n in old_ports if n not in consumed] + [
            ('in', 'fased_write_pair_complete'), ('out', 'fased_timing_aw_queue_ready')]
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append(dict(reverse=reverse, existing_modules_retained=len(prior),
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['Queue_14', 'LatencyPipe', 'FASEDMemoryTimingModel'],
                  constructor_orders=reports, reference_has_print_host=False,
                  scope='accepted AW ID queue; oldest accepted AW ID feeds write latency; AW/W pairing pulse and latency MMIO attachment remain external')
    (evidence / 'rocket-fased-timing-aw-queue-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC ten-entry accepted AW ID queue, target-qualified state/RAM and preserved Print banks in both orders')


if __name__ == '__main__': main()
