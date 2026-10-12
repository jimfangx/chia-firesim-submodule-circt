#!/usr/bin/env python3
# See LICENSE for license details.
"""Check the live Print/TracerV queue against SFC Queue_50 and prior native IR.

Evaluate emitted pointer/read-address state and handshake/count/write signals.
RAM geometry and connections are checked structurally; the native CTest checks
payload ordering, reset writes and read/write collisions through actual MemOps.
This comparison does not establish enabled-Print platform/runtime equivalence.
"""
import itertools
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module, without_targets
from PrintControlMasterCompare import ports
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintTracerVTriggerCompare import Parser, RTL, parsed_expression


class QueueRTL(RTL):
    def __init__(self, header, body, reference=False):
        # Both emitters use unsigned logic. Normalize hexadecimal case only.
        body = re.sub(r"\d+'h[0-9a-fA-F]+", lambda m: m[0].lower(), body)
        self.widths = {name: geometry[1] for name, geometry in ports(header).items()}
        self.registers = {}
        for kind, hi, lo, name in re.findall(r'\b(reg|wire|logic)\s*(?:\[(\d+):(\d+)\])?\s+(\w+)', body):
            width = int(hi) - int(lo) + 1 if hi else 1
            self.widths[name] = width
            if kind == 'reg' and name != 'ram' and not name.startswith('_RAND'):
                self.registers[name] = width
        self.nets = {name: parsed_expression(expr) for name, expr in equations(body).items()
                     if 'ram[' not in expr and name != 'io_deq_bits'}
        blocks = re.findall(r'always\s*@\(posedge clock\)\s*(begin.*?\n  end)', body, re.S)
        assert len(blocks) == 1
        block = blocks[0]
        if reference:
            # Check the entire SFC memory write before removing the array
            # assignment from the scalar statement evaluator.
            write = r'if \(ram_MPORT_en & ram_MPORT_mask\) begin\s*ram\[ram_MPORT_addr\] <= ram_MPORT_data;\s*end'
            assert len(re.findall(write, block)) == 1
            block = re.sub(write, '', block)
            assert re.search(r'reg \[511:0\] ram \[0:6143\]', body)
        else:
            block = re.sub(r'automatic logic\s*(?:\[[^]]+\])?\s+\w+;', '', block)
            for name, expr in re.findall(r'^\s*(\w+) = ([^;]+);', block, re.M):
                self.nets[name] = parsed_expression(expr)
            block = re.sub(r'^\s*\w+ = [^;]+;', '', block, flags=re.M)
        parser = Parser(block); self.clocked = parser.statement()
        assert parser.i == len(parser.tokens)


