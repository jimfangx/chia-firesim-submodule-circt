#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare live Print/TracerV attachment with native IR and immutable SFC RTL.

This boundary leaves TracerV control and DMA storage explicit. It does not
establish complete enabled-Print platform or runtime equivalence.
"""
import itertools
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, without_targets
from PrintControlMasterCompare import ports
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring

candidate, before, native, golden, golden_annos, report = map(Path, sys.argv[1:])
ir = (candidate / 'post-print-host-binding.mlir').read_text()
prior = before.read_text()
source = native.read_text()
assert native_module(ir, 'GGTracerVTokenEngine') == native_module(source, 'GGTracerVTokenEngine')
originals = re.findall(r'^    firrtl.module @(\w+)\(', prior, re.M)
for name in originals:
    assert native_module(ir, name) == native_module(prior, name), name
assert set(re.findall(r'^    firrtl.module @(\w+)\(', ir, re.M)) - set(originals) == {
    'GGTracerVTokenEngine', 'GGTracerVTokenWrapper'}
annos = json.loads((candidate / 'post-print-host-binding-all.json').read_text())
old_annos = json.loads(before.with_name('post-print-host-binding-all.json').read_text())
bridge = next(a for a in annos if a.get('widgetClass', '').endswith('TracerVBridgeModule'))
trace_names = set(bridge['channelMapping'].values())
assert len(annos) == len(old_annos)
for actual, old in zip(annos, old_annos):
    normalized = dict(actual)
    if actual.get('globalName') in trace_names:
        for key in ('sources', 'sinks'):
            if key not in old: normalized.pop(key)
    assert without_targets(normalized) == without_targets(old)
oracle_bridge = next(a for a in json.loads(golden_annos.read_text())
                     if a.get('widgetClass', '').endswith('TracerVBridgeModule'))
assert bridge['widgetConstructorKey'] == oracle_bridge['widgetConstructorKey']
oracle_channels = {channel['name']: channel for channel in oracle_bridge['bridgeChannels']}
assert set(bridge['channelMapping']) == set(oracle_channels)
for local, channel in oracle_channels.items():
    assert channel['class'] == 'firesim.lib.bridgeutils.PipeBridgeChannel'
    assert channel['latency'] == 1
    target_side = 'sinks' if local.startswith('trigger') else 'sources'
    host_side = 'sources' if target_side == 'sinks' else 'sinks'
    assert len(channel[target_side]) == 1 and not channel[host_side]
assert bridge['target'] == '~GGTracerVTokenWrapper|GGTracerVTokenEngine>hPort'
clock_maps = [a for a in annos if a['class'] == 'midas.passes.ChannelClockInfoAnnotation']
assert len(clock_maps) == 1
assert clock_maps == [a for a in old_annos if a['class'] == clock_maps[0]['class']]
for local, global_name in bridge['channelMapping'].items():
    assert clock_maps[0]['infoMap'][global_name] == bridge['clockInfo']
    channel = next(a for a in annos if a.get('globalName') == global_name)
    assert channel['channelInfo'] == {'class': 'midas.passes.fame.PipeChannel', 'latency': 1}
    host_side = 'sources' if local.startswith('trigger') else 'sinks'
    target_side = 'sinks' if host_side == 'sources' else 'sources'
    assert channel[host_side] == [bridge['target'] + '.hBits.' + local]
    assert len(channel[target_side]) == 1 and '|GGPrintBridgeHostWrapper>' in channel[target_side][0]

# Decode actual SSA connections, including each token's excluded ready/valid
# predicate. Represent the wrapper's constant-one folds as a synthetic leaf.
wrapper = native_module(ir, 'GGTracerVTokenWrapper')
constants = re.findall(r'(%\w+) = firrtl.constant 1 : !firrtl.uint<1>', wrapper)
assert len(constants) == 2
wrapper = wrapper.replace('firrtl.module @GGTracerVTokenWrapper(',
                          'firrtl.module @GGTracerVTokenWrapper(' + ''.join(f'in {c}: !firrtl.uint<1>, ' for c in constants))
nets = wiring(wrapper, 'GGTracerVTokenWrapper', aggregate=True)
old_channels = {a['globalName']: a for a in old_annos if a.get('globalName') in trace_names}
tokens = {local: 'sim.' + old_channels[global_name]['sinks' if local.startswith('trigger') else 'sources'][0].split('>')[1].removesuffix('.bits')
          for local, global_name in bridge['channelMapping'].items()}
engine = 'TracerVBridgeModule_0.hPort'
for local, token in tokens.items():
    outgoing = local.startswith('trigger')
    group = [n for n in tokens if n.startswith('trigger') == outgoing]
    handshake = engine + ('.fromHost.hValid' if outgoing else '.toHost.hReady')
    actual = nets[token + ('.valid' if outgoing else '.ready')]
    expected = {handshake} | {tokens[n] + ('.ready' if outgoing else '.valid') for n in group if n != local}
    assert set(actual) == expected, local
    dest, src = (token + '.bits', engine + '.hBits.' + local) if outgoing else (engine + '.hBits.' + local, token + '.bits')
    assert nets[dest] == src, local
for outgoing in (False, True):
    dest = engine + ('.fromHost.hReady' if outgoing else '.toHost.hValid')
    expected = {'top.' + constants[int(outgoing)][1:]} | {token + ('.ready' if outgoing else '.valid') for local, token in tokens.items()
                                                        if local.startswith('trigger') == outgoing}
    assert set(nets[dest]) == expected

rtl = clean((candidate / 'post-print-host-binding.sv').read_text())
reference = clean(golden.read_text())
header, body = module(rtl, 'GGTracerVTokenEngine')
ref_header, ref_body = module(reference, 'TracerVBridgeModule')
actual_ports, reference_ports = ports(header), ports(ref_header)
for name, geometry in reference_ports.items():
    if name.startswith('hPort_'):
        assert actual_ports[name] == geometry
assert actual_ports['stream_bits'] == reference_ports['streamToHostCPU_bits'] == ['output', 512]
eq, ref_eq = equations(body), equations(ref_body)


def value(expression, nets, inputs, active=()):
    """Evaluate this boundary's scalar Boolean and sized concatenation nets."""
    expression = expression.strip()
    if expression in inputs:
        return inputs[expression]
    if expression in nets:
        assert expression not in active, 'cyclic RTL expression'
        return value(nets[expression], nets, inputs, active + (expression,))
    if literal := re.fullmatch(r"(\d+)'([hdb])([0-9a-f]+)", expression):
        width, base, number = literal.groups()
        return int(width), int(number, {'h': 16, 'd': 10, 'b': 2}[base])
    if expression.startswith('{') and expression.endswith('}'):
        fields, depth, start = [], 0, 1
        for i, char in enumerate(expression[1:-1], 1):
            if char == '{': depth += 1
            if char == '}': depth -= 1
            if char == ',' and depth == 0:
                fields.append(expression[start:i]); start = i + 1
        fields.append(expression[start:-1])
        width, number = 0, 0
        for field in fields:
            bits, part = value(field, nets, inputs, active)
            width += bits; number = (number << bits) | part
        return width, number
    substituted = re.sub(r'\b[A-Za-z_]\w*\b',
                         lambda m: str(value(m[0], nets, inputs, active)[1]), expression)
    assert re.fullmatch(r'[0-9\s&|~()]+', substituted), expression
    return 1, eval(' '.join(substituted.split()), {'__builtins__': {}}, {}) & 1


