#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected native Print response wiring with immutable SFC NastiRouter.

The reference has printf disabled and eleven single-beat normal banks. Compare
shared retirement/ID semantics, specializing normal R.last to one only when
comparing with that optimized fixture. The error endpoint retains burst last.
"""
import json
import re
import sys
from pathlib import Path


def module(text, name):
    matches = re.findall(r"\bmodule\s+" + re.escape(name) + r"\s*\((.*?)\);(.*?)\bendmodule", text, re.S)
    assert len(matches) == 1, f"expected one {name}"
    return matches[0]


def clean(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def equations(body):
    return dict(re.findall(r"\b(?:wire(?:\s*\[[^]]+\])?|assign)\s+(\w+)\s*=\s*([^;]+);", body))


def instance(body, kind, name):
    matches = re.findall(r"\b" + kind + r"\s+" + name + r"\s*\((.*?)\);", body, re.S)
    assert len(matches) == 1, f"expected one {kind} {name}"
    return {key: value.strip() for key, value in re.findall(r"\.(\w+)\s*\(([^()]*)\)", matches[0])}


def terms(expression, eq, names, active=()):
    result = set()
    for term in expression.split("&"):
        term = term.strip()
        if term in names:
            result.add(names[term])
        else:
            assert term in eq and term not in active, f"unresolved term {term}"
            result.update(terms(eq[term], eq, names, active + (term,)))
    return result


def without_targets(value):
    if isinstance(value, str) and value.startswith("~"):
        return "<retargeted-reference>"
    if isinstance(value, list):
        return [without_targets(item) for item in value]
    if isinstance(value, dict):
        return {key: without_targets(item) for key, item in value.items()}
    return value


def main():
    evidence, golden = map(Path, sys.argv[1:])
    candidate = evidence / "candidate"
    native = clean((candidate / "post-print-control-responses.sv").read_text())
    reference = clean(golden.read_text())
    _, router = module(reference, "NastiRouter")
    rq = equations(router)
    _, read_tracker = module(native, "GGControlReadTrackerWrapper")
    _, read_arbiter = module(native, "GGControlReadArbiterWrapper")
    _, write_arbiter = module(native, "GGControlWriteArbiterWrapper")
    _, write_tracker = module(native, "GGControlWriteTrackerWrapper")
    rt = instance(read_tracker, "GGControlReadTracker", "controlReadTracker")
    wt = instance(write_tracker, "GGControlWriteTracker", "controlWriteTracker")
    r_inner = instance(read_tracker, "GGControlReadDispatchWrapper", "sim")
    w_inner = instance(write_tracker, "GGControlWriteArbiterWrapper", "sim")
    for tracker, inner, ready, accepted, tag, route in (
        (rt, r_inner, "ctrl_read_dispatch_tracker_ready", "ctrl_read_dispatch_track_valid",
         "ctrl_read_dispatch_track_tag", "ctrl_read_dispatch_track_target"),
        (wt, w_inner, "ctrl_write_route_aw_tracker_ready", "ctrl_write_route_aw_track_valid",
         "ctrl_write_dispatch_master_aw_bits_id", "ctrl_decode_aw_target"),
    ):
        assert tracker["enq_ready"] == inner[ready]
        assert tracker["enq_valid"] == inner[accepted]
        assert tracker["enq_bits_tag"] == inner[tag]
        assert tracker["enq_bits_data"] == inner[route]
    retirement = {}
    for channel, queue, body, kind, name in (
        ("r", "ar", read_arbiter, "GGControlReadArbiter", "readArbiter"),
        ("b", "aw", write_arbiter, "GGControlWriteArbiter", "writeArbiter"),
    ):
        arb = instance(body, kind, name)
        inner = instance(body, "GGControlReadTrackerWrapper" if channel == "r" else "GGControlReadArbiterWrapper", "sim")
        for i, bank in enumerate(("print_0_ctrl", "cpuStream_ctrl", "ctrl_error")):
            prefix = f"{bank}_{channel}_"
            ap = f"in_{i}_"
            for field in ("ready", "valid", "bits_id") + (("bits_last",) if channel == "r" else ()):
                assert arb[ap + field] == inner[prefix + field], f"{bank} {channel} {field} detached"
            if channel == "r":
                names = {r_inner[prefix + "ready"]: "ready", r_inner[prefix + "valid"]: "valid",
                         r_inner[prefix + "bits_last"]: "last"}
                actual = terms(rt[f"deq_{i}_valid"], equations(read_tracker), names)
                assert actual == {"ready", "valid", "last"}
                assert rt[f"deq_{i}_tag"] == r_inner[prefix + "bits_id"]
            else:
                names = {arb[ap + "ready"]: "ready", arb[ap + "valid"]: "valid"}
                actual = terms(equations(body)[f"ctrl_write_tracker_deq_{i}_valid"], equations(body), names)
                assert actual == {"ready", "valid"}
                assert equations(body)[f"ctrl_write_tracker_deq_{i}_tag"].strip() == arb[ap + "bits_id"]
                for field in ("valid", "tag"):
                    assert wt[f"deq_{i}_{field}"] == w_inner[f"ctrl_write_tracker_deq_{i}_{field}"]
            ref_index = i if i < 2 else 11
            rp = f"io_slave_{ref_index}_{channel}_" if i < 2 else f"err_slave_io_{channel}_"
            names = {rp + "ready": "ready", rp + "valid": "valid"}
            if channel == "r":
                names[rp + "bits_last"] = "last"
            expected = terms(rq[f"{queue}_queue_io_deq_{ref_index}_valid"], rq, names)
            # SFC has constant final-beat normal MCR responses. Its error
            # endpoint and this native general response boundary keep R.last.
            specialized = actual - {"last"} if channel == "r" and i < 2 else actual
            assert specialized == expected, f"{bank} retirement differs from SFC"
            assert rq[f"{queue}_queue_io_deq_{ref_index}_tag"].strip() == rp + "bits_id"
            retirement[f"{bank}.{channel}"] = sorted(actual)
    mlir = (candidate / "post-print-control-responses.mlir").read_text()
    for helper in ("GGControlReadTracker", "GGControlWriteTracker"):
        _, body = module(native, helper)
        for stem in ("roq_data_", "roq_tags_", "roq_free_"):
            assert set(map(int, re.findall(r"\breg(?:\s*\[[^]]+\])?\s+" + stem + r"(\d+)\s*;", body))) == set(range(64))
    _, reference_queue = module(reference, "ReorderQueue")
    # SFC eliminated unused deq.data outputs and hence route-data storage.
    # Native diagnostic route outputs remain explicit at this boundary.
    for stem in ("roq_tags_", "roq_free_"):
        assert set(map(int, re.findall(r"\breg(?:\s*\[[^]]+\])?\s+" + stem + r"(\d+)\s*;", reference_queue))) == set(range(64))
    assert mlir.count("goldengate.trackerTagWidth = 12 : i32") == 2
    before = json.loads((candidate / "post-print-control-dispatch-all.json").read_text())
    after = json.loads((candidate / "post-print-control-responses-all.json").read_text())
    assert without_targets(before) == without_targets(after), "response mapping changed annotation classes/payload/order"
    report = {"golden": str(golden), "candidate": str(candidate / "post-print-control-responses.sv"),
              "retirement": retirement, "tracker_slots": 64, "tag_width": 12,
              "enqueue_capacity_id_route_connected": True, "annotation_payloads_preserved": True,
              "reference_normal_r_last": 1, "reference_has_print_host": False,
              "reference_unused_route_storage_eliminated": True}
    (evidence / "control-responses-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS six Print/count/error retirement predicates, response IDs and tracker/arbiter connectivity versus SFC")


if __name__ == "__main__":
    main()
