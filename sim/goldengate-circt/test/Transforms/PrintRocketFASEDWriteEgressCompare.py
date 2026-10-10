#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket write acknowledgment scheduling against immutable SFC RTL.

Interpret emitted SSA across flag, acknowledgment ID and counter combinations.
Pin the SFC equations and sequential priority before using them as the oracle.
The SFC fixture has Print disabled; retain expanded Print banks in both orders.
"""
import itertools
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketFASEDReadBufferCompare import wrapper_wiring
from PrintRocketFASEDIssueCompare import expressions, evaluate


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    write = module(reference, 'WriteEgress')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(write).items()}
    expected = {'_T': 'io_req_hValid & io_req_t_valid',
                '_T_3': 'io_req_hValid & currReqReg_valid & haveAck & io_resp_tReady',
                'retry': 'currReqReg_valid & ~haveAck',
                'deqId': 'retry ? currReqReg_bits : io_req_t_bits',
                'idMatch': 'currReqReg_bits == io_enq_bits_id',
                'do_enq': 'io_enq_ready & io_enq_valid',
                'io_enq_ready': "1'h1", 'io_resp_tBits_id': 'currReqReg_bits',
                'io_resp_hValid': '~currReqReg_valid | haveAck'}
    for name, expr in expected.items(): assert eq[name] == expr, name
    assert re.search(r"if \(reset\) begin\s*currReqReg_valid <= 1'h0;\s*"
                     r"end else if \(io_req_hValid & io_req_t_valid\) begin\s*"
                     r"currReqReg_valid <= io_req_t_valid;\s*"
                     r"end else if \(io_req_hValid & currReqReg_valid & haveAck & io_resp_tReady\) begin\s*"
                     r"currReqReg_valid <= 1'h0;", write), 'SFC valid priority differs'
    assert re.search(r"end\s*if \(io_req_hValid & io_req_t_valid\) begin\s*"
                     r"currReqReg_bits <= io_req_t_bits;", write), 'SFC ID reset/capture differs'
    assert re.search(r"if \(reset\) begin\s*haveAck <= 1'h0;\s*"
                     r"end else if \(retry \| _T\) begin", write), 'SFC retry gated by token fire'
    # Pin every selector and wrapping counter, including global same-ID cancel.
    for i in range(1, 14):
        previous_selector = 'ackCounters_0' if i == 1 else f'_GEN_{i + 2}'
        assert eq[f'_GEN_{i + 3}'] == f"4'h{i:x} == deqId ? ackCounters_{i} : {previous_selector}"
    assert re.search(r"if \(4'hf == deqId\) begin\s*haveAck <= ackCounters_15;\s*"
                     r"end else if \(4'he == deqId\) begin\s*haveAck <= ackCounters_14;\s*"
                     r"end else begin\s*haveAck <= _GEN_16;", write)
    for i in range(16):
        assert re.search(r'reg\s+ackCounters_' + str(i) + r';', write), 'counter width differs'
        assert re.search(r"if \(reset\) begin\s*ackCounters_" + str(i) + r" <= 1'h0;\s*"
                         r"end else if \(~\(_T_3 & do_enq & idMatch\)\) begin\s*"
                         r"if \(do_enq & io_enq_bits_id == 4'h" + f'{i:x}' + r"\) begin\s*"
                         r"ackCounters_" + str(i) + r" <= ackCounters_" + str(i) + r" \+ 1'h1;\s*"
                         r"end else if \(_T_3 & currReqReg_bits == 4'h" + f'{i:x}' + r"\) begin\s*"
                         r"ackCounters_" + str(i) + r" <= ackCounters_" + str(i) + r" - 1'h1;", write)
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['writeEgress_clock'] == 'clock'
    assert host['writeEgress_reset'] == 'reset | _readEgress_reset_T_1'
    assert host['_readEgress_reset_T_1'] == 'hPort_hBits_reset & targetFire'
    assert ' '.join(host['writeEgress_io_req_hValid'].split()) == ' '.join(host['targetFire'].split())
    assert host['writeEgress_io_req_t_valid'] == 'model_tNasti_io_egressReq_b_valid'
    assert host['writeEgress_io_req_t_bits'] == 'model_tNasti_io_egressReq_b_bits'
    assert host['writeEgress_io_resp_tReady'] == 'model_tNasti_io_egressResp_bReady'
    assert host['model_tNasti_io_egressResp_bBits_id'] == 'writeEgress_io_resp_tBits_id'
    assert host['writeEgress_io_enq_valid'] == 'auto_to_host_dram_out_b_valid'
    assert host['writeEgress_io_enq_bits_id'] == 'auto_to_host_dram_out_b_bits_id'
    baseline = (evidence / 'candidate/post-fame-fased-write-egress.mlir').read_text()
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-write-egress{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-read-scheduler{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDWriteEgress', 'GGFASEDWriteEgressWrapper'}
        old, new = 'GGFASEDReadSchedulerWrapper', 'GGFASEDWriteEgressWrapper'
        expected_header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        old_ports = re.findall(r'\b(?:in|out) %(\w+):', native_module(before, old).splitlines()[0])
        for name in old_ports:
            if name not in ('fased_write_egress_valid', 'fased_host_write_responses'):
                expected_header = expected_header.replace('~' + old + '|' + old + '>' + name,
                                                          '~' + old + '|' + new + '>' + name)
        for old_leaf, new_leaf in [('fased_write_egress_valid', 'fased_write_egress_resp.hValid'),
                                   ('fased_host_write_responses.bReady', 'fased_host_write_response.ready'),
                                   ('fased_host_write_responses.bValid', 'fased_host_write_response.valid')]:
            expected_header = expected_header.replace('~' + old + '|' + old + '>' + old_leaf,
                                                      '~' + old + '|' + new + '>' + new_leaf)
        expected_header = expected_header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == expected_header, 'annotation retarget differs'
        egress = native_module(text, 'GGFASEDWriteEgress')
        assert egress == native_module(baseline, 'GGFASEDWriteEgress')
        assert len(re.findall(' = firrtl.regreset ', egress)) == 18
        assert len(re.findall(' = firrtl.reg ', egress)) == 1
        for name in ['currReqReg_valid', 'haveAck'] + [f'ackCounters_{i}' for i in range(16)]:
            init = re.search('%' + name + r' = firrtl.regreset %clock, %reset, (%\w+) : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>', egress)
            assert init and re.search(re.escape(init[1]) + r' = firrtl.constant 0 : !firrtl.uint<1>', egress)
        assert re.search(r'%currReqReg_bits = firrtl.reg %clock.*!firrtl.uint<4>', egress)
        assert '%c0_ui1 = firrtl.constant 0' in egress
        # Treat unreset RegOp as a state leaf; apply reset only to RegResetOp.
        nets, assertions = expressions(egress.replace(' = firrtl.reg ', ' = firrtl.regreset '), 'GGFASEDWriteEgress')
        assert not assertions
        cases = cancellations = retries = chained = wraps = old_samples = reset_starts = 0
        for flags in itertools.product((0, 1), repeat=7):
            reset, fire, request, ready, enqueue, active, ack = flags
            for old_id, enq_id in itertools.product(range(16), repeat=2):
                new_id = (old_id + enq_id + request + ready) % 16
                start, retire = fire and request, fire and active and ack and ready
                retry = active and not ack
                cancel = retire and enqueue and old_id == enq_id
                selected = old_id if retry else new_id
                # Complementary masks exercise both values of every counter,
                # including old sampled state when this cycle toggles that ID.
                for mask in (1 << selected, 65535 ^ (1 << selected)):
                    inputs = {'reset': reset, 'targetFire': fire, 'req.valid': request, 'req.bits': new_id,
                              'enq.valid': enqueue, 'enq.bits.id': enq_id,
                              'enq.bits.user': 1, 'enq.bits.resp': 3, 'resp.tReady': ready,
                              'currReqReg_valid': active, 'haveAck': ack, 'currReqReg_bits': old_id}
                    inputs.update({f'ackCounters_{i}': mask >> i & 1 for i in range(16)})
                    value = lambda name: evaluate(nets[name], inputs, nets)
                    assert value('enq.ready') == 1
                    assert value('resp.hValid') == (not active or ack)
                    assert value('resp.tBits.id') == old_id
                    assert value('resp.tBits.user') == 0 and value('resp.tBits.resp') == 0
                    assert (0 if reset else value('currReqReg_valid')) == (0 if reset else 1 if start else 0 if retire else active)
                    assert value('currReqReg_bits') == (new_id if start else old_id)
                    assert (0 if reset else value('haveAck')) == (0 if reset else (mask >> selected & 1) if retry or start else ack)
                    next_mask = mask
                    if not cancel:
                        if enqueue: next_mask ^= 1 << enq_id
                        if retire: next_mask ^= 1 << old_id
                    for i in range(16):
                        assert (0 if reset else value(f'ackCounters_{i}')) == (0 if reset else next_mask >> i & 1)
                    cases += 1; cancellations += bool(cancel); retries += bool(retry and not fire)
                    chained += bool(start and retire); wraps += bool(enqueue and (mask >> enq_id & 1) and not cancel)
                    old_samples += bool((retry or start) and enqueue and selected == enq_id)
                    reset_starts += bool(reset and start)
        assert all((cancellations, retries, chained, wraps, old_samples, reset_starts))
        wrapper = native_module(text, new)
        wiring = wrapper_wiring(wrapper)
        connections = {'writeEgress.clock': 'top.hostClock',
                       'writeEgress.reset': 'sim.fased_egress_reset',
                       'writeEgress.targetFire': 'sim.fased_tfire',
                       'writeEgress.req': 'top.fased_write_egress_req',
                       'writeEgress.enq': 'top.fased_host_write_response',
                       'top.fased_write_egress_resp': 'writeEgress.resp',
                       'sim.fased_write_egress_valid': 'writeEgress.resp.hValid',
                       'sim.fased_host_write_responses.bReady': 'top.fased_host_write_response.ready',
                       'sim.fased_host_write_responses.bValid': 'top.fased_host_write_response.valid',
                       'sim.fased_read_egress_req': 'top.fased_read_egress_req',
                       'top.fased_read_egress_resp': 'sim.fased_read_egress_resp',
                       'sim.fased_host_read_response': 'top.fased_host_read_response',
                       'top.fased_host_requests': 'sim.fased_host_requests'}
        # The copied aggregate connections retain the native read scheduler and
        # host R/request boundaries alongside the new acknowledgment state.
        for name in ('fased_read_egress_req', 'fased_read_egress_resp', 'fased_host_read_response', 'fased_host_requests'):
            assert name in wrapper.splitlines()[0]
        assert all(wiring[k] == v for k, v in connections.items()), 'write egress boundary wiring differs'
        assert all(n not in wrapper.splitlines()[0] for n in
                   ('fased_write_egress_valid', 'fased_host_write_responses'))
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'reverse': reverse, 'existing_modules_retained': len(prior),
                        'write_egress_cases': cases, 'same_ID_cancel': cancellations,
                        'host_only_retries': retries, 'start_and_retire': chained, 'counter_wraps': wraps,
                        'old_counter_samples': old_samples, 'reset_and_start': reset_starts,
                        'wrapper_connections_compared': len(connections)})
    report = {'golden': str(golden), 'SFC_modules': ['WriteEgress', 'FASEDMemoryTimingModel'],
              'constructor_orders': reports, 'reference_has_print_host': False,
              'scope': 'write acknowledgment state and qualified wiring; timing model requests remain external'}
    (evidence / 'rocket-fased-write-egress-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC write acknowledgment state/priority, qualified wiring and preserved Print banks in both orders')


if __name__ == '__main__': main()
