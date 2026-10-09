#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded native Rocket responses with immutable SFC NastiRouter.

Read actual FIRRTL SSA wiring at the native response composition boundary.
SFC has eleven single-beat normal banks and no Print host. Compare the seven
shared banks and the error source; check Print's unspecialized final-beat/ID
wiring against the same NastiRouter contract in both constructor orders.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, terms


def bindings(text):
    rows = re.findall(r'goldengate.controlReadBindings = \[(.*?)\]', text)
    assert rows and all(row == rows[0] for row in rows), 'propagated read catalogs differ'
    return [(name, port, int(slave)) for name, port, slave in re.findall(
        r'\{name = "([^"]+)", port = "([^"]+)", slave = (\d+) : i32\}', rows[0])]


def wiring(text, name, aggregate=False):
    body = re.search(r'^    firrtl.module @' + name + r'\(.*?^    }', text, re.M | re.S)
    assert body, f'missing native {name}'
    lines = body[0].splitlines()
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', lines[0])}
    nets = {}
    for line in lines[1:]:
        if ' = firrtl.instance ' in line:
            left, right = line.split(' = firrtl.instance ', 1)
            inst = right.split()[0]
            outputs = re.findall(r'%\w+', left)
            ports = re.findall(r'\b(?:in|out) (\w+):', right)
            assert len(outputs) == len(ports)
            values.update(zip(outputs, (inst + '.' + p for p in ports)))
        if match := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            dest, src, field = match.groups()
            assert isinstance(values[src], str)
            values[dest] = values[src] + '.' + field
        if match := re.search(r'(%\w+) = firrtl.and (%\w+), (%\w+)', line):
            dest, lhs, rhs = match.groups()
            values[dest] = frozenset(leaf(values[lhs]) | leaf(values[rhs]))
        connection = r'firrtl.(?:strictconnect|connect)' if aggregate else r'firrtl.strictconnect'
        if match := re.search(connection + r' (%\w+), (%\w+)', line):
            dest, src = match.groups()
            assert values[dest] not in nets, f'duplicate {name} driver'
            nets[values[dest]] = values[src]
    return nets


def leaf(value):
    return {value} if isinstance(value, str) else set(value)


def main():
    evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    router = equations(module(reference, 'NastiRouter')[1])
    baseline = bindings((evidence / 'candidate/post-fame-control-read-dispatch.mlir').read_text())
    indices = {name: slave for name, port, slave in baseline}
    assert len(indices) == 7
    reports = []
    for suffix in ('rocket-responses', 'rocket-responses-reverse'):
        path = evidence / f'binding.mlir.{suffix}.mlir'
        text = path.read_text()
        catalog = bindings(text)
        assert len(catalog) == 9
        rt = wiring(text, 'GGControlReadTrackerWrapper')
        ra = wiring(text, 'GGControlReadArbiterWrapper')
        wa = wiring(text, 'GGControlWriteArbiterWrapper')
        wt = wiring(text, 'GGControlWriteTrackerWrapper')
        sources = []
        for name, port, slave in catalog + [('error', 'ctrl_error', 13)]:
            error = name == 'error'
            ref_index = 11 if error else indices.get(name)
            for channel, arbiter, nets in (('r', 'readArbiter', ra), ('b', 'writeArbiter', wa)):
                ap = f'{arbiter}.in_{slave}_'
                bp = f'sim.ctrl_error_{channel}_' if error else f'sim.{port}.{channel}.'
                bits = bp + ('bits_' if error else 'bits.')
                assert nets[bp + 'ready'] == ap + 'ready' and nets[ap + 'valid'] == bp + 'valid'
                for field in ('resp', 'id', 'user') + (('data', 'last') if channel == 'r' else ()):
                    assert nets[ap + 'bits_' + field] == bits + field
                if channel == 'r':
                    actual = leaf(rt[f'controlReadTracker.deq_{slave}_valid'])
                    ready = 'top.ctrl_error_r_ready' if error else f'top.{port}.r.ready'
                    names = {ready: 'ready', bp + 'valid': 'valid', bits + 'last': 'last'}
                    assert rt[f'controlReadTracker.deq_{slave}_tag'] == bits + 'id'
                else:
                    actual = leaf(wa[f'top.ctrl_write_tracker_deq_{slave}_valid'])
                    names = {ap + 'ready': 'ready', ap + 'valid': 'valid'}
                    assert wa[f'top.ctrl_write_tracker_deq_{slave}_tag'] == ap + 'bits_id'
                    for field in ('valid', 'tag'):
                        assert wt[f'controlWriteTracker.deq_{slave}_{field}'] == f'sim.ctrl_write_tracker_deq_{slave}_{field}'
                assert actual == set(names)
                normalized = {names[term] for term in actual}
                if ref_index is not None:
                    rp = f'err_slave_io_{channel}_' if error else f'io_slave_{ref_index}_{channel}_'
                    reference_names = {rp + 'ready': 'ready', rp + 'valid': 'valid'}
                    if channel == 'r':
                        reference_names[rp + 'bits_last'] = 'last'
                    queue = 'ar' if channel == 'r' else 'aw'
                    expected = terms(router[f'{queue}_queue_io_deq_{ref_index}_valid'], router, reference_names)
                    specialized = normalized - {'last'} if channel == 'r' and not error else normalized
                    assert specialized == expected
                    assert router[f'{queue}_queue_io_deq_{ref_index}_tag'].strip() == rp + 'bits_id'
                    prefix = f'{channel}_arb_io_in_{ref_index}_'
                    assert router[prefix + 'valid'].strip() == rp + 'valid'
                    assert router[prefix + 'bits_id'].strip() == rp + 'bits_id'
                    assert router[rp + 'ready'].strip() == prefix + 'ready'
                    if channel == 'r' and not error:
                        assert router[prefix + 'bits_data'].strip() == rp + 'bits_data'
            sources.append({'name': name, 'native_slave': slave, 'SFC_slave': ref_index})
        assert text.count('goldengate.trackerSlots = 64 : i32') == 2
        assert text.count('goldengate.trackerTagWidth = 12 : i32') == 2
        assert text.count('goldengate.trackerDequeuePorts = 14 : i32') == 2
        assert text.count('goldengate.trackerRouteWidth = 4 : i32') == 2
        reports.append(sources)
    assert reports[0] == reports[1]
    assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
    report = {'golden': str(golden), 'native_sources': reports[0], 'constructor_orders': 2,
              'shared_SFC_response_banks': 7, 'error_SFC_slave': 11, 'native_error_slave': 13,
              'native_retirement_ports': 14, 'tracker_slots': 64, 'tag_width': 12,
              'SFC_normal_R_last_specialized_to_one': True, 'reference_has_print_host': False,
              'boundary': 'nine connected Rocket/Print banks plus error; four later response banks exposed'}
    (evidence / 'rocket-responses-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS seven SFC B/R bank identities, IDs/data and retirement predicates plus error; two native Print banks in both constructor orders')


if __name__ == '__main__':
    main()
