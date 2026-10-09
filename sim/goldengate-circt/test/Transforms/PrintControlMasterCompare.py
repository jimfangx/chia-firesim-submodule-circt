#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print control master with the immutable SFC U250 interface.

The golden platform has printf disabled. Compare the complete shared control
ABI and the candidate's actual master-to-router nets, not Print driver parity.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module, without_targets


def ports(header):
    result = {}
    direction = width = None
    for item in header.split(','):
        item = item.strip()
        declaration = re.fullmatch(r'(input|output|inout)\s+(?:wire\s+)?(?:\[(\d+):0\]\s*)?(\w+)', item)
        if declaration:
            direction, msb, name = declaration.groups()
            width = int(msb) + 1 if msb else 1
        else:
            assert direction and re.fullmatch(r'\w+', item), f'unrecognized port {item}'
            name = item
        assert name not in result
        result[name] = [direction, width]
    return result


def main():
    evidence, golden = map(Path, sys.argv[1:])
    candidate = evidence / 'candidate'
    native = clean((candidate / 'post-print-control-master.sv').read_text())
    before = clean((candidate / 'post-print-control-responses.sv').read_text())
    reference = clean(golden.read_text())
    header, body = module(native, 'GGControlMasterWrapper')
    native_ports = ports(header)
    ref_ports = ports(module(reference, 'FPGATop')[0])
    native_ctrl = {k: v for k, v in native_ports.items() if re.match(r'ctrl_(aw|w|b|ar|r)_', k)}
    ref_ctrl = {k: v for k, v in ref_ports.items() if re.match(r'ctrl_(aw|w|b|ar|r)_', k)}
    assert len(ref_ctrl) == 45 and native_ctrl == ref_ctrl, 'SFC U250 control ABI differs'
    inner = instance(body, 'GGControlWriteTrackerWrapper', 'sim')
    mapped = {}
    for pin in native_ctrl:
        _, channel, leaf = pin.split('_', 2)
        if channel == 'ar':
            boundary = 'ctrl_read_dispatch_master_ar_' + leaf
        elif channel in ('b', 'r'):
            boundary = f'ctrl_{"read" if channel == "r" else "write"}_arb_out_{leaf}'
        elif leaf in ('ready', 'valid'):
            boundary = f'ctrl_write_route_{channel}_{leaf}'
        elif channel == 'aw' and leaf == 'bits_addr':
            boundary = 'ctrl_decode_aw_addr'
        elif channel == 'w' and leaf == 'bits_last':
            boundary = 'ctrl_write_route_w_last'
        else:
            boundary = f'ctrl_write_dispatch_master_{channel}_{leaf}'
        assert inner[boundary] == pin, f'{pin} disconnected from {boundary}'
        mapped[pin] = boundary
    old_header, old_body = module(before, 'GGControlWriteTrackerWrapper')
    old_ports = ports(old_header)
    copied = {k: v for k, v in old_ports.items() if k not in mapped.values()}
    assert {k: v for k, v in native_ports.items() if k not in native_ctrl} == copied, 'copied boundary changed'
    old_eq, master_eq = equations(old_body), equations(body)
    inner_eq = equations(module(native, 'GGControlWriteTrackerWrapper')[1])
    folded = {}
    for pin in copied:
        if inner[pin] == pin:
            continue
        # CIRCT propagates constant instance outputs to the parent and connects
        # the now-unused output to a sink wire. Require the same literal in
        # both emissions and the inner module, never ignore a missing net.
        value = old_eq.get(pin, '')
        assert copied[pin][0] == 'output' and re.fullmatch(r"\d+'[hbd][0-9a-fA-F]+", value), f'copied boundary {pin} disconnected'
        assert inner[pin] == 'sim_unused_' + pin and master_eq.get(pin) == value and inner_eq.get(pin) == value, f'copied constant {pin} changed'
        folded[pin] = value
    prior = json.loads((candidate / 'post-print-control-responses-all.json').read_text())
    after = json.loads((candidate / 'post-print-control-master-all.json').read_text())
    assert without_targets(prior) == without_targets(after), 'master binding changed annotation payload/order'
    old_name, new_name = 'GGControlWriteTrackerWrapper', 'GGControlMasterWrapper'
    # Annotation references name aggregate FIRRTL ports, before RTL flattening.
    master_mlir = (candidate / 'post-print-control-master.mlir').read_text()
    ir_header = re.search(r'firrtl.module @GGControlMasterWrapper\((.*?)\)', master_mlir, re.S)
    assert ir_header, 'missing master FIRRTL port declarations'
    copied_roots = set(re.findall(r'\b(?:in|out|inout) %([\w]+):', ir_header[1])) - {'ctrl'}

    def retarget(value):
        if isinstance(value, str):
            prefix = '~' + old_name
            if value == prefix:
                return '~' + new_name
            if value.startswith(prefix + '|'):
                suffix = value[len(prefix):]
                module_prefix = '|' + old_name + '>'
                if suffix.startswith(module_prefix):
                    root = re.split(r'[.\[]', suffix[len(module_prefix):], 1)[0]
                    if root in copied_roots:
                        suffix = '|' + new_name + '>' + suffix[len(module_prefix):]
                return '~' + new_name + suffix
            return value
        if isinstance(value, list):
            return [retarget(x) for x in value]
        if isinstance(value, dict):
            return {k: retarget(x) for k, x in value.items()}
        return value

    assert retarget(prior) == after, 'master target transfer differs from copied/internalized identities'
    report = {'golden': str(golden), 'candidate': str(candidate / 'post-print-control-master.sv'),
              'control_ports': native_ctrl, 'master_connections': mapped,
              'copied_ports': len(copied), 'folded_constant_outputs': folded,
              'annotation_targets_preserved': True,
              'reference_has_print_host': False}
    (evidence / 'control-master-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS all 45 SFC U250 control widths/directions and native master connections; copied ports and annotation targets preserved')


if __name__ == '__main__':
    main()
