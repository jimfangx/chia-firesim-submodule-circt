#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected SRAM/Print metadata and inspect the emitted queued joins.

Arguments: evidence directory from SRAMPrintBindingOracle, immutable U250 RTL.
Record order is reported separately: matching fields does not imply matching
packed record offsets or ordered decoder collateral.
"""
import json
import re
import sys
from pathlib import Path


def module(text, name):
    matches = re.findall(r"\bmodule\s+" + re.escape(name) + r"\s*\((.*?)\);(.*?)\bendmodule", text, re.S)
    assert len(matches) == 1, f"expected one {name} module"
    return matches[0]


def bindings(body, kind, name):
    matches = re.findall(r"\b" + re.escape(kind) + r"\s+" + re.escape(name) + r"\s*\((.*?)\);", body, re.S)
    assert len(matches) == 1, f"expected one {kind} {name} instance"
    return {key: value.strip() for key, value in
            re.findall(r"\.(\w+)\s*\(([^()]*)\)", matches[0])}


def bridge(annotations):
    values = [a for a in annotations if a.get("widgetClass") == "midas.widgets.PrintBridgeModule"]
    assert len(values) == 1, "expected one Print clock domain"
    return values[0]


evidence = Path(sys.argv[1])
candidate = evidence / "candidate"
reference = json.loads((evidence / "sfc-post-ram.json").read_text())
native = json.loads((candidate / "post-print-host-binding-all.json").read_text())
rb, nb = bridge(reference), bridge(native)
assert rb["clockInfo"] == nb["clockInfo"], "Print rational clock differs"
assert rb["channelMapping"] == nb["channelMapping"], "Print channel mapping differs"
rk, nk = rb["widgetConstructorKey"], nb["widgetConstructorKey"]
assert rk["resetPortName"] == nk["resetPortName"]
records = lambda key: {p["name"]: p for p in key["printPorts"]}
assert records(rk) == records(nk), "Print formats, argument order or types differ"
fields = {nk["resetPortName"]: nk["resetPortName"]}
for record in nk["printPorts"]:
    for port in record["ports"]:
        assert len(port) == 1
        field = next(iter(port))
        fields[record["name"] + "_" + field] = record["name"] + "." + field
assert fields.keys() == nb["channelMapping"].keys()
globals_ = set(nb["channelMapping"].values())
assert len(globals_) == 18, "selected Print leaf count changed"
channels = lambda annotations: {a["globalName"]: a for a in annotations
    if a.get("class") == "midas.passes.fame.FAMEChannelConnectionAnnotation"
    and a["globalName"] in globals_}
rc, nc = channels(reference), channels(native)
assert rc.keys() == nc.keys() == globals_
for local, global_ in nb["channelMapping"].items():
    r, n = rc[global_], nc[global_]
    assert r["channelInfo"] == n["channelInfo"] == {
        "class": "midas.passes.fame.PipeChannel", "latency": 0}
    assert r["clock"].split("|", 1)[1] == n["clock"].split("|", 1)[1]
    assert n["sources"] == [f"~GGPrintBridgeHostWrapper|GGFAMEPipeWrapper>FireSim_{global_}_source.bits"]
    assert n["sinks"] == [f"~GGPrintBridgeHostWrapper|GGPrintBridgeHostQueued>hBits.{fields[local]}"]

rtl = (candidate / "post-print-host-binding.sv").read_text()
rtl = re.sub(r"//[^\n]*|/\*.*?\*/", "", rtl, flags=re.S)
_, wrapper = module(rtl, "GGPrintBridgeHostWrapper")
sim = bindings(wrapper, "GGFAMEPipeWrapper", "sim")
host = bindings(wrapper, "GGPrintBridgeHostQueued", "PrintBridgeModule_0")
aliases = dict(re.findall(r"\bwire\s+(\w+)\s*=\s*([^;]+);", wrapper))


def terms(expression, active=()):
    result = set()
    for value in expression.split("&"):
        value = value.strip()
        assert re.fullmatch(r"\w+", value), f"unsupported join term: {value}"
        assert value not in active, "cyclic join alias"
        result.update(terms(aliases[value], active + (value,)) if value in aliases else (value,))
    return result


valids = {sim[f"FireSim_{g}_source_valid"] for g in globals_}
assert len(valids) == 18
assert terms(host["hValid"]) == valids, "host valid lost a token"
for local, global_ in nb["channelMapping"].items():
    port = f"FireSim_{global_}_source"
    assert terms(sim[port + "_ready"]) == (valids - {sim[port + "_valid"]}) | {host["hReady"]}, "ready join differs"
    assert host["hBits_" + local] == sim[port + "_bits"], "payload bypassed its queued endpoint"
for signal in ("hostClock", "hostReset"):
    assert host[signal] == sim[signal] == signal
_, transport = module(rtl, "GGFAMEPipeWrapper")
for global_ in globals_:
    kinds = re.findall(r"\b(GGFAMEPipe\d+_L0)\s+PipeChannel_" + re.escape(global_) + r"\s*\(", transport)
    assert len(kinds) == 1, "missing latency-zero Print transport"
    pipe = bindings(transport, kinds[0], "PipeChannel_" + global_)
    assert pipe["clock"] == "hostClock" and pipe["reset"] == "hostReset"
    for field in ("ready", "valid", "bits"):
        assert pipe["io_out_" + field] == f"FireSim_{global_}_source_{field}"

golden = Path(sys.argv[2]).read_text()
golden_header, golden_queue = module(golden, "Queue_50")
native_header, _ = module(rtl, "GGPrintBridgeCPUQueue6144")
_, native_ram = module(rtl, "ram_6144x512")
assert re.search(r"reg\s+\[511:0\]\s+ram\s*\[0:6143\]", golden_queue)
assert re.search(r"reg\s+\[511:0\]\s+Memory\s*\[0:6143\]", native_ram)
assert re.search(r"output\s+\[12:0\]\s+io_count", golden_header)
assert re.search(r"output\s+\[12:0\]\s+count", native_header)
report = {
    "print_domains": 1, "scalar_pipe_channels": len(globals_),
    "constructor_fields_and_clock_match": True, "queued_joins_and_payload_routes_match": True,
    "golden_queue_geometry": {"depth": 6144, "width": 512, "count_width": 13},
    "golden_rtl": str(Path(sys.argv[2])),
    "reference_record_order": [p["name"] for p in rk["printPorts"]],
    "candidate_record_order": [p["name"] for p in nk["printPorts"]],
}
report["record_order_matches"] = report["reference_record_order"] == report["candidate_record_order"]
(evidence / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS 18 selected-SRAM Print channels: constructor fields, clocks, queued joins and payload routes")
print("PASS immutable U250 Queue_50 geometry: 6144 x 512, 13-bit count")
print("Print record-order match:", report["record_order_matches"])
