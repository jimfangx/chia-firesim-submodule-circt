#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print read scheduling against immutable SFC RTL.

Interpret the emitted scheduler SSA across all flag and AXI ID combinations.
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
        mapped = {'fased_egress_readiness.writeValid': 'fased_write_egress_valid',
                  'fased_egress_readiness.readValid': 'fased_read_egress_resp.hValid',
                  'fased_read_buffer_deq.bits.data': 'fased_read_egress_resp.tBits.data',
                  'fased_read_buffer_deq.bits.last': 'fased_read_egress_resp.tBits.last'}
        if ref in mapped: result = new + '>' + mapped[ref]
    return '~' + new + '|' + result


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    read = module(reference, 'ReadEgress')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(read).items()}
    expected = {
        '_T': 'io_req_hValid & io_req_t_valid',
        '_xactionDone_T_1': 'io_req_hValid & currReqReg_valid & currReqReg_valid',
        '_xactionDone_T_2': 'io_req_hValid & currReqReg_valid & currReqReg_valid & io_resp_tReady',
        'xactionDone': '_xactionDone_T_2 & io_resp_tBits_last',
        'multiQueue_io_deq_ready': '_xactionDone_T_1 & io_resp_tReady',
        'multiQueue_io_deqAddr': '_T ? io_req_t_bits : currReqReg_bits',
        'io_resp_hValid': '~currReqReg_valid | currReqReg_valid & multiQueue_io_deq_valid',
        'io_resp_tBits_id': 'currReqReg_bits',
        'io_resp_tBits_data': 'multiQueue_io_deq_bits_data',
        'io_resp_tBits_last': 'multiQueue_io_deq_bits_last',
    }
    for name, expr in expected.items(): assert eq[name] == expr, name
    assert re.search(r"if \(reset\) begin\s*currReqReg_valid <= 1'h0;\s*"
                     r"end else if \(io_req_hValid & io_req_t_valid\) begin\s*"
                     r"currReqReg_valid <= io_req_t_valid;\s*"
                     r"end else if \(io_req_hValid & xactionDone\) begin\s*"
                     r"currReqReg_valid <= 1'h0;", read), 'SFC valid priority differs'
    assert re.search(r"end\s*if \(io_req_hValid & io_req_t_valid\) begin\s*"
                     r"currReqReg_bits <= io_req_t_bits;", read), 'SFC ID is reset or capture differs'
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['readEgress_clock'] == 'clock'
    assert host['readEgress_reset'] == 'reset | hPort_hBits_reset & targetFire'
    assert ' '.join(host['readEgress_io_req_hValid'].split()) == ' '.join(host['targetFire'].split())
    assert host['readEgress_io_req_t_valid'] == 'model_tNasti_io_egressReq_r_valid'
    assert host['readEgress_io_req_t_bits'] == 'model_tNasti_io_egressReq_r_bits'
    assert host['readEgress_io_resp_tReady'] == 'model_tNasti_io_egressResp_rReady'
    assert host['model_tNasti_io_egressResp_rBits_id'] == 'readEgress_io_resp_tBits_id'
    baseline = baseline_path.read_text()
    previous = evidence.parent / 'iteration147-print-fased-read-buffer'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        directory = evidence / allocation
        text = (directory / 'post-print-fased-read-scheduler.mlir').read_text()
        before = (directory / 'post-print-fased-read-buffer.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDReadScheduler', 'GGSelectedPrintFASEDReadSchedulerWrapper'}
        old, new = 'GGSelectedPrintFASEDReadBufferWrapper', 'GGSelectedPrintFASEDReadSchedulerWrapper'
        copied = [p for p in native_ports(native_module(before, old)) if p[1] not in
                  ('fased_egress_readiness', 'fased_read_buffer_address', 'fased_read_buffer_deq')]
        raw_before = json.loads((directory / 'post-print-fased-read-buffer-all.json').read_text())
        raw_after = json.loads((directory / 'post-print-fased-read-scheduler-all.json').read_text())
        assert transfer(raw_before, old, new, {p[1] for p in copied}) == raw_after, 'recursive annotation transfer differs'
        scheduler = native_module(text, 'GGFASEDReadScheduler')
        assert scheduler == native_module(baseline, 'GGFASEDReadScheduler')
        assert len(re.findall(' = firrtl.regreset ', scheduler)) == 1
        assert len(re.findall(' = firrtl.reg ', scheduler)) == 1
        assert re.search(r'%currReqReg_valid = firrtl.regreset %clock, %reset, %c0_ui1.*!firrtl.uint<1>', scheduler)
        assert re.search(r'%currReqReg_bits = firrtl.reg %clock.*!firrtl.uint<4>', scheduler)
        assert '%c0_ui1 = firrtl.constant 0' in scheduler
        # The shared SSA interpreter treats registers as state leaves. RegOp
        # uses the same leaf representation as RegResetOp; reset is applied
        # separately below only to the recorded active-valid register.
        nets, assertions = expressions(scheduler.replace(' = firrtl.reg ', ' = firrtl.regreset '), 'GGFASEDReadScheduler')
        assert not assertions
        cases = chained = reset_starts = invalid_retire = 0
        for flags in itertools.product((0, 1), repeat=7):
            reset, fire, request, ready, buffer_valid, last, active = flags
            for old_id, new_id in itertools.product(range(16), repeat=2):
                data = 0xa55af00d01234567 ^ (old_id << 32) ^ new_id
                inputs = {'reset': reset, 'targetFire': fire, 'req.valid': request, 'req.bits': new_id,
                          'bufferDeq.valid': buffer_valid, 'bufferDeq.bits.last': last,
                          'bufferDeq.bits.data': data, 'resp.tReady': ready,
                          'currReqReg_valid': active, 'currReqReg_bits': old_id}
                value = lambda name: evaluate(nets[name], inputs, nets)
                start, pop = fire and request, fire and active and ready
                done = pop and last
                assert value('bufferAddress') == (new_id if start else old_id)
                assert value('bufferDeq.ready') == pop
                assert value('resp.hValid') == (not active or active and buffer_valid)
                assert value('resp.tBits.id') == old_id and value('resp.tBits.data') == data
                assert value('resp.tBits.last') == last
                assert value('resp.tBits.user') == 0 and value('resp.tBits.resp') == 0
                assert (0 if reset else value('currReqReg_valid')) == (0 if reset else 1 if start else 0 if done else active)
                assert value('currReqReg_bits') == (new_id if start else old_id)
                cases += 1; chained += bool(start and done)
                reset_starts += bool(reset and start); invalid_retire += bool(done and not buffer_valid)
        wrapper = native_module(text, new)
        wiring = wrapper_wiring(wrapper)
        connections = {'readScheduler.clock': 'top.hostClock',
                       'readScheduler.reset': 'sim.fased_egress_reset',
                       'readScheduler.targetFire': 'sim.fased_tfire',
                       'readScheduler.req': 'top.fased_read_egress_req',
                       'readScheduler.bufferDeq': 'sim.fased_read_buffer_deq',
                       'sim.fased_read_buffer_address': 'readScheduler.bufferAddress',
                       'top.fased_read_egress_resp': 'readScheduler.resp',
                       'sim.fased_egress_readiness.readValid': 'readScheduler.resp.hValid',
                       'sim.fased_egress_readiness.writeValid': 'top.fased_write_egress_valid'}
        for direction, name, typ in copied:
            dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
            connections[dest] = src
        assert wiring == connections, 'scheduler boundary wiring differs'
        assert native_ports(wrapper)[:len(copied)] == copied
        assert [p[1] for p in native_ports(wrapper)[len(copied):]] == [
            'fased_write_egress_valid', 'fased_read_egress_req', 'fased_read_egress_resp']
        emitted = clean((directory / 'post-print-fased-read-scheduler.sv').read_text())
        rtl = module(emitted, 'GGFASEDReadScheduler')[1]
        rtl_eq = {k: ' '.join(v.split()) for k, v in equations(rtl).items()}
        assert rtl_eq['bufferAddress'] == '_GEN ? req_bits : currReqReg_bits'
        assert rtl_eq['bufferDeq_ready'] == 'bufferDeq_ready_0'
        assert rtl_eq['bufferDeq_ready_0'] == 'targetFire & currReqReg_valid & resp_tReady'
        assert rtl_eq['resp_hValid'] == '~currReqReg_valid | currReqReg_valid & bufferDeq_valid'
        assert re.search(r"if \(reset\)\s*currReqReg_valid <= 1'h0;", rtl)
        assert re.search(r"else\s*currReqReg_valid <=\s*_GEN \| ~\(bufferDeq_ready_0 & bufferDeq_bits_last\) & currReqReg_valid;", rtl)
        assert re.search(r"if \(_GEN\)\s*currReqReg_bits <= req_bits;", rtl)
        for field in ('id', 'last', 'data', 'user', 'resp'):
            expected_payload = {'id': 'currReqReg_bits', 'last': 'bufferDeq_bits_last',
                                'data': 'bufferDeq_bits_data', 'user': "1'h0", 'resp': "2'h0"}
            assert rtl_eq['resp_tBits_' + field] == expected_payload[field]
        rtl_wrapper = module(emitted, new)[1]
        assert re.search(r'\.reset\s*\(_sim_fased_egress_reset\)', rtl_wrapper)
        assert re.search(r'\.targetFire\s*\(_sim_fased_tfire\)', rtl_wrapper)
        for collateral in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (directory / collateral).read_bytes() == (previous / allocation / collateral).read_bytes(), 'allocated collateral changed'
        reports.append({'allocation': allocation, 'existing_modules_retained': len(prior),
                        'scheduler_cases': cases, 'start_and_retire': chained,
                        'reset_and_start': reset_starts, 'retire_without_buffer_valid': invalid_retire,
                        'wrapper_connections_compared': len(connections),
                        'annotation_records_retained': len(raw_after), 'allocated_headers_unchanged': True})
    report = {'golden': str(golden), 'SFC_modules': ['ReadEgress', 'FASEDMemoryTimingModel'],
              'allocations': reports, 'reference_has_print_host': False,
              'scope': 'read request state and response validity; timing model and write egress remain external'}
    (evidence / 'read-scheduler-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC read scheduler state/priority, qualified wiring and preserved Print banks in both allocations')


if __name__ == '__main__': main()
