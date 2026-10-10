#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket read scheduling against immutable SFC RTL.

Interpret the emitted scheduler SSA across all flag and AXI ID combinations.
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
    baseline = (evidence / 'candidate/post-fame-fased-read-scheduler.mlir').read_text()
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-read-scheduler{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-read-buffer{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDReadScheduler', 'GGFASEDReadSchedulerWrapper'}
        old, new = 'GGFASEDReadBufferWrapper', 'GGFASEDReadSchedulerWrapper'
        expected_header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        old_ports = re.findall(r'\b(?:in|out) %(\w+):', native_module(before, old).splitlines()[0])
        for name in old_ports:
            if name not in ('fased_egress_readiness', 'fased_read_buffer_address', 'fased_read_buffer_deq'):
                expected_header = expected_header.replace('~' + old + '|' + old + '>' + name,
                                                          '~' + old + '|' + new + '>' + name)
        for old_leaf, new_leaf in [('fased_egress_readiness.writeValid', 'fased_write_egress_valid'),
                                   ('fased_egress_readiness.readValid', 'fased_read_egress_resp.hValid'),
                                   ('fased_read_buffer_deq.bits.data', 'fased_read_egress_resp.tBits.data'),
                                   ('fased_read_buffer_deq.bits.last', 'fased_read_egress_resp.tBits.last')]:
            expected_header = expected_header.replace('~' + old + '|' + old + '>' + old_leaf,
                                                      '~' + old + '|' + new + '>' + new_leaf)
        expected_header = expected_header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == expected_header, 'annotation retarget differs'
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
        assert all(wiring[k] == v for k, v in connections.items()), 'scheduler boundary wiring differs'
        assert all(n not in wrapper.splitlines()[0] for n in
                   ('fased_egress_readiness', 'fased_read_buffer_address', 'fased_read_buffer_deq'))
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'reverse': reverse, 'existing_modules_retained': len(prior),
                        'scheduler_cases': cases, 'start_and_retire': chained,
                        'reset_and_start': reset_starts, 'retire_without_buffer_valid': invalid_retire,
                        'wrapper_connections_compared': len(connections)})
    report = {'golden': str(golden), 'SFC_modules': ['ReadEgress', 'FASEDMemoryTimingModel'],
              'constructor_orders': reports, 'reference_has_print_host': False,
              'scope': 'read request state and response validity; timing model and write egress remain external'}
    (evidence / 'rocket-fased-read-scheduler-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC read scheduler state/priority, qualified wiring and preserved Print banks in both orders')


if __name__ == '__main__': main()
