#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare attached Print/TracerV trigger state with immutable SFC emitted RTL.

The evaluator accepts only the unsigned expression/clocked-statement subset
used by these two register banks, and rejects unparsed syntax. It evaluates
pre-edge state on both sides, independently of the native MLIR interpreter.
"""
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module, without_targets
from PrintControlMasterCompare import ports
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring


class Parser:
    precedence = {'||': 1, '&&': 2, '|': 3, '^': 4, '&': 5,
                  '==': 6, '!=': 6, '<': 7, '>': 7, '<=': 7, '>=': 7,
                  '<<': 8, '>>': 8, '+': 9, '-': 9}

    def __init__(self, text):
        self.tokens = re.findall(r"\d+'[hdb][0-9a-f]+|\w+|<=|>=|==|!=|&&|\|\||<<|>>|[^\s]", text)
        self.i = 0

    def take(self, expected=None):
        token = self.tokens[self.i]; self.i += 1
        assert expected is None or token == expected, (expected, token)
        return token

    def peek(self):
        return self.tokens[self.i] if self.i < len(self.tokens) else ''

    def expression(self, minimum=0):
        token = self.take()
        if token == '(':
            node = self.expression(); self.take(')')
        elif token in ('~', '!', '|', '&', '^'):
            node = ('unary', token, self.expression(10))
        elif token == '{':
            fields = [self.expression()]
            while self.peek() == ',':
                self.take(); fields.append(self.expression())
            self.take('}'); node = ('cat', fields)
        else:
            assert re.fullmatch(r"\w+|\d+'[hdb][0-9a-f]+", token), token
            node = ('leaf', token)
        while self.peek() == '[':
            self.take(); hi = int(self.take()); lo = hi
            if self.peek() == ':': self.take(); lo = int(self.take())
            self.take(']'); node = ('slice', node, hi, lo)
        while self.peek() in self.precedence and self.precedence[self.peek()] >= minimum:
            op = self.take(); right = self.expression(self.precedence[op] + 1)
            node = ('binary', op, node, right)
        if minimum == 0 and self.peek() == '?':
            self.take(); yes = self.expression(); self.take(':'); no = self.expression()
            node = ('mux', node, yes, no)
        return node

    def statement(self):
        token = self.take()
        if token == 'begin':
            body = []
            while self.peek() != 'end': body.append(self.statement())
            self.take('end'); return ('block', body)
        if token == 'if':
            self.take('('); condition = self.expression(); self.take(')')
            yes = self.statement(); no = ('block', [])
            if self.peek() == 'else': self.take(); no = self.statement()
            return ('if', condition, yes, no)
        self.take('<='); value = self.expression(); self.take(';')
        return ('assign', token, value)


def parsed_expression(text):
    parser = Parser(text); result = parser.expression()
    assert parser.i == len(parser.tokens), text
    return result


class RTL:
    def __init__(self, header, body):
        self.widths = {name: geometry[1] for name, geometry in ports(header).items()}
        self.registers = {}
        for kind, hi, lo, name in re.findall(r'\b(reg|wire)\s*(?:\[(\d+):(\d+)\])?\s+(\w+)', body):
            width = int(hi) - int(lo) + 1 if hi else 1
            self.widths[name] = width
            if kind == 'reg': self.registers[name] = width
        self.nets = {name: parsed_expression(expr) for name, expr in equations(body).items()}
        clocked = re.findall(r'always(?:_ff)?\s*@\(posedge clock\)\s*(begin.*?\n  end)', body, re.S)
        clocked = [block for block in clocked if re.search(r'\binitDone\s*<=', block)]
        assert len(clocked) == 1
        # Assertions have no register writes. Their write-only policy is
        # covered by the native bank test; exclude only explicit error calls.
        block = clocked[0].split('`ifndef SYNTHESIS')[0]
        if block != clocked[0]: block += ' end'
        block = re.sub(r'assert\([^;]+\) else \$error\("[^"]*"\);', '', block)
        parser = Parser(block); self.clocked = parser.statement()
        assert parser.i == len(parser.tokens)

    def eval(self, node):
        kind = node[0]
        if kind == 'leaf':
            token = node[1]
            if token in self.inputs: return self.widths[token], self.inputs[token]
            if token in self.nets:
                width, value = self.eval(self.nets[token])
                width = self.widths.get(token, width)
                return width, value & ((1 << width) - 1)
            literal = re.fullmatch(r"(\d+)'([hdb])([0-9a-f]+)", token)
            assert literal, token
            return int(literal[1]), int(literal[3], {'h': 16, 'd': 10, 'b': 2}[literal[2]])
        if kind == 'slice':
            _, value = self.eval(node[1]); width = node[2] - node[3] + 1
            return width, (value >> node[3]) & ((1 << width) - 1)
        if kind == 'cat':
            width = value = 0
            for field in node[1]:
                fw, fv = self.eval(field); width += fw; value = (value << fw) | fv
            return width, value
        if kind == 'mux':
            _, predicate = self.eval(node[1]); yes = self.eval(node[2]); no = self.eval(node[3])
            return max(yes[0], no[0]), yes[1] if predicate else no[1]
        if kind == 'unary':
            width, value = self.eval(node[2]); op = node[1]
            if op == '~': return width, value ^ ((1 << width) - 1)
            return 1, int(not value) if op == '!' else int(bool(value)) if op == '|' else int(value == (1 << width) - 1) if op == '&' else value.bit_count() % 2
        op = node[1]; aw, a = self.eval(node[2]); bw, b = self.eval(node[3])
        width = max(aw, bw)
        if op in ('==', '!=', '<=', '>=', '<', '>'):
            return 1, int({'==': a == b, '!=': a != b, '<=': a <= b, '>=': a >= b, '<': a < b, '>': a > b}[op])
        if op in ('&&', '||'): return 1, int(bool(a) and bool(b)) if op == '&&' else int(bool(a) or bool(b))
        value = {'&': lambda: a & b, '|': lambda: a | b, '^': lambda: a ^ b,
                 '+': lambda: a + b, '-': lambda: a - b,
                 '<<': lambda: a << b, '>>': lambda: a >> b}[op]()
        return width, value & ((1 << width) - 1)

    def edge(self, inputs):
        self.inputs = inputs; result = dict(inputs)
        def execute(node):
            if node[0] == 'block':
                for statement in node[1]: execute(statement)
            elif node[0] == 'if': execute(node[2] if self.eval(node[1])[1] else node[3])
            else:
                name = node[1]; result[name] = self.eval(node[2])[1] & ((1 << self.registers[name]) - 1)
        execute(self.clocked)
        return result


def main():
    candidate, prior_path, native_path, golden, report = map(Path, sys.argv[1:])
    text = (candidate / 'post-print-host-binding.mlir').read_text()
    prior = prior_path.read_text(); native = native_path.read_text()
    names = re.findall(r'^    firrtl.module @(\w+)\(', prior, re.M)
    for name in names:
        if name != 'GGTracerVTokenWrapper': assert native_module(text, name) == native_module(prior, name), name
    assert native_module(text, 'GGTracerVTriggerConfig') == native_module(native, 'GGTracerVTriggerConfig')
    assert text.count('firrtl.module @GGTracerVTriggerConfig(') == 1
    assert set(re.findall(r'^    firrtl.module @(\w+)\(', text, re.M)) - set(names) == {'GGTracerVTriggerWrapper'}
    original_annos = json.loads(prior_path.with_name('post-print-host-binding-all.json').read_text())
    annos = json.loads((candidate / 'post-print-host-binding-all.json').read_text())
    assert without_targets(annos) == without_targets(original_annos)
    maps = lambda rows: [row for row in rows if row['class'] == 'midas.passes.ChannelClockInfoAnnotation']
    assert maps(annos) == maps(original_annos)
    bridge = next(a for a in annos if a.get('widgetClass', '').endswith('TracerVBridgeModule'))
    assert bridge['target'] == '~GGTracerVTriggerWrapper|GGTracerVTokenEngine>hPort'
    net = wiring(native_module(text, 'GGTracerVTriggerWrapper'), 'GGTracerVTriggerWrapper', aggregate=True)
    assert net['tracervRegisters.control'] == 'sim.tracerv_control'
    assert net['tracervRegisters.trace'] == 'sim.tracerv_trace'
    assert net['top.tracerv_mcr'] == 'tracervRegisters.mcr'
    assert net['tracervRegisters.clock'] == 'top.hostClock' and net['tracervRegisters.reset'] == 'top.hostReset'
    rtl = clean((candidate / 'post-print-host-binding.sv').read_text())
    wrapper = module(rtl, 'GGTracerVTriggerWrapper')[1]
    registers = instance(wrapper, 'GGTracerVTriggerConfig', 'tracervRegisters')
    inner = instance(wrapper, 'GGTracerVTokenWrapper', 'sim')
    for field in ('initDone', 'traceEnable', 'trigger', 'tCycle'):
        assert registers['control_' + field] == inner['tracerv_control_' + field]
    for field in ('valid', 'iaddr', 'insn'):
        assert registers['trace_' + field] == inner['tracerv_trace_' + field]
    assert registers['clock'] == 'hostClock' and registers['reset'] == 'hostReset'
    bank = RTL(*module(clean((candidate / 'post-print-host-binding.sv').read_text()), 'GGTracerVTriggerConfig'))
    oracle = RTL(*module(clean(golden.read_text()), 'TracerVBridgeModule'))
    assert len(bank.registers) == 22
    for name, width in bank.registers.items(): assert oracle.registers[name] == width
    for expression, expected in (("2'h2 == 2'h2 | 1'h0", 1),
                                 ("1'h0 ? 4'hf : 4'h3", 3),
                                 ("~1'h0 & 1'h1", 1), ("(|4'h8)", 1)):
        bank.inputs = {}; assert bank.eval(parsed_expression(expression))[1] == expected
    rng = random.Random(127); samples = 0
    for i in range(4096):
        state = {name: rng.getrandbits(width) for name, width in oracle.registers.items()}
        state['triggerSelector'] = i % 6
        pc, insn, cycle = rng.getrandbits(40), rng.getrandbits(32), rng.getrandbits(64)
        # Exercise coincident boundaries, masked instruction matches and stalls.
        state['triggerPCStart'] = pc if i & 1 else pc ^ 1
        state['triggerPCEnd'] = pc if i & 2 else pc ^ 2
        state['hostTriggerStartInst'] = insn if i & 4 else insn ^ 1
        state['hostTriggerEndInst'] = insn if i & 8 else insn ^ 2
        state['hostTriggerStartInstMask'] = 0 if i & 16 else 0xffffffff
        state['hostTriggerEndInstMask'] = 0xffffffff
        state['trace_cycle_counter'] = cycle
        state['triggerCycleCountStart'] = cycle if i & 32 else cycle ^ 1
        state['triggerCycleCountEnd'] = cycle if i & 64 else cycle ^ 2
        reset, valid, target_reset = int(i % 17 == 0), (i >> 7) & 1, (i >> 8) & 1
        reference = dict(state, reset=reset, hPort_hBits_tiletrace_reset=target_reset,
                         hPort_hBits_tiletrace_trace_retiredinsns_0_valid=valid,
                         hPort_hBits_tiletrace_trace_retiredinsns_0_iaddr=pc,
                         hPort_hBits_tiletrace_trace_retiredinsns_0_insn=insn,
                         hPort_toHost_hValid=0, hPort_fromHost_hReady=0, streamToHostCPU_ready=0)
        actual = {name: state[name] for name in bank.registers}
        actual.update(reset=reset, trace_valid=valid & (1 - target_reset), trace_iaddr=pc,
                      trace_insn=insn, control_tCycle=cycle)
        for slot in range(15):
            write, data = rng.getrandbits(1), rng.getrandbits(32)
            reference[f'crFile_io_mcr_write_{slot}_valid'] = write
            reference[f'crFile_io_mcr_write_{slot}_bits'] = data
            actual[f'mcr_write_{slot}_valid'] = write
            actual[f'mcr_write_{slot}_bits'] = data
        a, r = bank.edge(actual), oracle.edge(reference)
        for name in bank.registers:
            assert a[name] == r[name], (i, name, a[name], r[name]); samples += 1
        for out, expected in (('control_trigger', 'trigger'), ('control_initDone', 'initDone'), ('control_traceEnable', 'traceEnable')):
            assert bank.eval(('leaf', out))[1] == oracle.eval(('leaf', expected))[1]; samples += 1
        for slot in range(2):
            assert bank.eval(('leaf', f'mcr_read_{slot}_bits')) == oracle.eval(('leaf', f'crFile_io_mcr_read_{slot}_bits')); samples += 1
    report.write_text(json.dumps(dict(golden=str(golden.resolve()), matching_state_and_output_samples=samples,
        matching_trigger_registers=22, matching_mmio_words=15, unchanged_modules=len(names)-1,
        unchanged_annotation_records=len(annos), clock_map_preserved=True,
        boundary='live Print/TracerV triggers; stream queue and CPU/control composition pending'), indent=2) + '\n')
    print(f'PASS {samples} SFC next-state/control/read samples; 22 registers, Print hardware and analyzed clocks preserved')


if __name__ == '__main__': main()
