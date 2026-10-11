#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket address translation against immutable SFC.

Check actual FIRRTL SSA, AXI widths/directions, address arithmetic and all
four assertion predicates. SFC disables Print and optimizes away B.ready;
the candidate preserves that ready path and the expanded Print allocation.
"""
import argparse
import itertools
import json
import re
from pathlib import Path

from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDControlCompare import transfer
from PrintRocketFASEDHostMemoryCompare import CHANNELS, memory_type
from PrintRocketFASEDIssueCompare import evaluate, expressions
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

OLD = 'GGFASEDHostMemoryWrapper'
NEW = 'GGFASEDAddressTranslationWrapper'
HELPER = 'GGFASEDAddressTranslation'
BASE, BOUND, SHIFT = 0x80000000, 0x47fffffff, 0x380000000
MASK = (1 << 34) - 1


def routes():
    result, fields = {}, {}
    for ch, bit_fields in CHANNELS:
        request = ch in ('aw', 'w', 'ar')
        for leaf, width in [('ready', 1), ('valid', 1)] + [('bits.' + n, w) for n, w in bit_fields]:
            outward = request != (leaf == 'ready')
            dest, src = ('out.', 'in.') if outward else ('in.', 'out.')
            key = ch + '.' + leaf
            result[dest + key] = ('leaf', src + key)
            fields[key] = (outward, width)
    for ch in ('aw', 'ar'):
        result['out.' + ch + '.bits.addr'] = (
            'bits', ('add', ('leaf', 'in.' + ch + '.bits.addr'), ('const', SHIFT)), 33, 0)
    return result, fields


def sfc_contract(path):
    header, body = module(clean(path.read_text()), 'AXI4AddressTranslation')
    eq = {key: ' '.join(value.split()) for key, value in equations(body).items()}
    _, fields = routes()
    expected_ports = {'clock': ['input', 1], 'reset': ['input', 1]}
    connections = {}
    for key, (outward, width) in fields.items():
        if key == 'b.ready':
            continue  # Both sides are removed by SFC dead-code elimination.
        leaf = key.replace('.', '_')
        for side in ('in', 'out'):
            output = outward == (side == 'out')
            port_width = 34 if side == 'out' and key in ('aw.bits.addr', 'ar.bits.addr') else width
            expected_ports['auto_' + side + '_' + leaf] = ['output' if output else 'input', port_width]
        dest, src = ('out', 'in') if outward else ('in', 'out')
        name = 'auto_' + dest + '_' + leaf
        source = 'auto_' + src + '_' + leaf
        if key in ('aw.bits.addr', 'ar.bits.addr'):
            ch = key.split('.')[0]
            temp = '_nodeOut_' + ch + '_bits_addr_T_1'
            assert eq[temp] == source + " + 35'h380000000"
            source = temp + '[33:0]'
        assert eq[name] == source, 'SFC channel equation differs: ' + name
        connections[name] = source
    assert rtl_ports(header) == expected_ports and len(expected_ports) == 74
    assert len(connections) == 36
    predicates = {
        '_T_2': "~auto_in_aw_valid | auto_in_aw_bits_addr <= 35'h47fffffff",
        '_T_8': "~auto_in_ar_valid | auto_in_ar_bits_addr <= 35'h47fffffff",
        '_T_14': "_T | auto_in_aw_bits_addr >= 35'h80000000",
        '_T_20': "_T_6 | auto_in_ar_bits_addr >= 35'h80000000",
        '_T': '~auto_in_aw_valid', '_T_6': '~auto_in_ar_valid', '_T_4': '~reset',
    }
    assert all(eq[name] == value for name, value in predicates.items())
    checks = re.findall(r'if \((~reset|_T_4)\) begin\s+assert\((\w+)\);', body)
    assert checks == [('~reset', '_T_2'), ('_T_4', '_T_8'), ('_T_4', '_T_14'), ('_T_4', '_T_20')]
    assert len(re.findall(r'always @\(posedge clock\)', body)) == 2
    for ch in ('AW', 'AR'):
        for ending in (' exceeds region bound.', ' is less than region base.'):
            assert 'Assertion failed: ' + ch + ' request address in memory region MainMemory_0' + ending in body
    return expected_ports, connections, predicates


def helper_semantics(text):
    body = native_module(text, HELPER)
    assert native_ports(body) == [('in', 'clock', '!firrtl.clock'), ('in', 'reset', '!firrtl.uint<1>'),
                                  ('in', 'in', memory_type()),
                                  ('out', 'out', memory_type().replace('addr: uint<35>', 'addr: uint<34>'))]
    region = ('goldengate.memoryRegion = {hostBase = 0 : i64, name = "MainMemory_0", '
              'offset = -2147483648 : i64, virtualBase = 2147483648 : i64, virtualBound = 19327352831 : i64}')
    assert region in body.splitlines()[0]
    nets, checks = expressions(text, HELPER)
    assert nets == routes()[0] and len(nets) == 37
    assert len(checks) == 4
    expected = []
    for op, limit, ending in [('leq', BOUND, ' exceeds region bound.'),
                              ('geq', BASE, ' is less than region base.')]:
        for ch in ('aw', 'ar'):
            predicate = ('or', ('not', ('leaf', 'in.' + ch + '.valid')),
                         (op, ('leaf', 'in.' + ch + '.bits.addr'), ('const', limit)))
            expected.append((predicate, ('not', ('leaf', 'reset')),
                             ch.upper() + ' request address in memory region MainMemory_0' + ending))
    assert checks == expected
    assert re.findall(r'firrtl.assert (%\w+)', body) == ['%clock'] * 4
    assert body.count('eventControl = 0 : i32, isConcurrent = false') == 4
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', body), 'translation introduced state'
    cases = 0
    for aw, ar, valid, reset, ready in itertools.product(
            (0, BASE - 1, BASE, 0xffffffff, 0x400000000, BOUND, BOUND + 1, (1 << 35) - 1),
            (BASE - 1, BASE, BOUND, BOUND + 1), range(4), range(2), range(4)):
        inputs = {'reset': reset, 'in.aw.bits.addr': aw, 'in.ar.bits.addr': ar,
                  'in.aw.valid': valid & 1, 'in.ar.valid': bool(valid & 2),
                  'out.aw.ready': ready & 1, 'out.ar.ready': bool(ready & 2)}
        for ch, addr in [('aw', aw), ('ar', ar)]:
            assert evaluate(nets['out.' + ch + '.bits.addr'], inputs, nets) == (addr - BASE) & MASK
        for index, (predicate, enabled, _) in enumerate(checks):
            addr, active = (aw, valid & 1) if index % 2 == 0 else (ar, valid & 2)
            failed = not reset and active and (addr > BOUND if index < 2 else addr < BASE)
            assert bool(evaluate(enabled, inputs, nets) and not evaluate(predicate, inputs, nets)) == bool(failed)
        cases += 1
    assert cases == 1024
    return cases


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, text, re.M | re.S)) for text in (before, after))
    assert len(prior) == 261 and set(result) - set(prior) == {HELPER, NEW}
    assert all(result.get(name) == body for name, body in prior.items()), 'existing module body changed'
    old, new = native_module(before, OLD), native_module(after, NEW)
    copied = [(d, n, t) for d, n, t in native_ports(old) if n != 'fased_host_mem']
    assert len(copied) == 109
    out_type = memory_type().replace('addr: uint<35>', 'addr: uint<34>')
    assert native_ports(new) == copied + [('out', 'fased_host_mem', out_type)]
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'translation.clock': 'top.hostClock', 'translation.reset': 'top.hostReset',
                 'translation.in': 'sim.fased_host_mem', 'top.fased_host_mem': 'translation.out'})
    assert wiring(new) == nets and len(nets) == 113
    assert len(re.findall(r' = firrtl.instance sim @' + OLD + r'\b', new)) == 1
    assert len(re.findall(r' = firrtl.instance translation @' + HELPER + r'\b', new)) == 1
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', new)
    vectors = helper_semantics(after)
    assert native_module(after, HELPER) == native_module(baseline, HELPER), 'shared helper semantics changed'
    print_modules = [name for name in prior if name.startswith('GGPrintBridge')]
    assert len(print_modules) == 21
    regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', after)[1]
    print_regions = re.findall(r'name = "(PrintBridgeModule_\d+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64', regions)
    assert print_regions == [('PrintBridgeModule_0', '32', '8', '544'), ('PrintBridgeModule_1', '32', '9', '576')]
    header = before.splitlines()[1]
    probes = re.findall(r'\{class = "test.TranslationBoundary", target = "([^"]+)"}', after.splitlines()[1])
    assert probes and 'class = "test.TranslationBoundary"' not in header
    original_probes = ['~' + OLD + '|' + OLD + '>' + ref for ref in (
        'ctrl.aw.bits.addr', 'hostReset', 'fased_host_mem.aw.bits.addr',
        'fased_host_mem.ar.bits.addr', 'fased_host_mem.r.bits.data', 'fased_host_mem')]
    original_probes.append('~' + OLD)
    assert len(probes) == 7, 'missing boundary identity probes'
    end = header.rfind(']} {')
    assert end >= 0
    header = header[:end] + ', ' + ', '.join('{class = "test.TranslationBoundary", target = "' + target + '"}' for target in original_probes) + header[end:]
    header = transfer(header, OLD, NEW, [(d, n) for d, n, _ in copied])
    assert after.splitlines()[1] == header, 'annotation archive transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(prior),
                copied_ports=len(copied), wrapper_connections=len(nets), helper_passthrough_connections=35,
                translated_addresses=2, assertions=4, semantic_vectors=vectors,
                retained_Print_modules=len(print_modules), retained_Print_regions=print_regions,
                annotation_archive_transfer=True, annotation_identity_probes=len(probes),
                shared_helper_match=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, action='append', required=True)
    parser.add_argument('--after', type=Path, action='append', required=True)
    parser.add_argument('--sfc', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--report-json', type=Path, required=True)
    args = parser.parse_args()
    assert len(args.before) == len(args.after)
    ports, connections, predicates = sfc_contract(args.sfc.resolve())
    baseline = args.baseline.read_text()
    reports = [compare(before.resolve(), after.resolve(), baseline) for before, after in zip(args.before, args.after)]
    report = dict(golden=str(args.sfc.resolve()), SFC_module='AXI4AddressTranslation',
                  SFC_ports=ports, SFC_channel_connections=connections, SFC_assertion_predicates=predicates,
                  optimized_SFC_AXI_leaves=['auto_in_b_ready', 'auto_out_b_ready'],
                  virtual_base=hex(BASE), virtual_bound=hex(BOUND), output_address_bits=34,
                  offset=-BASE, orders=reports,
                  limitation='SFC disables Print. Comparison covers local address translation; Print-enabled runtime parity and later deinterleaving remain pending.')
    args.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS 72 surviving SFC AXI widths/directions, 34 passthrough routes, two address translations and four assertions; '
          '1024 semantic vectors per order, 113 wrapper connections, 261 prior modules and annotation archive preserved')


if __name__ == '__main__':
    main()
