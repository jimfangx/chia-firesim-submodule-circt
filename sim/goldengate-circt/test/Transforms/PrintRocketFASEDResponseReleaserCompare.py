#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket response occupancy with immutable SFC RTL.

Pin AXI4Releaser, its pipe queues and gated model clock before interpreting
emitted CIRCT SSA. The fixture disables Print; retain expanded banks separately.
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
    release = module(reference, 'AXI4Releaser')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(release).items()}
    expected = {'_q_io_deq_ready_T': 'io_r_ready & io_r_valid',
                'currentRead_q_io_deq_ready': '_q_io_deq_ready_T & io_r_bits_last',
                'currentWrite_q_io_deq_ready': 'io_b_ready & io_b_valid'}
    for channel, queue, payload in [('r', 'Read', ['data', 'last', 'id']), ('b', 'Write', ['id'])]:
        expected.update({f'io_{channel}_valid': f'current{queue}_q_io_deq_valid',
                         f'io_egressReq_{channel}_valid': f'io_next{queue}_ready & io_next{queue}_valid',
                         f'io_egressReq_{channel}_bits': f'io_next{queue}_bits_id',
                         f'io_egressResp_{channel}Ready': f'io_{channel}_ready',
                         f'io_next{queue}_ready': f'current{queue}_q_io_enq_ready',
                         f'current{queue}_q_clock': 'clock', f'current{queue}_q_reset': 'reset',
                         f'current{queue}_q_io_enq_valid': f'io_next{queue}_valid'})
        for field in payload:
            expected[f'io_{channel}_bits_{field}'] = f'io_egressResp_{channel}Bits_{field}'
    for name, expr in expected.items(): assert eq[name] == expr, name
    queue_names = []
    for kind in ('Read', 'Write'):
        match = re.search(r'\b(\w+) current' + kind + r'_q \(', release)
        assert match
        queue_names.append(match[1])
        body = module(reference, match[1])[1]
        queue = equations(body)
        assert re.search(r'reg\s+maybe_full;', body)
        for name, expr in {'empty': '~maybe_full', 'do_enq': 'io_enq_ready & io_enq_valid',
                           'do_deq': 'io_deq_ready & io_deq_valid',
                           'io_enq_ready': 'io_deq_ready | empty', 'io_deq_valid': '~empty'}.items():
            assert queue[name] == expr
        assert re.search(r"if \(reset\) begin\s*maybe_full <= 1'h0;\s*"
                         r"end else if \(do_enq != do_deq\) begin\s*maybe_full <= do_enq;", body)
    pipe = equations(module(reference, 'LatencyPipe')[1])
    assert pipe['xactionRelease_clock'] == 'clock' and pipe['xactionRelease_reset'] == 'reset'
    for ch in ('r', 'b'):
        assert pipe[f'xactionRelease_io_{ch}_ready'] == f'tNasti_io_tNasti_{ch}_ready'
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['gate_I'] == 'clock' and host['model_clock'] == 'gate_O'
    assert host['model_reset'] == 'hPort_hBits_reset'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    baseline = (evidence / 'candidate/post-fame-fased-response-releaser.mlir').read_text()
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-response-releaser{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-write-egress{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDResponseReleaser', 'GGFASEDResponseReleaserWrapper'}
        old, new = 'GGFASEDWriteEgressWrapper', 'GGFASEDResponseReleaserWrapper'
        consumed = ('fased_timing', 'fased_read_egress_req', 'fased_read_egress_resp',
                    'fased_write_egress_req', 'fased_write_egress_resp')
        expected_header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        old_ports = re.findall(r'\b(?:in|out) %(\w+):', native_module(before, old).splitlines()[0])
        for name in old_ports:
            if name not in consumed:
                expected_header = expected_header.replace('~' + old + '|' + old + '>' + name,
                                                          '~' + old + '|' + new + '>' + name)
        for ch in ('aw', 'w', 'ar'):
            expected_header = expected_header.replace('~' + old + '|' + old + '>fased_timing.' + ch + '.',
                                                      '~' + old + '|' + new + '>fased_timing_requests.' + ch + '.')
        expected_header = expected_header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == expected_header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDResponseReleaser')
        assert helper == native_module(baseline, 'GGFASEDResponseReleaser')
        assert len(re.findall(' = firrtl.regreset ', helper)) == 2
        assert ' = firrtl.reg ' not in helper and 'firrtl.mem ' not in helper
        for name in ('currentRead_full', 'currentWrite_full'):
            init = re.search('%' + name + r' = firrtl.regreset %clock, (%\w+), (%\w+) : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>, !firrtl.uint<1>', helper)
            assert init and re.search(re.escape(init[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(init[2]) + r' = firrtl.constant 0 : !firrtl.uint<1>', helper)
        nets, assertions = expressions(helper, 'GGFASEDResponseReleaser')
        assert not assertions
        assert nets['r.bits'] == ('leaf', 'egressResp.rBits')
        assert nets['b.bits'] == ('leaf', 'egressResp.bBits')
        cases = replacements = non_last = blocked = stalled_reset = stalled_accept = empty_push = 0
        for flags in itertools.product((0, 1), repeat=9):
            reset, fire, read_full, write_full, rready, bready, last, next_read, next_write = flags
            rpop, bpop = read_full and rready and last, write_full and bready
            ready_r, ready_b = not read_full or rpop, not write_full or bpop
            push_r, push_b = ready_r and next_read, ready_b and next_write
            for read_id, write_id in itertools.product(range(16), repeat=2):
                # Response IDs deliberately differ from newly accepted request IDs.
                read_bits = {'user': next_write, 'id': read_id ^ 15, 'last': last,
                             'data': (read_id << 60) | (write_id << 56) | 0x123456789abcde, 'resp': 3}
                write_bits = {'user': next_read, 'id': write_id ^ 15, 'resp': 2}
                inputs = {'reset': reset, 'targetFire': fire, 'currentRead_full': read_full,
                          'currentWrite_full': write_full, 'r.ready': rready, 'b.ready': bready,
                          'egressResp.rBits.last': last, 'egressResp.rBits': read_bits,
                          'egressResp.bBits': write_bits, 'nextRead.valid': next_read,
                          'nextRead.bits.id': read_id, 'nextRead.bits.len': (read_id * 17) & 255,
                          'nextWrite.valid': next_write, 'nextWrite.bits.id': write_id}
                value = lambda name: evaluate(nets[name], inputs, nets)
                for ch, kind, full, pop, push, ready, req_id, bits, out_ready in [
                        ('r', 'Read', read_full, rpop, push_r, ready_r, read_id, read_bits, rready),
                        ('b', 'Write', write_full, bpop, push_b, ready_b, write_id, write_bits, bready)]:
                    assert value(f'next{kind}.ready') == ready
                    assert value(f'{ch}.valid') == full and value(f'{ch}.bits') == bits
                    assert value(f'egressReq.{ch}.valid') == push
                    assert value(f'egressReq.{ch}.bits') == req_id
                    assert value(f'egressResp.{ch}Ready') == out_ready
                    expected_full = 0 if fire and reset else push if fire and push != pop else full
                    actual_full = 0 if fire and reset else value(f'current{kind}_full')
                    assert actual_full == expected_full
                cases += 1
                replacements += bool(fire and rpop and push_r and not reset)
                non_last += bool(read_full and rready and not last)
                blocked += bool(next_read and not ready_r)
                stalled_reset += bool(reset and not fire and (read_full or write_full))
                stalled_accept += bool(not fire and (push_r or push_b))
                empty_push += bool(not read_full and push_r)
        assert all((replacements, non_last, blocked, stalled_reset, stalled_accept, empty_push))
        wrapper = native_module(text, new)
        wiring = wrapper_wiring(wrapper)
        connections = {'releaser.clock': 'top.hostClock', 'releaser.reset': 'sim.fased_model_reset',
                       'releaser.targetFire': 'sim.fased_tfire',
                       'releaser.nextRead': 'top.fased_next_read', 'releaser.nextWrite': 'top.fased_next_write',
                       'top.fased_host_requests': 'sim.fased_host_requests',
                       'sim.fased_host_read_response': 'top.fased_host_read_response',
                       'sim.fased_host_write_response': 'top.fased_host_write_response'}
        for ch in ('aw', 'w', 'ar'):
            connections['top.fased_timing_requests.' + ch] = 'sim.fased_timing.' + ch
        for ch, kind in [('r', 'read'), ('b', 'write')]:
            connections.update({f'releaser.{ch}.ready': f'sim.fased_timing.{ch}.ready',
                                f'sim.fased_timing.{ch}.valid': f'releaser.{ch}.valid',
                                f'sim.fased_timing.{ch}.bits': f'releaser.{ch}.bits',
                                f'sim.fased_{kind}_egress_req': f'releaser.egressReq.{ch}',
                                f'releaser.egressResp.{ch}Bits': f'sim.fased_{kind}_egress_resp.tBits',
                                f'sim.fased_{kind}_egress_resp.tReady': f'releaser.egressResp.{ch}Ready'})
        assert all(wiring[k] == v for k, v in connections.items()), 'response boundary wiring differs'
        ports = re.findall(r'\b(?:in|out) %(\w+):', wrapper.splitlines()[0])
        assert set(ports) == set(old_ports) - set(consumed) | {'fased_timing_requests', 'fased_next_read', 'fased_next_write'}
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'reverse': reverse, 'existing_modules_retained': len(prior),
                        'response_cases': cases, 'read_replacements': replacements, 'non_last_beats': non_last,
                        'blocked_read_requests': blocked, 'stalled_reset_occupied': stalled_reset,
                        'combinational_accept_without_fire': stalled_accept, 'empty_push_without_flow': empty_push,
                        'wrapper_connections_compared': len(connections)})
    report = {'golden': str(golden), 'SFC_modules': ['AXI4Releaser', *queue_names, 'LatencyPipe', 'FASEDMemoryTimingModel'],
              'constructor_orders': reports, 'reference_has_print_host': False,
              'scope': 'response occupancy and qualified reset; timing completion/readiness remain external'}
    (evidence / 'rocket-fased-response-releaser-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC response retirement/replacement, gated state/reset and preserved Print banks in both orders')


if __name__ == '__main__': main()
