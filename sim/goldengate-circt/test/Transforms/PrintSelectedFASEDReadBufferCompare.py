#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print response storage with immutable SFC ReadEgress.

The shared queue's executable FIRRTL interpreter covers legacy and selected
roots; this comparison pins the SFC memory/select/reset boundary and actual
candidate composition, targets, host wiring and emitted RTL in both allocations.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDReadBufferCompare import wrapper_wiring


def reference_boundary(golden):
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
    assert eq['_enqPtrs_T_1'] == "_GEN_47 + 3'h1"
    assert eq['_deqPtrs_T_3'] == "_GEN_101 + 3'h1"
    for i in range(16):
        ident = format(i, 'x')
        assert eq[f'_GEN_{166+i}'] == f"4'h{ident} == io_enqAddr | maybe_full_{i}"
        assert eq[f'_GEN_{182+i}'] == f"do_enq ? _GEN_{166+i} : maybe_full_{i}"
        expected_full = f"""if (reset) begin maybe_full_{i} <= 1'h0;
          end else if (io_enqAddr == deqAddrReg) begin
          if (do_enq != do_deq) begin if (4'h{ident} == io_enqAddr) begin
          maybe_full_{i} <= do_enq; end end end else if (do_deq) begin
          if (4'h{ident} == deqAddrReg) begin maybe_full_{i} <= 1'h0;
          end else begin maybe_full_{i} <= _GEN_{182+i}; end end else begin
          maybe_full_{i} <= _GEN_{182+i}; end"""
        assert ' '.join(expected_full.split()) in ' '.join(ref_queue.split()), 'per-ID full update differs'
        for prefix, address, update in [('enqPtrs', 'io_enqAddr', '_enqPtrs_T_1'),
                                         ('deqPtrs', 'deqAddrReg', '_deqPtrs_T_3')]:
            pulse = 'do_enq' if prefix == 'enqPtrs' else 'do_deq'
            expected_ptr = f"""if (reset) begin {prefix}_{i} <= 3'h0; end else if ({pulse}) begin
              if (4'h{ident} == {address}) begin {prefix}_{i} <= {update}; end end"""
            assert ' '.join(expected_ptr.split()) in ' '.join(ref_queue.split()), 'per-ID pointer update differs'
    for field in ('data', 'last'):
        assert eq[f'ram_{field}_io_deq_bits_MPORT_en'] == "1'h1"
        assert eq[f'ram_{field}_MPORT_mask'] == "1'h1"
        assert eq[f'ram_{field}_MPORT_en'] == 'io_enq_ready & io_enq_valid'
        assert eq[f'ram_{field}_MPORT_data'] == 'io_enq_bits_' + field
    return wanted


def transfer(value, old, new, copied):
    if isinstance(value, list): return [transfer(v, old, new, copied) for v in value]
    if isinstance(value, dict): return {k: transfer(v, old, new, copied) for k, v in value.items()}
    if not isinstance(value, str): return value
    prefix = '~' + old + '|'
    if value == '~' + old: return '~' + new
    if not value.startswith(prefix): return value
    result = value[len(prefix):]
    target = old + '>'
    if result.startswith(target):
        ref = result[len(target):]
        root = re.split(r'[.\[]', ref)[0]
        if root in copied: result = new + '>' + ref
        for leaf, destination in [('rReady', 'fased_host_read_response.ready'),
                                  ('rValid', 'fased_host_read_response.valid'),
                                  ('rLast', 'fased_host_read_response.bits.last'),
                                  ('bReady', 'fased_host_write_responses.bReady'),
                                  ('bValid', 'fased_host_write_responses.bValid')]:
            if ref == 'fased_host_responses.' + leaf: result = new + '>' + destination
    return '~' + new + '|' + result


