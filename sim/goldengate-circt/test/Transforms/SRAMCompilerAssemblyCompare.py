#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the selected Rocket adapter after complete FireSim assembly.

Reference: independent SFC FAME/RAM adapter lowered by pinned firtool.
Candidate: full native compiler RTL. This checks port contracts and every
adapter equation/binding, not textual equality of the target or RAM state.
"""
import re
import sys
from pathlib import Path


def adapter(path):
    text = Path(path).read_text()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    modules = re.findall(r"\bmodule\s+rf\s*\((.*?)\);(.*?)\bendmodule", text, re.S)
    assert len(modules) == 1, "expected one rf adapter definition"
    header, body = modules[0]
    assert not re.search(r"\b(reg|always|always_ff|mem)\b", body), "adapter retained state"
    ports = {}
    direction, width = None, 1
    for part in header.split(","):
        match = re.fullmatch(r"\s*(?:(input|output)\s+(?:\[\s*(\d+)\s*:\s*0\s*\]\s*)?)?(\w+)\s*", part)
        assert match, f"unsupported adapter port: {part}"
        if match[1]:
            direction, width = match[1], int(match[2]) + 1 if match[2] else 1
        assert direction and match[3] not in ports
        ports[match[3]] = (direction, width)
    aliases = dict(re.findall(r"\bwire\s+(\w+)\s*=\s*([^;]+);", body))

    def terms(expression, active=()):
        result = set()
        for leaf in expression.split("&"):
            leaf = leaf.strip()
            assert re.fullmatch(r"\w+|\d+'h[0-9a-fA-F]+", leaf), f"unsupported equation: {leaf}"
            assert leaf not in active, "cyclic adapter alias"
            result.update(terms(aliases[leaf], active + (leaf,)) if leaf in aliases else (leaf,))
        return tuple(sorted(result))

    instances = re.findall(r"\bRamModel\s+model\s*\((.*?)\);", body, re.S)
    assert len(instances) == 1, "missing or duplicated timing implementation"
    bindings = dict(re.findall(r"\.(\w+)\s*\(([^()]*)\)", instances[0]))
    # SFC leaves this unused output open; native assembly gives it a wire.
    unused = bindings.pop("channels_reset_ready").strip()
    if unused:
        assert len(re.findall(r"\b" + re.escape(unused) + r"\b", body)) == 2, "reset-ready output is observed"
    equations = {name: terms(value) for name, value in bindings.items()}
    equations.update({name: terms(value) for name, value in
        re.findall(r"\bassign\s+(\w+)\s*=\s*([^;]+);", body)})
    return ports, equations


reference, candidate = map(adapter, sys.argv[1:3])
assert reference[0] == candidate[0], "assembled SRAM adapter ABI differs from SFC"
assert reference[1] == candidate[1], "assembled SRAM adapter equations differ from SFC"
print(f"PASS assembled Rocket.rf: {len(reference[0])} port contracts and {len(reference[1])} adapter bindings/equations match independent SFC")