def main():
    candidate, before, native, golden, report = map(Path, sys.argv[1:])
    text = (candidate / 'post-print-host-binding.mlir').read_text()
    prior = before.read_text()
    names = re.findall(r'^    firrtl.module @(\w+)\(', prior, re.M)
    for name in names:
        assert native_module(text, name) == native_module(prior, name), name
    added = set(re.findall(r'^    firrtl.module @(\w+)\(', text, re.M)) - set(names)
    assert added == {'GGTracerVStreamQueue6144', 'GGPrintTracerVStreamQueueWrapper'}
    queue = native_module(text, 'GGTracerVStreamQueue6144')
    assert queue == native_module(native.read_text(), 'GGTracerVStreamQueue6144')
    assert 'goldengate.ramStyle = "ULTRA"' in queue
    for entry in ('depth = 6144', 'readLatency = 0', 'writeLatency = 1'):
        assert entry in queue
    assert 'goldengate.streamParameters = {depth = 6144 : i64, index = 0 : i64' in queue
    annos = json.loads((candidate / 'post-print-host-binding-all.json').read_text())
    old = json.loads(before.with_name('post-print-host-binding-all.json').read_text())
    assert without_targets(annos) == without_targets(old)
    clocks = lambda rows: [a for a in rows if a['class'] == 'midas.passes.ChannelClockInfoAnnotation']
    assert clocks(annos) == clocks(old)
    bridge = next(a for a in annos if a.get('widgetClass', '').endswith('TracerVBridgeModule'))
    assert bridge['target'] == '~GGPrintTracerVStreamQueueWrapper|GGTracerVTokenEngine>hPort'
    wrapper = 'GGPrintTracerVStreamQueueWrapper'
    wrapper_ir = native_module(text, wrapper)
    nets = wiring(wrapper_ir, wrapper, aggregate=True)
    fifo = 'TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q'
    assert nets[fifo + '.enq'] == 'sim.tracerv_stream'
    assert nets['top.tracerv_stream'] == fifo + '.deq'
    assert nets[fifo + '.clock'] == 'top.hostClock' and nets[fifo + '.reset'] == 'top.hostReset'
    assert nets['top.tracerv_stream_count'] == fifo + '.count'
    copied_ports = re.findall(r'\b(in|out) %(\w+):', wrapper_ir.splitlines()[0])
    assert copied_ports
    for direction, port in copied_ports:
        if port in ('tracerv_stream', 'tracerv_stream_count'): continue
        dest, src = ('sim.', 'top.') if direction == 'in' else ('top.', 'sim.')
        assert nets[dest + port] == src + port, port
    rtl = clean((candidate / 'post-print-host-binding.sv').read_text())
    _, body = module(rtl, wrapper)
    queue_ports = instance(body, 'GGTracerVStreamQueue6144', fifo)
    producer = instance(body, 'GGTracerVTriggerWrapper', 'sim')
    assert queue_ports['clock'] == 'hostClock' and queue_ports['reset'] == 'hostReset'
    assert queue_ports['count'] == 'tracerv_stream_count'
    for field in ('ready', 'valid', 'bits'):
        assert queue_ports['enq_' + field] == producer['tracerv_stream_' + field]
        assert queue_ports['deq_' + field] == 'tracerv_stream_' + field
    h, b = module(rtl, 'GGTracerVStreamQueue6144')
    memory = instance(b, 'ram_6144x512', 'ram_ext')
    assert memory == dict(R0_addr='ram_read_addr', R0_en="1'h1", R0_clk='clock',
                         R0_data='deq_bits', W0_addr='enq_ptr_value', W0_en='_GEN_2',
                         W0_clk='clock', W0_data='enq_bits')
    actual = QueueRTL(h, b)
    reference = clean(golden.read_text())
    instance(module(reference, 'CPUManagedStreamEngine')[1], 'Queue_50', fifo)
    oracle = QueueRTL(*module(reference, 'Queue_50'), reference=True)
    assert oracle.nets['ram_MPORT_mask'] == ('leaf', "1'h1")
    assert oracle.nets['ram_io_deq_bits_MPORT_en'] == ('leaf', "1'h1")
    aliases = {'ram_read_addr': 'ram_io_deq_bits_MPORT_addr'}
    assert len(actual.registers) == 4
    for name, width in actual.registers.items(): assert oracle.registers[aliases.get(name, name)] == width
    actual.inputs = {}
    assert actual.eval(parsed_expression("13'(13'h1fff + 13'h1)")) == (13, 0)
    rng = random.Random(129); comparisons = states = 0
    pointers = (0, 1, 2, 6142, 6143)
    cases = list(itertools.product(pointers, pointers, range(2), range(2), range(2), range(2)))
    cases += [(rng.randrange(6144), rng.randrange(6144), rng.getrandbits(1),
               rng.getrandbits(1), rng.getrandbits(1), rng.getrandbits(1)) for _ in range(4096)]
    for enq, deq, full, reset, valid, ready in cases:
        address, data = rng.randrange(6144), rng.getrandbits(512)
        a = dict(enq_ptr_value=enq, deq_ptr_value=deq, maybe_full=full, ram_read_addr=address,
                 reset=reset, enq_valid=valid, deq_ready=ready, enq_bits=data)
        r = dict(enq_ptr_value=enq, deq_ptr_value=deq, maybe_full=full, ram_io_deq_bits_MPORT_addr=address,
                 reset=reset, io_enq_valid=valid, io_deq_ready=ready, io_enq_bits=data)
        an, rn = actual.edge(a), oracle.edge(r)
        for name in actual.registers:
            assert an[name] == rn[aliases.get(name, name)], (states, name)
            comparisons += 1
        for output, expected in [('enq_ready', 'io_enq_ready'), ('deq_valid', 'io_deq_valid'),
                                 ('count', 'io_count'), (memory['W0_en'], 'ram_MPORT_en'),
                                 (memory['W0_addr'], 'ram_MPORT_addr'), (memory['W0_data'], 'ram_MPORT_data')]:
            assert actual.eval(('leaf', output)) == oracle.eval(('leaf', expected)), (states, output)
            comparisons += 1
        states += 1
    report.write_text(json.dumps(dict(golden=str(golden.resolve()), golden_module='Queue_50',
        matching_states=states, matching_state_and_signal_samples=comparisons,
        unchanged_modules=len(names), unchanged_annotation_records=len(annos), clock_map_preserved=True,
        memory_geometry='6144x512, unreset synchronous read-address capture, reset-independent writes',
        boundary='live Print/TracerV queue; CPU allocation and platform MMIO composition pending'), indent=2) + '\n')
    print(f'PASS {comparisons} SFC queue next-state/handshake/count/write samples; {len(names)} modules preserved')


if __name__ == '__main__': main()
