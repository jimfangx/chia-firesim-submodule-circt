#!/usr/bin/env python3
"""Audit inferred FAME channel routes through ground wrapper connectivity.

This checks actual instance/pin connections before comparing channel records.
It neither normalizes target spellings nor proves sequential equivalence. The
accepted stage has ground ports and direct wrapper connects. An endpoint with
unresolved logic, mixed types, multiple drivers, or ambiguous instance matching
fails instead of falling back to a name heuristic.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def ground_type(text):
    text = text.strip()
    match = re.fullmatch(r"(U|S)Int<(\d+)>", text)
    if match:
        return ("uint" if match[1] == "U" else "sint", int(match[2]))
    match = re.fullmatch(r"!firrtl\.(uint|sint)<(\d+)>", text)
    if match:
        return (match[1], int(match[2]))
    clocks = {"Clock": "clock", "Reset": "reset", "AsyncReset": "asyncreset"}
    if text in clocks:
        return (clocks[text],)
    if text in ("!firrtl.clock", "!firrtl.reset", "!firrtl.asyncreset"):
        return (text.split(".")[-1],)
    raise ValueError(f"route audit requires a supported ground type: {text}")


def read_firrtl(path):
    modules, current = {}, None
    with Path(path).open() as source:
        for line in source:
            match = re.match(r"  (?:ext)?module (\w+) :", line)
            if match:
                current = modules.setdefault(match[1], dict(ports={}, instances={}, edges=[]))
            if current is None:
                continue
            match = re.match(r"    (input|output) (\w+) : ([^@]+)", line)
            if match:
                current["ports"][match[2]] = (match[1], ground_type(match[3]))
            match = re.match(r"    inst (\w+) of (\w+)", line)
            if match:
                current["instances"][match[1]] = match[2]
            match = re.match(r"    ([\w.]+) <= ([\w.]+)(?:\s|$)", line)
            if match:
                current["edges"].append((match[1], match[2]))
    return modules


def read_mlir(path):
    modules, current = {}, None
    ground = r"!firrtl\.(?:uint<\d+>|sint<\d+>|clock|reset|asyncreset)"
    with Path(path).open() as source:
        for line in source:
            match = re.match(r"    firrtl.module @(\w+)\((.*)\)(?: attributes .*?)? \{", line)
            if match:
                current = modules.setdefault(match[1], dict(ports={}, instances={}, edges=[], ssa={}))
                for direction, name, type_ in re.findall(r"(in|out) %(\w+): (" + ground + ")", match[2]):
                    current["ports"][name] = ("input" if direction == "in" else "output", ground_type(type_))
                continue
            if current is None:
                continue
            match = re.match(r"      (.*?) = firrtl.instance (\w+) @(\w+)\((.*)\)", line)
            if match:
                values = re.findall(r"%(\w+)", match[1])
                pins = re.findall(r"(in|out) (\w+): (" + ground + ")", match[4])
                if len(values) != len(pins):
                    raise ValueError(f"unsupported instance signature: {match[2]}")
                current["instances"][match[2]] = match[3]
                for value, (_, pin, _) in zip(values, pins):
                    current["ssa"][value] = match[2] + "." + pin
            # Empty-interface instances have no SSA assignment.
            match = re.match(r"      firrtl.instance (\w+) @(\w+)\(\)", line)
            if match:
                current["instances"][match[1]] = match[2]
            match = re.match(r"      firrtl.(?:strictconnect|connect) %(\w+), %(\w+)\s*:", line)
            if match:
                current["edges"].append((match[1], match[2]))
    for module in modules.values():
        module["edges"] = [tuple(module["ssa"].get(value, value) for value in edge)
                           for edge in module["edges"]]
    return modules


def instance_bijection(golden, candidate, golden_modules, candidate_modules):
    expected, actual = golden["instances"], candidate["instances"]
    mapping = {name: name for name, module in expected.items() if actual.get(name) == module}
    for name, module in expected.items():
        if name in mapping:
            continue
        candidates = [other for other, child in actual.items()
                      if child == module and other not in mapping.values()]
        if len(candidates) != 1:
            raise ValueError(f"unmatched or ambiguous instance role {name}:{module}: {candidates}")
        mapping[name] = candidates[0]
    if len(mapping) != len(expected) or len(mapping) != len(actual):
        raise ValueError("wrapper instance counts differ")
    for original, renamed in mapping.items():
        left = golden_modules[expected[original]]["ports"]
        right = candidate_modules[actual[renamed]]["ports"]
        if left != right:
            raise ValueError(f"instance interfaces differ: {original} -> {renamed}")
    return mapping


def route_resolver(top_name, module, modules, renamed):
    graph = defaultdict(set)
    for destination, source in module["edges"]:
        graph[destination].add(source)
        graph[source].add(destination)

    def resolve(target):
        match = re.fullmatch(r"~([^|]+)\|([^>]+)>(\w+)", target)
        if not match or match[1] != top_name or match[2] != top_name:
            raise ValueError(f"expected a local wrapper port: {target}")
        port = match[3]
        if port not in module["ports"]:
            raise ValueError(f"unresolved wrapper port: {target}")
        seen, pending = {port}, [port]
        while pending:
            for neighbor in graph[pending.pop()] - seen:
                seen.add(neighbor)
                pending.append(neighbor)
        terminals, drivers = [], []
        direction, type_ = module["ports"][port]
        for value in seen:
            if "." in value:
                instance, pin = value.split(".")
                child = module["instances"][instance]
                pin_direction, pin_type = modules[child]["ports"][pin]
                if pin_type != type_:
                    raise ValueError(f"route changes types: {target} -> {value}")
                terminals.append((renamed.get(instance, instance), child, pin, pin_direction, pin_type))
                if pin_direction == "output":
                    drivers.append(value)
            elif value in module["ports"]:
                if module["ports"][value][1] != type_:
                    raise ValueError(f"wrapper alias changes types: {target} -> {value}")
                if module["ports"][value][0] == "input":
                    drivers.append(value)
            else:
                raise ValueError(f"route contains an unsupported intermediate declaration: {value}")
        if len(drivers) != 1:
            raise ValueError(f"route must have exactly one driver: {target}: {drivers}")
        if not terminals:
            terminals = [("external", port, direction, type_)]
        return dict(direction=direction, type=type_, terminals=sorted(terminals))
    return resolve


def channels(path):
    return [entry for entry in json.loads(Path(path).read_text())
            if entry.get("class") == "midas.passes.fame.FAMEChannelConnectionAnnotation"]


def resolved_channel(channel, resolve):
    result = dict(channel)
    result.pop("globalName")
    for member in ("sources", "sinks"):
        if result.get(member) is not None:
            result[member] = [resolve(target) for target in result[member]]
    if result.get("clock") is not None:
        result["clock"] = resolve(result["clock"])
    info = dict(result["channelInfo"])
    for member in ("readySink", "validSource", "readySource", "validSink"):
        if info.get(member) is not None:
            info[member] = resolve(info[member])
    result["channelInfo"] = info
    return json.dumps(result, sort_keys=True)


def compare(golden_fir, candidate_mlir, golden_annotations, candidate_annotations):
    golden, candidate = read_firrtl(golden_fir), read_mlir(candidate_mlir)
    expected, actual = channels(golden_annotations), channels(candidate_annotations)
    targets = [target for channel in expected + actual for member in ("sources", "sinks")
               for target in channel.get(member, []) or []]
    tops = {target.split("|", 1)[1].split(">", 1)[0] for target in targets}
    if len(tops) != 1:
        raise ValueError(f"expected one local inferred channel wrapper: {tops}")
    top = tops.pop()
    mapping = instance_bijection(golden[top], candidate[top], golden, candidate)
    resolve_golden = route_resolver(top, golden[top], golden, {})
    resolve_candidate = route_resolver(top, candidate[top], candidate, {value: key for key, value in mapping.items()})
    left, right = defaultdict(list), defaultdict(list)
    for channel in expected:
        left[resolved_channel(channel, resolve_golden)].append(channel["globalName"])
    for channel in actual:
        right[resolved_channel(channel, resolve_candidate)].append(channel["globalName"])
    match = Counter({key: len(value) for key, value in left.items()}) == Counter({key: len(value) for key, value in right.items()})
    return dict(match=match, golden_channels=len(expected), candidate_channels=len(actual),
                instance_bijection=mapping,
                name_deltas=[dict(golden=left[key], candidate=right[key], resolved_contract=json.loads(key))
                             for key in left.keys() & right.keys() if left[key] != right[key]],
                golden_only=[json.loads(key) for key in left.keys() - right.keys()],
                candidate_only=[json.loads(key) for key in right.keys() - left.keys()],
                scope="Actual direct wrapper instance/pin/type/driver routes and clock aliases; sequential logic is not proven.")
