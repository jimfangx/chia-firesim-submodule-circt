#!/usr/bin/env python3
# See LICENSE for license details.
"""Check expanded Print/Rocket ReadEgress against immutable SFC U250 RTL.

Compare actual CIRCT modules, memory/reset geometry, and wrapper SSA wiring.
The shared oracle disables Print; request scheduling and MMIO are later stages.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module


def wrapper_wiring(body):
    lines = body.splitlines()
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', lines[0])}
    nets = {}
    for line in lines[1:]:
        if ' = firrtl.instance ' in line:
            lhs, rhs = line.split(' = firrtl.instance ', 1)
            values.update(zip(re.findall(r'%\w+', lhs),
                              (rhs.split()[0] + '.' + p for p in re.findall(r'\b(?:in|out) (\w+):', rhs))))
        if m := re.search(r'(%\w+) = firrtl.constant (\d+)', line): values[m[1]] = int(m[2])
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            values[m[1]] = values[m[2]] + '.' + m[3]
        if m := re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)', line):
            assert values[m[1]] not in nets, 'duplicate wrapper driver'
            nets[values[m[1]]] = values[m[2]]
    return nets


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    read = equations(module(reference, 'ReadEgress')[1])
    for name, source in {'clock': 'clock', 'reset': 'reset', 'io_enq_valid': 'io_enq_valid',
                         'io_enq_bits_data': 'io_enq_bits_data', 'io_enq_bits_last': 'io_enq_bits_last',
                         'io_enqAddr': 'io_enq_bits_id'}.items():
        assert read['multiQueue_' + name] == source
    assert read['io_resp_tBits_data'] == 'multiQueue_io_deq_bits_data'
    assert read['io_resp_tBits_last'] == 'multiQueue_io_deq_bits_last'
    ref_queue = module(reference, 'MultiQueue')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(ref_queue).items()}
    expected = {'do_enq': 'io_enq_ready & io_enq_valid', 'do_deq': 'io_deq_ready & io_deq_valid',
                'io_enq_ready': '~full', 'io_deq_valid': 'deqValid',
                'deqPtr': 'do_deq & deqAddrReg == io_deqAddr ? _deqPtr_T_1 : _GEN_261',
                'empty': 'do_deq & deqAddrReg == io_deqAddr ? _deqPtr_T_1 == _GEN_277 : _GEN_309 & ~_GEN_293',
                'ram_data_io_deq_bits_MPORT_addr': '{io_deqAddr,deqPtr}',
                'ram_last_io_deq_bits_MPORT_addr': '{io_deqAddr,deqPtr}',
                'ram_data_MPORT_addr': '{io_enqAddr,_GEN_47}',
                'ram_last_MPORT_addr': '{io_enqAddr,_GEN_47}'}
    for name, value in expected.items(): assert eq[name] == value, name
    # Resolve SFC's lowered vector selectors rather than trusting generated
    # _GEN names: prefetch reads current ID, retirement advances delayed ID.
    def selector(name, address, prefix):
        for i in range(15, 0, -1):
            match = re.fullmatch(r"4'h([0-9a-f]+) == " + address + r" \? " + prefix +
                                 r"_(\d+) : (\w+)", eq[name])
            assert match and int(match[1], 16) == i and int(match[2]) == i, name
            name = match[3]
        assert name == prefix + '_0'
    for name, address, prefix in [('_GEN_261', 'io_deqAddr', 'deqPtrs'),
                                  ('_GEN_277', 'io_deqAddr', 'enqPtrs'),
                                  ('_GEN_293', 'io_deqAddr', 'maybe_full'),
                                  ('_GEN_309', 'io_deqAddr', 'ptr_matches'),
                                  ('_GEN_47', 'io_enqAddr', 'enqPtrs'),
                                  ('_GEN_101', 'deqAddrReg', 'deqPtrs'),
                                  ('_GEN_15', 'io_enqAddr', 'ptr_matches'),
                                  ('_GEN_31', 'io_enqAddr', 'maybe_full')]:
        selector(name, address, prefix)
    for i in range(16): assert eq[f'ptr_matches_{i}'] == f'enqPtrs_{i} == deqPtrs_{i}'
    assert eq['full'] == '_GEN_15 & _GEN_31'
    assert eq['_deqPtr_T_1'] == "_GEN_261 + 3'h1"
    assert re.search(r'reg \[63:0\] ram_data \[0:127\]', ref_queue)
    assert re.search(r'reg\s+ram_last \[0:127\]', ref_queue)
    ref_resets = dict(re.findall(r'if \(reset\) begin\s*(\w+) <= (\d+)\x27h0;', ref_queue))
    wanted = {**{f'{name}_{i}': str(width) for name, width in [('enqPtrs', 3), ('deqPtrs', 3), ('maybe_full', 1)]
                 for i in range(16)}, 'deqValid': '1'}
    assert ref_resets == wanted
    assert re.search(r'deqAddrReg <= io_deqAddr;', ref_queue)
    assert re.search(r'deqValid <= ~empty;', ref_queue)
    # These synchronous reads and writes are outside the reset branches.
    assert ref_queue.index('ram_data[ram_data_MPORT_addr] <=') < ref_queue.index('if (reset) begin')
    assert ref_queue.index('ram_last[ram_last_MPORT_addr] <=') < ref_queue.index('if (reset) begin')
    baseline = (evidence / 'candidate/post-fame-fased-read-buffer.mlir').read_text()
    baseline_queue = native_module(baseline, 'GGFASEDReadBuffer16x8')
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-read-buffer{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-issue{suffix}.mlir').read_text()
        # Retain every earlier module definition, including banks and bindings.
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[name] == body for name, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDReadBuffer16x8', 'GGFASEDReadBufferWrapper'}
        for key in ('globalName', 'widgetClass', 'class'):
            pattern = key + r' = "([^"]+)"'
            assert sorted(re.findall(pattern, text)) == sorted(re.findall(pattern, before)), key + ' catalog changed'
        # Compare the full annotation archive after the actual top/port rename,
        # retaining opaque records and nested channel identities byte for byte.
        old, new = 'GGFASEDIngressIssueWrapper', 'GGFASEDReadBufferWrapper'
        expected_header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        old_ports = re.findall(r'\b(?:in|out) %(\w+):', native_module(before, old).splitlines()[0])
        for name in old_ports:
            if name != 'fased_host_responses':
                expected_header = expected_header.replace('~' + old + '|' + old + '>' + name,
                                                          '~' + old + '|' + new + '>' + name)
        for old_leaf, new_leaf in [('rReady', 'fased_host_read_response.ready'),
                                   ('rValid', 'fased_host_read_response.valid'),
                                   ('rLast', 'fased_host_read_response.bits.last'),
                                   ('bReady', 'fased_host_write_responses.bReady'),
                                   ('bValid', 'fased_host_write_responses.bValid')]:
            expected_header = expected_header.replace('~' + old + '|' + old + '>fased_host_responses.' + old_leaf,
                                                      '~' + old + '|' + new + '>' + new_leaf)
        expected_header = expected_header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == expected_header, 'annotation identity/retarget differs'
        queue = native_module(text, 'GGFASEDReadBuffer16x8')
        assert queue == baseline_queue, 'expanded queue differs from fresh native baseline'
        memory = [line for line in queue.splitlines() if ' = firrtl.mem ' in line]
        assert len(memory) == 1 and all(x in memory[0] for x in
            ['depth = 128 : i64', 'readLatency = 1 : i32', 'writeLatency = 1 : i32', 'data flip: uint<65>', 'data: uint<65>'])
        resets = {name: width for name, width in re.findall(
            r'%(\w+) = firrtl.regreset %clock, %reset, %\w+[^\n]*!firrtl.uint<(\d+)>$', queue, re.M)}
        assert resets == wanted
        constants = dict(re.findall(r'(%\w+) = firrtl.constant (\d+)', queue))
        for reset in re.findall(r'firrtl.regreset %clock, %reset, (%\w+)', queue):
            assert constants[reset] == '0', 'pointer/valid reset value differs'
        assert re.search(r'%deqAddrReg = firrtl.reg %clock.*?!firrtl.uint<4>', queue)
        wrapper = native_module(text, 'GGFASEDReadBufferWrapper')
        nets = wrapper_wiring(wrapper)
        expected = {'readBuffer.clock': 'top.hostClock', 'readBuffer.reset': 'sim.fased_egress_reset',
                    'top.fased_host_read_response.ready': 1,
                    'sim.fased_host_responses.rReady': 'top.fased_host_read_response.ready',
                    'sim.fased_host_responses.rValid': 'top.fased_host_read_response.valid',
                    'sim.fased_host_responses.rLast': 'top.fased_host_read_response.bits.last',
                    'readBuffer.enq.valid': 'top.fased_host_read_response.valid',
                    'readBuffer.enq.bits.data': 'top.fased_host_read_response.bits.data',
                    'readBuffer.enq.bits.last': 'top.fased_host_read_response.bits.last',
                    'readBuffer.enqAddr': 'top.fased_host_read_response.bits.id',
                    'readBuffer.deqAddr': 'top.fased_read_buffer_address',
                    'top.fased_read_buffer_deq': 'readBuffer.deq'}
        for field in ('bReady', 'bValid'):
            expected['sim.fased_host_responses.' + field] = 'top.fased_host_write_responses.' + field
        assert all(nets[name] == value for name, value in expected.items()), 'read egress wiring differs'
        assert 'fased_host_responses' not in wrapper.splitlines()[0], 'consumed port remains external'
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'reverse': reverse, 'existing_modules_retained': len(prior),
                        'queues': 16, 'beats_per_id': 8, 'memory_width': 65, 'reset_registers': len(resets),
                        'wrapper_connections_compared': len(expected)})
    report = {'golden': str(golden), 'SFC_modules': ['ReadEgress', 'MultiQueue'],
              'constructor_orders': reports, 'reference_has_print_host': False,
              'scope': 'per-ID host read buffering; address/dequeue scheduler and MMIO remain external'}
    (evidence / 'rocket-fased-read-buffer-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC per-ID synchronous memory/reset geometry, payload/clock/reset wiring and preserved expanded Print banks in both orders')


if __name__ == '__main__': main()
