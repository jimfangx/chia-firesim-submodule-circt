#!/usr/bin/env python3
# See LICENSE for license details.
"""Check the fully lowered NIC platform and published collateral against SFC.

These are integration/ABI checks, not equivalence of the NIC or SRAM models.
Manager-owned NIC runtime verification is still required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Transforms'))
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, module
from PrintRocketF1ShimCompare import routes
from NICDriverHeaderCompare import constructors, normalize
from NICMetasimInterfaceCompare import macros, config


def compare(candidate, golden):
    base = 'FireSim-generated'
    actual = clean((candidate / (base + '.sv')).read_text())
    expected = clean((golden / (base + '.sv')).read_text())
    interfaces = {}
    for name, count in [('F1Shim', 143), ('FPGATop', 121)]:
        reference = ports(module(expected, name)[0])
        observed = ports(module(actual, name)[0])
        assert len(reference) == count and observed == reference, name + ' ABI differs'
        interfaces[name] = len(reference)
    reference_routes, observed_routes = routes(expected), routes(actual)
    assert observed_routes == reference_routes, 'final shim routes differ'

    # Resolve every generated-clock destination through the emitted hierarchy;
    # comparing a renamed XDC alone could accept a constraint for missing RTL.
    implementation = (candidate / (base + '.implementation.xdc')).read_text()
    reference_xdc = (golden / (base + '.implementation.xdc')).read_text()
    assert implementation == reference_xdc.replace('target/FireSim_', 'target_FAMETop/FireSim')
    destination, = re.findall(r'firesim_top/top/([^\s]+?)/O\.\*', implementation)
    current = 'FPGATop'
    hierarchy = []
    for name in destination.split('/'):
        body = module(actual, current)[1]
        matches = re.findall(r'\b(\w+)\s+' + re.escape(name) + r'\s*\(', body)
        assert len(matches) == 1, 'missing or ambiguous clock instance: ' + name
        kind, = matches
        hierarchy.append(dict(parent=current, instance=name, module=kind))
        current = kind
    assert current == 'BUFGCE', 'generated clock is not a live Xilinx buffer'
    for suffix in ('.synthesis.xdc', '.defines.vh'):
        assert (candidate / (base + suffix)).read_bytes() == (golden / (base + suffix)).read_bytes(), suffix

    header = (candidate / (base + '.const.h')).read_text()
    reference_header = (golden / (base + '.const.h')).read_text()
    assert constructors(header) == constructors(reference_header)
    checks = lambda s: sorted(normalize(x) for x in re.findall(r'static_assert\(.*?;', s, re.S))
    assert checks(header) == checks(reference_header) and config(header) == config(reference_header)
    assert (candidate / 'driver-header' / (base + '.const.h')).read_text() == header
    constants = macros((golden / (base + '.const.vh')).read_text())
    assert constants.pop('MEM_HAS_CHANNEL-1') == '1'
    assert macros((candidate / (base + '.const.vh')).read_text()) == constants
    archive = json.loads((candidate / 'post-fame-xdc-all.json').read_text())
    published, = [a for a in archive if a.get('fileSuffix') == '.const.h']
    assert published['body'] == header
    return dict(interfaces=interfaces, shim_routes=len(reference_routes),
                live_generated_clock_path=hierarchy, matching_constructors=len(constructors(header)),
                fixed_layout_checks=len(checks(header)), metasim_constants=len(constants),
                complete_header_published=True, annotation_count=len(archive))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('candidate', 'golden', 'output'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    report = compare(args.candidate, args.golden)
    if args.mutations:
        # Change copies only. These controls catch a wrong public ABI, a stale
        # clock destination, and a dropped constructor in otherwise complete
        # compiler output. None touches the SFC oracle or production candidate.
        report['mutations_rejected'] = []
        for name, suffix, old, new in [
                ('platform-port', '.sv', 'io_master_w_bits_data', 'broken_io_master_w_bits_data'),
                ('clock-instance', '.sv', 'BUFGCE clockBridge_clocks_0_buffer',
                 'BUFGCE disconnected_clock_buffer'),
                ('driver-constructor', '.const.h', 'new simplenic_t(', 'new missing_nic_t(')]:
            with tempfile.TemporaryDirectory(prefix=name + '-', dir=args.output.parent) as temporary:
                directory = Path(temporary)
                filename = 'FireSim-generated' + suffix
                for path in args.candidate.iterdir():
                    if path.name != filename:
                        (directory / path.name).symlink_to(path.resolve())
                text = (args.candidate / filename).read_text()
                assert old in text, 'mutation no longer applies: ' + name
                (directory / filename).write_text(text.replace(old, new, 1))
                try:
                    compare(directory, args.golden)
                except AssertionError:
                    report['mutations_rejected'].append(name)
                else:
                    raise AssertionError('invalid integration accepted: ' + name)
    report.update(candidate=str(args.candidate.resolve()), golden=str(args.golden.resolve()),
                  golden_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in args.golden.glob('FireSim-generated.*')
                                 if p.suffix in ('.sv', '.h', '.vh', '.xdc')},
                  limitation='Platform integration and collateral only; NIC runtime and SRAM equivalence remain open.')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
