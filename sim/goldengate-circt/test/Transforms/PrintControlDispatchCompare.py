#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print MMIO dispatch against the immutable SFC NastiRouter.

This fixture has printf disabled. Compare shared request/backpressure semantics,
not a claim of Print-enabled platform equivalence or identical address maps.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path


def module(text, name):
    matches = re.findall(r"\bmodule\s+" + re.escape(name) + r"\s*\((.*?)\);(.*?)\bendmodule", text, re.S)
    assert len(matches) == 1, f"expected one {name} module"
    return matches[0]


def clean(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def equations(body):
    return dict(re.findall(r"\b(?:wire(?:\s*\[[^]]+\])?|assign)\s+(\w+)\s*=\s*([^;]+);", body))


def terms(expression, eq, names, active=()):
    result = set()
    for term in expression.split("&"):
        term = term.strip()
        if term in names:
            result.add(names[term])
        else:
            assert term in eq and term not in active, f"unresolved handshake term {term}"
            result.update(terms(eq[term], eq, names, active + (term,)))
    return result


def instance(body, kind, name):
    matches = re.findall(r"\b" + re.escape(kind) + r"\s+" + re.escape(name) + r"\s*\((.*?)\);", body, re.S)
    assert len(matches) == 1, f"expected one {kind} {name} instance"
    return dict(re.findall(r"\.(\w+)\s*\(\s*(\w+)\s*\)", matches[0]))


evidence, golden = Path(sys.argv[1]), Path(sys.argv[2])
candidate = evidence / "candidate"
native = clean((candidate / "post-print-control-dispatch.sv").read_text())
reference = clean(golden.read_text())
_, router = module(reference, "NastiRouter")
_, read = module(native, "GGControlReadDispatch")
_, write = module(native, "GGControlWriteDispatch")
_, route = module(native, "GGControlWriteRoute")
rq, nr, nw, nq = map(equations, (router, read, write, route))
# Treat selected slave readiness as an independent predicate. The dispatcher
# tests separately evaluate priority/error selection and arbitrary stalls.
queue_ready = nq["aw_ready"].split("&")[0].strip()
queue_valid = nq["w_ready"].split("&")[0].strip()
read_ready = nr["master_ar_ready"].split("&")[1].strip()
write_names = {"aw_valid": "aw_valid", "w_valid": "w_valid", queue_ready: "route_queue_ready",
               queue_valid: "route_queue_valid", "aw_tracker_ready": "aw_tracker_ready",
               "aw_slave_ready": "aw_slave_ready", "w_slave_ready": "w_slave_ready"}
reference_names = {"io_master_aw_valid": "aw_valid", "io_master_w_valid": "w_valid",
                   "io_master_ar_valid": "ar_valid", "aw_queue_io_enq_ready": "aw_tracker_ready",
                   "ar_queue_io_enq_ready": "ar_tracker_ready", "w_queue_io_enq_ready": "route_queue_ready",
                   "w_queue_io_deq_valid": "route_queue_valid", "aw_ready": "aw_slave_ready",
                   "w_ready": "w_slave_ready", "ar_ready": "ar_slave_ready",
                   "aw_route[0]": "aw_selected", "ar_route[0]": "ar_selected",
                   "w_queue_io_deq_bits[0]": "w_selected"}
handshakes = {}
for native_output, reference_output in {"aw_ready": "io_master_aw_ready", "w_ready": "io_master_w_ready",
                                      "aw_track_valid": "aw_queue_io_enq_valid"}.items():
    actual = terms(nq[native_output], nq, write_names)
    expected = terms(rq[reference_output], rq, reference_names)
    assert actual == expected, f"{native_output}: {actual} != {expected}"
    handshakes[native_output] = sorted(actual)
for channel in ("aw", "w"):
    actual = terms(nq[channel + "_slave_valid"], nq, write_names) | {channel + "_selected"}
    expected = terms(rq["io_slave_0_" + channel + "_valid"], rq, reference_names)
    assert actual == expected, f"{channel} dispatch predicate differs"
    assert nw["slave_0_" + channel + "_valid"].strip() == channel + "_valid & " + channel + "_route[0]"
    handshakes[channel + "_slave_valid"] = sorted(actual)
read_names = {"master_ar_valid": "ar_valid", "tracker_ready": "ar_tracker_ready",
              read_ready: "ar_slave_ready", "route[0]": "ar_selected"}
for native_output, reference_output in {"master_ar_ready": "io_master_ar_ready",
                                      "slave_0_ar_valid": "io_slave_0_ar_valid",
                                      "track_valid": "ar_queue_io_enq_valid"}.items():
    actual = terms(nr[native_output], nr, read_names)
    expected = terms(rq[reference_output], rq, reference_names)
    assert actual == expected, f"{native_output} predicate differs"
    handshakes[native_output] = sorted(actual)
for channel, fields in {"aw": ("addr", "len", "id"), "ar": ("addr", "len", "id"), "w": ("data",)}.items():
    eq = nr if channel == "ar" else nw
    for field in fields:
        assert eq[f"slave_0_{channel}_bits_{field}"].strip() == f"master_{channel}_bits_{field}"
        assert rq[f"io_slave_0_{channel}_bits_{field}"].strip() == f"io_master_{channel}_bits_{field}"
# Follow the actual emitted instance nets from the selected dispatcher through
# the bound Print control and count adapter, including flipped readiness.
_, writes = module(native, "GGControlWidgetWriteWrapper")
_, reads = module(native, "GGControlReadDispatchWrapper")
_, counts = module(native, "GGCPUStreamControlWrapper")
inner = instance(writes, "GGControlWriteDispatchWrapper", "sim")
read_inner = instance(reads, "GGControlWidgetWriteWrapper", "sim")
dispatch = instance(reads, "GGControlReadDispatch", "controlReadDispatch")
for slave, bank in enumerate(("print_0_ctrl", "cpuStream_ctrl")):
    for channel, fields in {"aw": ("valid", "ready", "bits_addr", "bits_len", "bits_id"),
                            "w": ("valid", "ready", "bits_data", "bits_last")}.items():
        for field in fields:
            signal = inner[f"ctrl_write_dispatch_slave_{slave}_{channel}_{field}"]
            control = inner[f"{bank}_{channel}_{field}"]
            assert signal == control, f"{bank} {channel} {field} detached from dispatch"
    for field in ("ready", "valid", "bits_addr", "bits_len", "bits_id"):
        assert read_inner[f"{bank}_ar_{field}"] == dispatch[f"slave_{slave}_ar_{field}"]
count_top = instance(counts, "GGCPUStreamCountWrapper", "sim")
count_adapter = instance(counts, "GGCPUStreamMCRFile", "crFile")
assert count_top["cpuStream_mcr_read_0_bits"] == count_adapter["mcr_read_0_bits"], "live count detached from AXI adapter"
mlir = (candidate / "post-print-control-dispatch.mlir").read_text()
assert 'name = "PrintBridgeModule_0", size = 32 : i64, slave = 0 : i32, start = 0 : i64' in mlir
assert 'name = "CPUManagedStreamEngine_0", size = 4 : i64, slave = 1 : i32, start = 32 : i64' in mlir
before = json.loads((candidate / "post-print-cpu-streams-all.json").read_text())
after = json.loads((candidate / "post-print-control-dispatch-all.json").read_text())
assert Counter(a['class'] for a in before) == Counter(a['class'] for a in after), "control mapping changed annotation classes"
def without_targets(value):
    if isinstance(value, str) and value.startswith("~"):
        return "<retargeted-reference>"
    if isinstance(value, list):
        return [without_targets(item) for item in value]
    if isinstance(value, dict):
        return {key: without_targets(item) for key, item in value.items()}
    return value
assert without_targets(before) == without_targets(after), "control mapping changed annotation payloads/order"
report = {"golden": str(golden), "candidate": str(candidate / "post-print-control-dispatch.sv"),
          "handshakes": handshakes, "banks": [{"name": "PrintBridgeModule_0", "words": 6, "base": 0, "bytes": 32},
                                               {"name": "CPUManagedStreamEngine_0", "words": 1, "base": 32, "bytes": 4}],
          "request_and_count_connected": True, "annotation_classes_preserved": True,
          "reference_has_print_host": False, "response_arbitration": "explicit boundary"}
(evidence / "control-dispatch-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS Print MMIO request/count connectivity and eight SFC NastiRouter handshake predicates")