boolean_inputs = ['counter', 'triggerReg', 'hPort_hBits_tiletrace_reset',
                  'hPort_hBits_tiletrace_trace_retiredinsns_0_valid',
                  'hPort_toHost_hValid', 'hPort_fromHost_hReady', 'stream_ready',
                  'control_initDone', 'control_traceEnable', 'control_trigger']
rename = {'stream_ready': 'streamToHostCPU_ready', 'control_initDone': 'initDone',
          'control_traceEnable': 'traceEnable', 'control_trigger': 'trigger'}
outputs = {'hPort_toHost_hReady': 'hPort_toHost_hReady',
           'hPort_fromHost_hValid': 'hPort_fromHost_hValid',
           'hPort_hBits_triggerCredit': 'hPort_hBits_triggerCredit',
           'hPort_hBits_triggerDebit': 'hPort_hBits_triggerDebit',
           'stream_valid': 'streamToHostCPU_valid', 'stream_bits': 'streamToHostCPU_bits'}
rng = random.Random(126)
samples = 0
for bits in itertools.product((0, 1), repeat=len(boolean_inputs)):
    inputs = dict(zip(boolean_inputs, ((1, bit) for bit in bits)))
    inputs.update(hPort_hBits_tiletrace_trace_retiredinsns_0_iaddr=(40, rng.getrandbits(40)),
                  trace_cycle_counter=(64, rng.getrandbits(64)))
    ref_inputs = {rename.get(name, name): number for name, number in inputs.items()}
    for output, ref_output in outputs.items():
        assert value(output, eq, inputs) == value(ref_output, ref_eq, ref_inputs), (output, bits)
        samples += 1
report.write_text(json.dumps(dict(golden_rtl=str(golden.resolve()),
    golden_annotations=str(golden_annos.resolve()), unchanged_modules=len(originals),
    unchanged_annotation_records=len(annos), live_channels=len(bridge['channelMapping']),
    analyzed_clock=bridge['clockInfo'], matching_sfc_output_samples=samples,
    boundary='live TracerV token engine beside bound Print; control and stream storage remain explicit'), indent=2) + '\n')
print(f'PASS {samples} SFC handshake/payload samples; twelve live channels and Print clocks/hardware preserved')
