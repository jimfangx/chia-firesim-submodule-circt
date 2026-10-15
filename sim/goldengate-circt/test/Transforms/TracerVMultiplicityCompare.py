#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the quad-Rocket token boundary with archived SFC bridge hardware.

Run in the sourced FireSim environment. This deliberately stops before trigger
MMIO and stream storage: four token engines do not establish quad-core support.
"""
import itertools
import copy
import json
import random
import re
import subprocess
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module, terms, without_targets
from PrintControlMasterCompare import ports
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintTracerVTriggerCompare import RTL, parsed_expression


class CombinationalRTL(RTL):
    def __init__(self, header, body):
        self.widths = {name: geometry[1] for name, geometry in ports(header).items()}
        self.widths.update({name: int(hi) - int(lo) + 1 if hi else 1
                           for hi, lo, name in re.findall(
                               r'\b(?:wire|reg)\s*(?:\[(\d+):(\d+)\])?\s+(\w+)', body)})
        self.nets = {name: parsed_expression(expr) for name, expr in equations(body).items()}

    def output(self, name):
        return self.eval(('leaf', name))


def compare_identity(ir, annos, prior, reference_annos, reference):
    bridges = lambda rows: [a for a in rows if a.get('widgetClass', '').endswith('TracerVBridgeModule')]
    actual, old, expected = map(bridges, (annos, prior, reference_annos))
    assert len(actual) == len(old) == len(expected) == 4
    assert len({a['target'] for a in actual}) == 4
    assert len(annos) == len(prior)
    channels = {a['globalName']: a for a in annos if 'globalName' in a}
    original = {a['globalName']: a for a in prior if 'globalName' in a}
    clocks = lambda rows: [a for a in rows if a['class'] == 'midas.passes.ChannelClockInfoAnnotation']
    assert clocks(annos) == clocks(prior)
    trace_channels = set().union(*(set(a['channelMapping'].values()) for a in actual))
    for a, p in zip(annos, prior):
        normalized = dict(a)
        if a.get('globalName') in trace_channels:
            for end in ('sources', 'sinks'):
                if end not in p: normalized.pop(end)
        assert without_targets(normalized) == without_targets(p)
    wrapper = native_module(ir, 'GGTracerVTokenWrapper')
    constants = re.findall(r'(%\w+) = firrtl.constant 1 : !firrtl.uint<1>', wrapper)
    assert len(constants) == 8
    wrapper = wrapper.replace('firrtl.module @GGTracerVTokenWrapper(',
        'firrtl.module @GGTracerVTokenWrapper(' + ''.join(f'in {v}: !firrtl.uint<1>, ' for v in constants))
    nets = wiring(wrapper, 'GGTracerVTokenWrapper', aggregate=True)
    top = module(reference, 'FPGATop')[1]
    oracle_nets = equations(top)
    for n, (a, p, e) in enumerate(zip(actual, old, expected)):
        for key in ('widgetConstructorKey', 'channelMapping'):
            assert a[key] == p[key] == e[key]
        suffix = f'_{n}' if n else ''
        assert a['target'] == f'~GGTracerVTokenWrapper|GGTracerVTokenEngine{suffix}>hPort'
        inst = f'TracerVBridgeModule_{n}'
        oracle = instance(top, 'TracerVBridgeModule', inst)
        assert nets[inst + '.clock'] == 'top.hostClock'
        assert nets[inst + '.reset'] == 'top.hostReset'
        assert nets['top.tracerv_control' + suffix] == inst + '.control'
        assert nets['top.tracerv_stream' + suffix] == inst + '.stream'
        tokens = {}
        for local, global_name in a['channelMapping'].items():
            outgoing = local.startswith('trigger')
            host, target = ('sources', 'sinks') if outgoing else ('sinks', 'sources')
            c = channels[global_name]
            assert c['channelInfo'] == {'class': 'midas.passes.fame.PipeChannel', 'latency': 1}
            assert c[host] == [a['target'] + '.hBits.' + local]
            assert len(c[target]) == 1 and '|GGPeekPokeBridgeControlWrapper>' in c[target][0]
            assert clocks(annos)[0]['infoMap'][global_name] == a['clockInfo']
            tokens[local] = 'sim.' + original[global_name][target][0].split('>')[1].removesuffix('.bits')
        for local, token in tokens.items():
            outgoing = local.startswith('trigger')
            group = [k for k in tokens if k.startswith('trigger') == outgoing]
            expected_terms = {inst + '.hPort.' + ('fromHost.hValid' if outgoing else 'toHost.hReady')}
            expected_terms |= {tokens[k] + ('.ready' if outgoing else '.valid') for k in group if k != local}
            assert set(nets[token + ('.valid' if outgoing else '.ready')]) == expected_terms
            dest, src = ((token + '.bits', inst + '.hPort.hBits.' + local) if outgoing else
                         (inst + '.hPort.hBits.' + local, token + '.bits'))
            assert nets[dest] == src
        # SFC optimizes unused payloads away, but retains all handshake terms.
        for outgoing in (False, True):
            field = 'fromHost_hReady' if outgoing else 'toHost_hValid'
            names = {f'sim_channelPorts_FireSim_{g}_' + ('sink_ready' if outgoing else 'source_valid'): local
                     for local, g in a['channelMapping'].items() if local.startswith('trigger') == outgoing}
            assert terms(oracle_nets[oracle['hPort_' + field]], oracle_nets, names) == set(names.values())
            native_dest = inst + '.hPort.' + ('fromHost.hReady' if outgoing else 'toHost.hValid')
            expected_terms = {'top.' + constants[2 * n + int(outgoing)][1:]}
            expected_terms |= {tokens[k] + ('.ready' if outgoing else '.valid') for k in names.values()}
            assert set(nets[native_dest]) == expected_terms
        for local in ('tiletrace_reset', 'tiletrace_trace_retiredinsns_0_valid',
                      'tiletrace_trace_retiredinsns_0_iaddr', 'tiletrace_trace_retiredinsns_0_insn'):
            assert oracle_nets[oracle['hPort_hBits_' + local]] == f'sim_channelPorts_FireSim_{a["channelMapping"][local]}_source_bits'
    return actual


def main():
    candidate, golden, firtool, evidence = map(Path, sys.argv[1:])
    evidence.mkdir(parents=True, exist_ok=False)
    ir = (candidate / 'post-fame-tracerv-token.mlir').read_text()
    annos = json.loads((candidate / 'post-fame-tracerv-token-all.json').read_text())
    prior = json.loads((candidate / 'post-fame-peek-poke-control-all.json').read_text())
    golden_annos = golden / 'post-bridge-extraction.json'
    golden_rtl = golden / 'FireSim-generated.sv'
    reference = clean(golden_rtl.read_text())
    reference_annos = json.loads(golden_annos.read_text())
    actual = compare_identity(ir, annos, prior, reference_annos, reference)
    golden_clocks = golden / 'post-find-default-clocks.json'
    clock_bridges = [a for a in json.loads(golden_clocks.read_text())
                     if a.get('widgetClass', '').endswith('TracerVBridgeModule')]
    assert len(clock_bridges) == 4
    for a, e in zip(actual, clock_bridges):
        assert a['channelMapping'] == e['channelMapping']
        assert a['clockInfo'] == e['clockInfo']
    firrtl = (golden / 'post-sim-mapping.fir').read_text()
    assert re.findall(r'^    inst (TracerVBridgeModule_\d+) of TracerVBridgeModule\b', firrtl, re.M) == [
        f'TracerVBridgeModule_{n}' for n in range(4)]
    rejected = 0
    for mode in range(3):
        bad = copy.deepcopy(annos)
        bridges = [a for a in bad if a.get('widgetClass', '').endswith('TracerVBridgeModule')]
        if mode == 0:
            bridges[3]['target'] = bridges[0]['target']
        elif mode == 1:
            bridges[3]['channelMapping'] = bridges[0]['channelMapping']
        else:
            name = bridges[3]['channelMapping']['triggerCredit']
            next(a for a in bad if a.get('globalName') == name)['sources'] = [bridges[0]['target'] + '.hBits.triggerCredit']
        try:
            compare_identity(ir, bad, prior, reference_annos, reference)
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f'corrupted bridge identity/channel mapping accepted: {mode}')
    expected = CombinationalRTL(*module(reference, 'TracerVBridgeModule'))
    boolean_inputs = ['counter', 'triggerReg', 'hPort_hBits_tiletrace_reset',
                      'hPort_hBits_tiletrace_trace_retiredinsns_0_valid', 'hPort_toHost_hValid',
                      'hPort_fromHost_hReady', 'stream_ready', 'control_initDone',
                      'control_traceEnable', 'control_trigger']
    rename = dict(stream_ready='streamToHostCPU_ready', control_initDone='initDone',
                  control_traceEnable='traceEnable', control_trigger='trigger')
    outputs = {name: name for name in ('hPort_toHost_hReady', 'hPort_fromHost_hValid',
                                      'hPort_hBits_triggerCredit', 'hPort_hBits_triggerDebit')}
    outputs.update(stream_valid='streamToHostCPU_valid', stream_bits='streamToHostCPU_bits')
    rng = random.Random(237); samples = 0
    for n in range(4):
        name = 'GGTracerVTokenEngine' + (f'_{n}' if n else '')
        source = evidence / (name + '.mlir'); rtl = evidence / (name + '.sv')
        source.write_text('module { firrtl.circuit "' + name + '" {\n' + native_module(ir, name) + '\n} }\n')
        subprocess.run([str(firtool), str(source), '--format=mlir', '--disable-all-randomization',
                        '--strip-debug-info', '-o', str(rtl)], check=True)
        header, body = module(clean(rtl.read_text()), name)
        observed = CombinationalRTL(header, body)
        for pin, geometry in ports(module(reference, 'TracerVBridgeModule')[0]).items():
            if pin.startswith('hPort_'): assert ports(header)[pin] == geometry
        for bits in itertools.product((0, 1), repeat=len(boolean_inputs)):
            inputs = dict(zip(boolean_inputs, bits))
            inputs.update(hPort_hBits_tiletrace_trace_retiredinsns_0_iaddr=rng.getrandbits(40),
                          trace_cycle_counter=rng.getrandbits(64))
            observed.inputs = inputs; expected.inputs = {rename.get(k, k): v for k, v in inputs.items()}
            for pin, oracle_pin in outputs.items():
                assert observed.output(pin) == expected.output(oracle_pin), (n, pin, bits)
                samples += 1
    report = dict(golden_annotations=str(golden_annos), golden_rtl=str(golden_rtl),
                  golden_clocks=str(golden_clocks), matching_sfc_bridge_clocks=4,
                  golden_firrtl=str(golden / 'post-sim-mapping.fir'),
                  bridges=4, distinct_engine_targets=[a['target'] for a in actual], live_channels=48,
                  unchanged_annotation_records=len(annos), matching_sfc_output_samples=samples,
                  rejected_identity_mapping_endpoint_mutations=rejected,
                  boundary='four independent token engines; trigger banks, MMIO and stream storage remain unported')
    (evidence / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS four bridge identities, 48 channel handshakes and {samples} SFC RTL output comparisons')


if __name__ == '__main__':
    main()
