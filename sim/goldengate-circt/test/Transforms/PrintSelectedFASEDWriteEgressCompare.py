#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print write acknowledgment scheduling against immutable SFC RTL.

Interpret emitted SSA across flag, acknowledgment ID and counter combinations.
Pin the SFC equations and sequential priority before using them as the oracle.
The SFC fixture has Print disabled; retain expanded Print banks in both allocations.
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
from PrintRocketControlMasterCompare import native_ports


def transfer(value, old, new, copied):
    if isinstance(value, list): return [transfer(v, old, new, copied) for v in value]
    if isinstance(value, dict): return {k: transfer(v, old, new, copied) for k, v in value.items()}
    if not isinstance(value, str): return value
    if value == '~' + old: return '~' + new
    prefix = '~' + old + '|'
    if not value.startswith(prefix): return value
    result = value[len(prefix):]
    if result.startswith(old + '>'):
        ref = result[len(old) + 1:]
        if re.split(r'[.\[]', ref)[0] in copied: result = new + '>' + ref
        mapped = {'fased_write_egress_valid': 'fased_write_egress_resp.hValid',
                  'fased_host_write_responses.bReady': 'fased_host_write_response.ready',
                  'fased_host_write_responses.bValid': 'fased_host_write_response.valid'}
        if ref in mapped: result = new + '>' + mapped[ref]
    return '~' + new + '|' + result


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
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
    baseline = baseline_path.read_text()
    previous = evidence.parent / 'iteration148-print-fased-read-scheduler'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        directory = evidence / allocation
        text = (directory / 'post-print-fased-write-egress.mlir').read_text()
        before = (directory / 'post-print-fased-read-scheduler.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDWriteEgress', 'GGSelectedPrintFASEDWriteEgressWrapper'}
        old, new = 'GGSelectedPrintFASEDReadSchedulerWrapper', 'GGSelectedPrintFASEDWriteEgressWrapper'
        copied = [p for p in native_ports(native_module(before, old)) if p[1] not in
                  ('fased_write_egress_valid', 'fased_host_write_responses')]
        raw_before = json.loads((directory / 'post-print-fased-read-scheduler-all.json').read_text())
        raw_after = json.loads((directory / 'post-print-fased-write-egress-all.json').read_text())
        assert transfer(raw_before, old, new, {p[1] for p in copied}) == raw_after, 'recursive annotation transfer differs'
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
                       }
        for direction, name, typ in copied:
            dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
            connections[dest] = src
        assert wiring == connections, 'write egress boundary wiring differs'
        assert native_ports(wrapper)[:len(copied)] == copied
        assert [p[1] for p in native_ports(wrapper)[len(copied):]] == [
            'fased_host_write_response', 'fased_write_egress_req', 'fased_write_egress_resp']
        emitted = clean((directory / 'post-print-fased-write-egress.sv').read_text())
        rtl = module(emitted, 'GGFASEDWriteEgress')[1]
        rtl_eq = {k: ' '.join(v.split()) for k, v in equations(rtl).items()}
        assert rtl_eq['enq_ready'] == "1'h1"
        assert rtl_eq['resp_hValid'] == '~currReqReg_valid | haveAck'
        assert rtl_eq['resp_tBits_id'] == 'currReqReg_bits'
        assert rtl_eq['resp_tBits_user'] == "1'h0" and rtl_eq['resp_tBits_resp'] == "2'h0"
        reset_block = re.search(r'if \(reset\) begin(.*?)end\s+else', rtl, re.S)
        assert reset_block, 'emitted reset block missing'
        expected_reset = {'currReqReg_valid', 'haveAck'} | {f'ackCounters_{i}' for i in range(16)}
        assert set(re.findall(r"(\w+) <= 1'h0;", reset_block[1])) == expected_reset, 'emitted reset state differs'
        assert 'currReqReg_bits' not in reset_block[1], 'emitted request ID acquired reset'
        rtl_wrapper = module(emitted, new)[1]
        assert re.search(r'\.reset\s*\(_sim_fased_egress_reset\)', rtl_wrapper)
        assert re.search(r'\.targetFire\s*\(_sim_fased_tfire\)', rtl_wrapper)
        for collateral in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (directory / collateral).read_bytes() == (previous / allocation / collateral).read_bytes(), 'allocated collateral changed'
        reports.append({'allocation': allocation, 'existing_modules_retained': len(prior),
                        'write_egress_cases': cases, 'same_ID_cancel': cancellations,
                        'host_only_retries': retries, 'start_and_retire': chained, 'counter_wraps': wraps,
                        'old_counter_samples': old_samples, 'reset_and_start': reset_starts,
                        'wrapper_connections_compared': len(connections),
                        'annotation_records_retained': len(raw_after), 'allocated_headers_unchanged': True})
    report = {'golden': str(golden), 'SFC_modules': ['WriteEgress', 'FASEDMemoryTimingModel'],
              'allocations': reports, 'reference_has_print_host': False,
              'scope': 'write acknowledgment state and qualified wiring; timing model requests remain external'}
    (evidence / 'write-egress-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC write acknowledgment state/priority, qualified wiring and preserved Print banks in both allocations')


if __name__ == '__main__': main()
