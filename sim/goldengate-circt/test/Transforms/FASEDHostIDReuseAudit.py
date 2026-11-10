#!/usr/bin/env python3
"""Read-only constructor/host-cone audit of the four fixed immutable F2 goldens.

This is source/geometry evidence for native idReuse 1/2 guards. It neither runs
RTL nor establishes native/SFC behavioral equivalence, full-target parity,
formal proof, F2 execution, or compiler speed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

import yaml


TARGETS = {
    "rocket": ("firesim_rocket_singlecore_no_nic_l2_lbp", 1, 10),
    "megaboomv3": ("firesim_megaboom_singlecore_no_nic_l2_llc4mb_ddr3", 2, 20),
    "shuttle": ("firesim_shuttle_singlecore_no_nic_l2_llc4mb_ddr3", 1, 10),
    "saturn_shuttle": (
        "firesim_saturn_GENV256D128_shuttle_singlecore_no_nic_l2_llc4mb_ddr3", 2, 20),
}
HOST_CONES = ("AXI4Deinterleaver", "AXI4Buffer_1", "AXI4Xbar", "AXI4Buffer")
NATIVE_STAGES = (
    "FASEDReadDeinterleaver", "FASEDHostMemoryBuffer", "HostMemoryWriteArbiter",
    "HostMemoryWriteResponses", "HostMemoryReadArbiter", "HostMemoryReadResponses",
    "HostMemoryOutputBuffer", "HostMemoryPort")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def file_identity(path):
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": digest(path)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def dictionaries(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from dictionaries(child)
    elif isinstance(value, list):
        for child in value:
            yield from dictionaries(child)


def cone(modules, top):
    selected = {}

    def visit(name):
        if name in selected:
            return
        selected[name] = modules[name]
        candidates = set(re.findall(r"\b(\w+)\s+\w+\s*\(", modules[name]))
        # Keep the same module-file-order traversal used by existing component
        # closure() helpers; all literal bodies are retained unchanged.
        for child in modules:
            if child != name and child in candidates:
                visit(child)

    visit(top)
    text = "\n".join(selected.values())
    normalized = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    for index, name in enumerate(selected):
        normalized = re.sub(r"\b" + re.escape(name) + r"\b",
                            "Module" + str(index), normalized)
    normalized = re.sub(r"\s+", "", normalized)
    return {"modules": list(selected), "sha256": hashlib.sha256(text.encode()).hexdigest(),
            "normalized_sha256": hashlib.sha256(normalized.encode()).hexdigest()}


def anchors(root, relative, snippets):
    path = root / relative
    lines = path.read_text().splitlines()
    matches = []
    for snippet in snippets:
        found = [index + 1 for index, line in enumerate(lines) if snippet in line]
        require(bool(found), f"missing source anchor {relative}: {snippet}")
        matches.append({"contains": snippet, "lines": found})
    return {**file_identity(path), "anchors": matches}


def run(args, report):
    root = args.repository.resolve()
    base = root / "sims/firesim/sim/goldengate-circt"
    selected_path = root / "automation/chia_firesim_circt/selected_config_build_recipes.yaml"
    sample_path = root / "sims/firesim-staging/sample_config_build_recipes.yaml"
    selected = yaml.safe_load(selected_path.read_text())
    sample = yaml.safe_load(sample_path.read_text())
    require(set(selected) == {entry[0] for entry in TARGETS.values()},
            "selected recipes must contain exactly the four fixed targets")
    for target, (name, _, _) in TARGETS.items():
        if target != "shuttle":
            require(selected[name] == sample[name], f"sample recipe changed for {target}")
        else:
            derived = dict(selected[TARGETS["megaboomv3"][0]])
            derived["TARGET_CONFIG"] = (
                "WithDefaultFireSimBridges_WithFireSimTestChipConfigTweaks_chipyard.ShuttleConfig")
            require(selected[name] == derived, "plain Shuttle recipe derivation changed")
    report["recipes"] = {"selected": file_identity(selected_path),
                         "sample": file_identity(sample_path), "exact_entries": selected}
    report["policy_sources"] = [file_identity(Path(__file__).resolve())]
    report["policy_sources"] += [file_identity(base / "lib/Transforms" / (name + ".cpp"))
                                  for name in NATIVE_STAGES]
    report["policy_sources"] += [file_identity(base / "test/Transforms/HostMemoryProfileChecks.h")]
    report["oracle_source_anchors"] = [
        anchors(root, "sims/firesim/sim/rocket-chip/src/main/scala/amba/axi4/Deinterleaver.scala", [
            "val beats = maxBeats(edgeOut.slave)", "val maxFlightPerId = Seq.tabulate(endId)",
            "if (mf > 0)", "entries = beats", "log2Ceil(beats+1)"]),
        anchors(root, "sims/firesim/sim/rocket-chip/src/main/scala/amba/axi4/Buffer.scala", [
            "this(BufferParams.default)", "config.depth, pipe=config.pipe, flow=config.flow"]),
        anchors(root, "sims/firesim/sim/midas/src/main/scala/midas/core/FPGATop.scala", [
            "p(HostMemIdSpaceKey) match", "case None", "AXI4Deinterleaver(bridge.memorySlaveConstraints.supportsRead.max)"]),
        anchors(root, "sims/firesim/sim/midas/src/main/scala/midas/models/dram/FASEDMemoryTimingModel.scala", [
            "maxFlight = Some(math.max(cfg.maxReadsPerID, cfg.maxWritesPerID))"]),
        anchors(root, "sims/firesim/sim/midas/src/main/scala/midas/models/dram/EgressUnit.scala", [
            "virtualState > 0x20000", "maxReqLength * maxReqsPerId",
            "log2Up(maxReqsPerId + 1)"]),
    ]
    for target, (recipe_name, reuse, flight) in TARGETS.items():
        receipt_path = args.goldens.resolve() / target / "complete.json"
        receipt_identity = file_identity(receipt_path)
        receipt = json.loads(receipt_path.read_text())
        require(receipt["status"] == "complete" and receipt["target"] == target,
                f"incomplete/mismatched receipt {target}")
        manifest_path = Path(receipt["manifest"]["path"])
        manifest_identity = file_identity(manifest_path)
        require(manifest_identity["sha256"] == receipt["manifest"]["sha256"],
                f"manifest digest mismatch {target}")
        manifest = json.loads(manifest_path.read_text())
        require(manifest["status"] == "complete" and manifest["target"] == target,
                f"incomplete/mismatched manifest {target}")
        require(manifest["recipe_name"] == recipe_name and manifest["recipe"] == selected[recipe_name],
                f"golden recipe mismatch {target}")
        archived = []
        for value in dictionaries(manifest["preMIDAS"]):
            if not {"path", "sha256", "bytes"} <= value.keys():
                continue
            path = Path(value["path"])
            # Live source paths are provenance. Validate archived immutable
            # resources and handed-off FIRRTL/annotations inside the attempt.
            if not path.resolve().is_relative_to(manifest_path.parent.resolve()):
                continue
            actual = file_identity(path)
            require(actual == {key: value[key] for key in actual}, f"archived input mismatch {path}")
            archived.append(actual)
        require(bool(archived), f"missing archived inputs {target}")
        constructors = [value for value in dictionaries(json.loads(
            Path(manifest["preMIDAS"]["annotations"]["path"]).read_text()))
            if "axi4Edge" in value]
        require(len(constructors) == 1, f"expected one FASED constructor {target}")
        constructor = constructors[0]
        require(constructor["axi4Widths"] == {"addrBits": 35, "dataBits": 64, "idBits": 4},
                f"target widths changed {target}")
        edge = constructor["axi4Edge"]
        require((edge["idReuse"], edge["maxFlight"], edge["maxReadTransfer"],
                 edge["maxWriteTransfer"]) == (reuse, flight, 8, 8), f"edge changed {target}")
        rtl_path = manifest_path.parent / "sfc/FireSim-generated.sv"
        rtl_identity = file_identity(rtl_path)
        require(rtl_identity["sha256"] == manifest["sfc_artifacts"][rtl_path.name]["sha256"],
                f"RTL mismatch {target}")
        text = rtl_path.read_text()
        modules = {match.group(1): match.group(0) for match in re.finditer(
            r"(?ms)^module\s+(\w+)\b.*?^endmodule\b[^\n]*(?:\n|$)", text)}
        cones = {name: cone(modules, name) for name in HOST_CONES + ("ReadEgress", "WriteEgress")}
        physical = re.findall(
            r"(?:input |output)\s+(\[[^]]+\])?\s*(mem_0_(?:aw_bits_id|ar_bits_id|aw_bits_addr|w_bits_data))",
            modules["FPGATop"])
        require(physical == [("[15:0]", "mem_0_aw_bits_id"), ("[33:0]", "mem_0_aw_bits_addr"),
                             ("[63:0]", "mem_0_w_bits_data"), ("[15:0]", "mem_0_ar_bits_id")],
                f"physical memory fields changed {target}")
        report["targets"].append({"target": target, "constructor": constructor,
                                  "receipt": receipt_identity, "manifest": manifest_identity,
                                  "sfc": rtl_identity, "archived_preMIDAS_files": archived,
                                  "cones": cones, "physical_mem0_declared_fields": physical})
    for name in HOST_CONES:
        require(len({target["cones"][name]["sha256"] for target in report["targets"][1:]}) == 1,
                f"selected DDR3 literal cone differs {name}")
        require(len({target["cones"][name]["normalized_sha256"] for target in report["targets"]}) == 1,
                f"fixed-four normalized host cone differs {name}")
    report["selected_ddr3_host_cones_literal_identical"] = True
    report["fixed_four_host_cones_normalized_identical"] = True
    for target in report["targets"]:
        for identity in [target["receipt"], target["manifest"], target["sfc"]] + target["archived_preMIDAS_files"]:
            require(file_identity(Path(identity["path"])) == identity, f"input changed during audit {identity['path']}")
    for identity in report["policy_sources"] + report["oracle_source_anchors"] + [
            report["recipes"]["selected"], report["recipes"]["sample"]]:
        require(file_identity(Path(identity["path"]))["sha256"] == identity["sha256"],
                f"source/policy changed during audit {identity['path']}")
    report["immutable_inputs_and_policy_unchanged"] = True
    report["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--goldens", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), "output must be fresh")
    report = {"scope": __doc__, "command": [sys.executable] + sys.argv,
              "normalization": ["Remove // line comments and /* block comments */ from copied cones.",
                                "Rename only whole module-name identifiers to Module0, Module1, etc. in deterministic dependency order, including declarations and instantiations.",
                                "Remove whitespace; retain every other token, width, payload, constant, queue depth and statement unchanged.",
                                "Literal cone identity among the three DDR3 targets uses none of these normalizations."],
              "limits": ["No native-versus-SFC RTL execution or semantic comparison in this audit.",
                         "Only four named host dependency cones are required equal; ReadEgress/WriteEgress are recorded and differ with reuse.",
                         "Physical memory evidence covers four declared fields, not complete FPGATop wiring equivalence.",
                         "Live original resource paths are provenance; archived pre-MIDAS copies inside each immutable attempt are validated."],
              "targets": [], "passed": False}
    try:
        run(args, report)
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        with args.output.open("x") as destination:
            json.dump(report, destination, indent=2)
            destination.write("\n")
    print(f"PASS fixed-four host ID-reuse source/cone audit: {args.output}")


if __name__ == "__main__":
    main()
