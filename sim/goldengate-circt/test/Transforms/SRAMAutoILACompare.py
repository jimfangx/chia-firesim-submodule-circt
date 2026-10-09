#!/usr/bin/env python3
# See LICENSE for license details.
"""Trace emitted ILA wires through normalized host hierarchy to target ports.

The independent Scala oracle checks source identities/order/widths and IP
settings. This checks their actual RTL bindings, including the host clock.
"""
import json
import re
import sys
from pathlib import Path


def compare(directory):
    directory = Path(directory)
    text = (directory / "FireSim-generated.sv").read_text()
    text += (directory / "FireSim-generated.ila_wrapper_inst.v").read_text()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    modules = {}
    for name, header, body in re.findall(r"\bmodule\s+(\w+)\s*\((.*?)\);(.*?)\bendmodule", text, re.S):
        ports = {}
        direction, width = None, 1
        for part in header.split(","):
            match = re.fullmatch(r"\s*(?:(input|output|inout)\s+(?:\[\s*(\d+)\s*:\s*0\s*\]\s*)?)?(\w+)\s*", part)
            assert match, f"unsupported module port in {name}: {part}"
            if match[1]:
                direction, width = match[1], int(match[2]) + 1 if match[2] else 1
            assert direction
            ports[match[3]] = (direction, width)
        aliases = dict(re.findall(r"\bassign\s+(\w+)\s*=\s*([^;]+);", body))
        aliases.update(re.findall(r"\bwire\s+(?:\[[^]]+\]\s*)?(\w+)\s*=\s*([^;]+);", body))
        instances = {}
        for kind, instance, bindings in re.findall(r"(?m)^\s*(\w+)\s+(\w+)\s*\((.*?)\);", body, re.S):
            connections = dict(re.findall(r"\.(\w+)\s*\(([^()]*)\)", bindings))
            if connections:
                instances[instance] = (kind, {p: v.strip() for p, v in connections.items()})
        modules[name] = (ports, aliases, instances)

    def canonical(module, net):
        seen = set()
        aliases = modules[module][1]
        while re.fullmatch(r"\w+", net) and net in aliases:
            assert net not in seen, "cyclic route aliases"
            seen.add(net)
            net = aliases[net].strip()
        return re.sub(r"\s+", "", net)

    def trace(module, net, target_module, target_port, parents=(), active=()):
        net = canonical(module, net)
        state = (module, net, parents)
        assert state not in active, "cyclic ILA route"
        if module == target_module and net == canonical(module, target_port):
            return
        ports, _, instances = modules[module]
        if net in ports and ports[net][0] == "input" and parents:
            parent, instance = parents[-1]
            trace(parent, modules[parent][2][instance][1][net], target_module,
                  target_port, parents[:-1], active + (state,))
            return
        writers = []
        for instance, (kind, bindings) in instances.items():
            if kind not in modules:
                continue
            for port, value in bindings.items():
                if (port in modules[kind][0] and modules[kind][0][port][0] == "output"
                        and canonical(module, value) == net):
                    writers.append((instance, kind, port))
        assert len(writers) == 1, f"ambiguous/missing ILA writer in {module}: {net}"
        instance, kind, port = writers[0]
        trace(kind, port, target_module, target_port,
              parents + ((module, instance),), active + (state,))

    wrapper_kind, bindings = modules["F1Shim"][2]["ila_wrapper_inst"]
    assert wrapper_kind == "ila_wrapper"
    trace("F1Shim", bindings["clock"], "F1Shim", "clock")
    probes = json.loads((directory / "autoila-probes.json").read_text())
    wrapper_ports = list(modules[wrapper_kind][0].items())
    assert len(wrapper_ports) == len(probes) + 1 == 4
    for probe, (port, contract) in zip(probes, wrapper_ports[1:]):
        source = probe["target"].split(".")
        assert contract == ("input", probe["width"])
        assert port == probe["suggested_name"]
        trace("F1Shim", bindings[port], source[1], source[2])
    assert "`ifdef SYNTHESIS" in text, "ILA IP lost its metasimulation guard"
    print("PASS emitted SRAM AutoILA RTL: three ordered, typed Rocket routes and host clock survive hierarchy normalization")


if __name__ == "__main__":
    compare(sys.argv[1])
