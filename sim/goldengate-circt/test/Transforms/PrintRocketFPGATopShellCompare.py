#!/usr/bin/env python3
# See LICENSE for license details.
"""Check expanded Print shell identity/archive and complete immutable SFC ABI.

The replay test explicitly supplies missing FAME host identities. SFC disables
Print; matching its platform ABI does not establish enabled-Print runtime parity.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports as rtl_ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring

OLD, TOP = 'GGPrintCPUStreamPlatformWrapper', 'FPGATop'
PUBLIC = dict(hostClock='clock', hostReset='reset', ctrl='ctrl', mem_0='mem_0',
              cpu_managed_axi4='cpu_managed_axi4')
PROBES = ['hostClock', 'hostReset', 'ctrl.aw.bits.addr', 'mem_0.r.bits.data',
          'cpu_managed_axi4.r.bits.data', 'other', 'fased_pending_reads']


def modules(text):
    found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
    result = {m[1]: m[0] for m in found}
    assert len(result) == len(found)
    return result


def compare(before_path, after_path):
    before, after = before_path.read_text(), after_path.read_text()
    old, new = modules(before), modules(after)
    assert len(old) == 299 and len(new) == 300 and set(new) - set(old) == {TOP}
    for n, body in old.items():
        assert body == new[n], 'changed retained body ' + n
    prior = native_ports(old[OLD]); assert len(prior) == 98
    expected = [(d, PUBLIC[n], t) for n in PUBLIC for d, name, t in prior if name == n]
    assert len(expected) == 5 and native_ports(new[TOP]) == expected
    routes = {('sim.' + n if d == 'in' else 'top.' + PUBLIC[n]):
              ('top.' + PUBLIC[n] if d == 'in' else 'sim.' + n)
              for n in PUBLIC for d, name, _ in prior if name == n}
    assert wiring(new[TOP]) == routes and len(routes) == 5
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD)]
    assert len(re.findall(r'firrtl\.(?:instance|connect)\b', new[TOP])) == 6
    # Check the complete historical archive as well as recursive private/public
    # probes, independently of the compiler's retargeting implementation.
    line = after.splitlines()[1]
    added = ', {class = "midas.passes.fame.FAMEHostClock", target = "~FPGATop|FPGATop>clock"}'
    added += ', {class = "midas.passes.fame.FAMEHostReset", target = "~FPGATop|FPGATop>reset"}'
    targets = []
    for p in PROBES:
        root, _, suffix = p.partition('.')
        target = TOP + '>' + PUBLIC[root] + ('.' + suffix if suffix else '') if root in PUBLIC else OLD + '>' + p
        targets.append('"~FPGATop|' + target + '"')
    added += ', {class = "test.FPGATopTargets", nested = {targets = [' + ', '.join(targets) + ']}}'
    added += ', {class = "midas.passes.HostClockSource", target = "~FPGATop|FPGATop>clock"}'
    assert line.count(added) == 1, 'FAME identity/probe/source archive mismatch'
    line = line.replace(added, '')
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "FPGATop"')
    def transfer(match):
        value = match[1]
        if value == '~' + OLD: return '"~FPGATop"'
        if not value.startswith('~' + OLD + '|'): return match[0]
        value = '~FPGATop|' + value[len(OLD)+2:]
        for n, target in PUBLIC.items():
            value = re.sub(r'\|' + OLD + '>' + n + r'(?=[.\[]|$)', '|FPGATop>' + target, value)
        return '"' + value + '"'
    original = re.sub(r'"(~[^"\\]+)"', transfer, original)
    assert line == original, 'historical annotation archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=299,
                retained_Print_modules=21, retained_private_ports=93, shell_ports=5,
                identity_connections=5, unique_HostClockSource=True, complete_archive_transfer=True)


def stub(after, path):
    bodies = modules(after.read_text())
    child = bodies[OLD].split(' {', 1)[0].strip()
    child = child.replace('firrtl.module @', 'firrtl.extmodule private @').replace('%', '')
    child += ' attributes {defname = "' + OLD + '"}'
    path.write_text('module { firrtl.circuit "FPGATop" {\n' + child + '\n' + bodies[TOP] + '\n} }\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    p.add_argument('--stub', type=Path)
    p.add_argument('--rtl', type=Path)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y) for x, y in zip(a.before, a.after)])
    if a.stub: stub(a.after[0], a.stub)
    if a.rtl:
        golden = rtl_ports(module(clean(a.sfc.read_text()), 'FPGATop')[0])
        candidate = rtl_ports(module(clean(a.rtl.read_text()), 'FPGATop')[0])
        assert len(golden) == 121 and candidate == golden, 'complete platform RTL ABI differs'
        report['SFC_platform_ABI'] = dict(module='FPGATop', leaves=121, names_widths_directions_match=True)
    report['limitation'] = 'FAME host identities explicitly seeded by replay test; disabled-Print SFC platform ABI only. Production enabled-Print integration/runtime parity pending.'
    a.report_json.write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