def main():
    evidence, golden, baseline_path = (Path(p).resolve() for p in sys.argv[1:])
    wanted = reference_boundary(golden)
    baseline = native_module(baseline_path.read_text(), 'GGFASEDReadBuffer16x8')
    old, new = 'GGSelectedPrintFASEDIngressIssueWrapper', 'GGSelectedPrintFASEDReadBufferWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        directory = evidence / allocation
        before = (directory / 'post-print-fased-ingress-deadlock.mlir').read_text()
        text = (directory / 'post-print-fased-read-buffer.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after.get(n) == b for n, b in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDReadBuffer16x8', new}
        queue = native_module(text, 'GGFASEDReadBuffer16x8')
        assert queue == baseline, 'selected queue differs from oracle-compared native baseline'
        memory = [l for l in queue.splitlines() if ' = firrtl.mem ' in l]
        assert len(memory) == 1 and all(x in memory[0] for x in
            ['depth = 128 : i64', 'readLatency = 1 : i32', 'writeLatency = 1 : i32',
             'data flip: uint<65>', 'data: uint<65>'])
        resets = dict(re.findall(r'%(\w+) = firrtl.regreset %clock, %reset, %\w+[^\n]*!firrtl.uint<(\d+)>$', queue, re.M))
        assert resets == wanted
        constants = dict(re.findall(r'(%\w+) = firrtl.constant (\d+)', queue))
        assert all(constants[v] == '0' for v in re.findall(r'firrtl.regreset %clock, %reset, (%\w+)', queue))
        assert re.search(r'%deqAddrReg = firrtl.reg %clock.*?!firrtl.uint<4>', queue)
        copied = [n for d, n, t in native_ports(native_module(before, old)) if n != 'fased_host_responses']
        wrapper = native_module(text, new)
        assert native_ports(wrapper)[:len(copied)] == [p for p in native_ports(native_module(before, old)) if p[1] in copied]
        assert [p[1] for p in native_ports(wrapper)[len(copied):]] == [
            'fased_host_write_responses', 'fased_host_read_response', 'fased_read_buffer_address', 'fased_read_buffer_deq']
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
        for leaf in ('bReady', 'bValid'):
            expected['sim.fased_host_responses.' + leaf] = 'top.fased_host_write_responses.' + leaf
        for direction, name, typ in native_ports(native_module(before, old)):
            if name in copied:
                dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
                expected[dest] = src
        assert nets == expected, 'selected response wrapper wiring differs'
        raw_before = json.loads((directory / 'post-print-fased-ingress-deadlock-all.json').read_text())
        raw_after = json.loads((directory / 'post-print-fased-read-buffer-all.json').read_text())
        assert transfer(raw_before, old, new, copied) == raw_after, 'annotation archive/target transfer differs'
        emitted = clean((directory / 'post-print-fased-read-buffer.sv').read_text())
        rtl_queue = module(emitted, 'GGFASEDReadBuffer16x8')[1]
        storage_header, storage = module(emitted, 'ram_128x65')
        assert re.search(r'reg \[64:0\] Memory\[0:127\];', storage), 'emitted storage geometry differs'
        assert 'reset' not in storage_header and 'reset' not in storage, 'payload memory unexpectedly reset'
        assert re.search(r'always @\(posedge R0_clk\) begin\s*_R0_en_d0 <= R0_en;\s*_R0_addr_d0 <= R0_addr;', storage)
        assert equations(storage)['R0_data'] == "_R0_en_d0 ? Memory[_R0_addr_d0] : 65'bx"
        assert re.search(r'always @\(posedge W0_clk\) begin\s*if \(W0_en & 1\x27h1\)\s*Memory\[W0_addr\] <= W0_data;', storage)
        assert re.search(r'\.R0_clk\s*\(clock\)', rtl_queue) and re.search(r'\.W0_clk\s*\(clock\)', rtl_queue)
        assert re.search(r'\.R0_en\s*\(1\x27h1\)', rtl_queue)
        assert re.search(r'\.W0_data\s*\(\{enq_bits_data, enq_bits_last\}\)', rtl_queue)
        assert re.search(r'deqAddrReg <= deqAddr;', rtl_queue), 'read ID update differs'
        reset_block = re.search(r'if \(reset\) begin(.*?)end\s*else begin', rtl_queue, re.S)[1]
        assert dict(re.findall(r'(\w+) <= (\d+)\x27h0;', reset_block)) == wanted
        assert 'deqAddrReg' not in reset_block
        assert re.search(r'if \(reset\)', rtl_queue) and 'deqValid <=' in rtl_queue
        rtl_wrapper = module(emitted, new)[1]
        assert re.search(r'assign fased_host_read_response_ready = 1\x27h1;', rtl_wrapper)
        assert re.search(r'\.reset\s*\(_sim_fased_egress_reset\)', rtl_wrapper)
        previous = evidence.parent / 'iteration146-print-fased-deadlock' / allocation
        for collateral in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (directory / collateral).read_bytes() == (previous / collateral).read_bytes(), 'allocated collateral changed'
        reports.append({'allocation': allocation, 'existing_modules_retained': len(prior),
            'queues': 16, 'beats_per_id': 8, 'payload_bits': 65, 'reset_registers': len(resets),
            'wrapper_connections_compared': len(expected), 'annotation_records_retained': len(raw_after),
            'allocated_headers_unchanged': True})
    report = {'golden': str(golden), 'SFC_modules': ['ReadEgress', 'MultiQueue'],
        'allocations': reports, 'reference_has_print_host': False,
        'scope': 'non-ROB read storage; request scheduler, target response/token wiring remain external'}
    (evidence / 'read-buffer-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC ReadEgress/MultiQueue storage, selects, reset, wiring, annotations and headers in both allocations')


if __name__ == '__main__': main()
