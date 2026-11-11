#!/usr/bin/env python3
"""Compare exactly four matched compiler output directories and route snapshots.

Input JSON is a list of four objects with name, golden, candidate and
candidate_routes directory paths. Debug snapshots must reproduce the candidate
RTL, all generated sidecars and inferred channel annotations exactly. This
report checks static contracts and explicitly distinguishes the native F2 ACK
protocol extension from exact SFC ABI equality. It is not a formal proof, a
syntax/lint result, or permission to waive unexplained differences.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess

import CompareF2Semantics as semantics
import CompareFAMEBoundaryRoutes as routes
import NICDriverHeaderCompare as headers
import CompileContractProvenance as provenance


BASE = semantics.BASE
EXTRA_MODEL_CLASSES = tuple("midas.passes.fame." + name for name in (
    "ModelReadPort", "ModelWritePort", "ModelReadWritePort")) + (
    "midas.targetutils.FirrtlMemModelAnnotation",
    "midas.targetutils.FirrtlFAMEModelAnnotation",
    "midas.targetutils.FirrtlEnableModelMultiThreadingAnnotation")


def annotation_file(directory, stage):
    for suffix in (".json", "-all.json"):
        path = directory / (stage + suffix)
        if path.is_file():
            return path
    raise ValueError(f"missing annotations: {directory / stage}")


def macros(text):
    result = {}
    for match in re.finditer(r"^`define\s+([A-Za-z_]\w*)([^\n]*)", text, re.M):
        if match[1] in result:
            raise ValueError(f"duplicate header macro: {match[1]}")
        result[match[1]] = match[2].strip()
    if not result:
        raise ValueError("empty generated Verilog header")
    return result


def compare_pair(pair, destination, verilator, metasim_top):
    golden, candidate = (Path(pair[key]).resolve() for key in ("golden", "candidate"))
    snapshots = Path(pair["candidate_routes"]).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    inputs, checks, inventory = {}, {}, {}

    def bind(path):
        path = path.resolve()
        if inventory and any(path.is_relative_to(directory) for directory in (golden, candidate, snapshots)) and str(path) not in inventory:
            raise ValueError(f"compared artifact is absent from compiler receipt inventory: {path}")
        if str(path) not in inputs:
            inputs[str(path)] = dict(bytes=path.stat().st_size, sha256=semantics.digest(path))
        if str(path) in inventory and inputs[str(path)] != inventory[str(path)]:
            raise ValueError(f"artifact differs from successful compiler receipt: {path}")
        return path

    def text(directory, suffix):
        return bind(directory / (BASE + "." + suffix)).read_text()

    checks["provenance"], inventory = provenance.validate(pair, bind)
    for source in (Path(__file__), Path(semantics.__file__), Path(routes.__file__),
                   Path(headers.__file__), Path(provenance.__file__)):
        bind(source)

    # Debug compilation must reproduce every final sidecar as well as the RTL;
    # do not bind routes from a different compiler/profile into this candidate.
    artifact_names = sorted(path.name for path in candidate.glob(BASE + ".*"))
    if not artifact_names:
        raise ValueError("missing native output inventory")
    snapshot_binding = {}
    for name in artifact_names:
        left, right = bind(candidate / name), bind(snapshots / name)
        snapshot_binding[name] = inputs[str(left)] == inputs[str(right)]
    infer_golden = bind(annotation_file(golden, "post-infer-model-ports"))
    infer_candidate = bind(annotation_file(candidate, "post-infer-model-ports"))
    infer_snapshot = bind(annotation_file(snapshots, "post-infer-model-ports"))
    snapshot_binding["post-infer-model-ports"] = inputs[str(infer_candidate)] == inputs[str(infer_snapshot)]
    checks["snapshot_binding"] = dict(match=all(snapshot_binding.values()), artifacts=snapshot_binding)
    if not checks["snapshot_binding"]["match"]:
        raise ValueError("native route snapshots do not reproduce candidate artifacts exactly")

    checks["bridge_extraction"] = semantics.boundary(
        bind(annotation_file(golden, "post-bridge-extraction")),
        bind(annotation_file(candidate, "post-bridge-extraction")), semantics.BRIDGES)
    checks["infer_model_ports"] = semantics.boundary(
        infer_golden, infer_candidate, semantics.MODELS + EXTRA_MODEL_CLASSES)
    checks["resolved_fame_routes"] = routes.compare(
        bind(golden / "post-infer-model-ports.fir"),
        bind(snapshots / "post-infer-model-ports.mlir"), infer_golden, infer_candidate)
    rtl = [text(directory, "sv") for directory in (golden, candidate)]
    for name in ("F1Shim", "FPGATop"):
        checks["ports_" + name] = semantics.compare_values(*(semantics.ports(source, name) for source in rtl))
    protocol_path = candidate / (BASE + ".f2-cpu-stream-protocol.json")
    protocol = json.loads(bind(protocol_path).read_text()) if protocol_path.exists() else None
    checks["driver_abi"] = semantics.compare_driver_contracts(
        *(semantics.driver_contract(text(directory, "const.h"), headers) for directory in (golden, candidate)),
        protocol=protocol)
    checks["implementation_clocks"] = semantics.compare_implementation_clocks(
        *(semantics.implementation_clocks(text(directory, "implementation.xdc"), source)
          for directory, source in zip((golden, candidate), rtl)))
    for suffix, parser in (("peer2peer.const.yaml", semantics.stream_metadata),
                           ("partition.const.h", semantics.partition_contract)):
        checks[suffix] = semantics.compare_values(*(parser(text(directory, suffix)) for directory in (golden, candidate)))
    for suffix in ("defines.vh", "synthesis.xdc"):
        checks[suffix] = semantics.compare_values(*(text(directory, suffix) for directory in (golden, candidate)))

    # This is the actual metasim Verilog consumer. Preprocessing both complete
    # headers plus that source proves whether a macro delta affects this source;
    # an unused/extra macro is never silently erased from the report.
    bind(metasim_top)
    bind(verilator)
    outputs, commands = [], []
    for label, directory in (("golden", golden), ("candidate", candidate)):
        header = bind(directory / (BASE + ".const.vh"))
        argv = [str(verilator), "-E", "-P", "+define+CLOCK_PERIOD=10", str(header), str(metasim_top)]
        run = subprocess.run(argv, text=True, capture_output=True, timeout=60)
        (destination / (label + "-metasim-preprocessed.sv")).write_text(run.stdout)
        (destination / (label + "-metasim-preprocessor.stderr")).write_text(run.stderr)
        commands.append(dict(argv=argv, returncode=run.returncode))
        outputs.append(run.stdout)
    macro_comparison = semantics.compare_values(*(macros(text(directory, "const.vh")) for directory in (golden, candidate)))
    checks["metasim_header"] = dict(match=all(command["returncode"] == 0 for command in commands) and bool(outputs[0].strip()) and outputs[0] == outputs[1],
                                   macros=macro_comparison, commands=commands,
                                   scope="Byte-identical preprocessed actual metasim top; all macro deltas remain listed.")
    match = all(check.get("match") is True for check in checks.values())
    result = dict(name=pair["name"], static_contracts_match=match,
                  strict_sfc_abi_match=checks["driver_abi"].get("raw_match") is True,
                  strict_sfc_contracts_match=match and checks["driver_abi"].get("raw_match") is True,
                  equivalence_proven=False, checks=checks, inputs=inputs,
                  proof_obligations=semantics.DEFERRED_OBLIGATIONS)
    (destination / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verilator", type=Path, required=True)
    parser.add_argument("--metasim-top", type=Path,
                        default=Path(__file__).resolve().parents[3] / "midas/src/main/verilog/top.sv")
    args = parser.parse_args()
    pairs = json.loads(args.manifest.read_text())
    if not isinstance(pairs, list) or len(pairs) != 4:
        parser.error("manifest must select exactly four matched targets")
    names = [pair["name"] for pair in pairs]
    if len(set(names)) != 4 or any(not re.fullmatch(r"[A-Za-z0-9_-]+", name) for name in names):
        parser.error("target names must be four unique directory-safe labels")
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for pair in pairs:
        try:
            result = compare_pair(pair, args.output / pair["name"], args.verilator.resolve(), args.metasim_top.resolve())
        except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            result = dict(name=pair["name"], static_contracts_match=False, equivalence_proven=False,
                          error=f"{type(error).__name__}: {error}")
        results.append(result)
        print(json.dumps({key: value for key, value in result.items() if key not in ("inputs", "checks")}))
    report = dict(static_contracts_match=all(result["static_contracts_match"] for result in results),
                  equivalence_proven=False, targets=results,
                  manifest=dict(path=str(args.manifest.resolve()), bytes=args.manifest.stat().st_size,
                                sha256=semantics.digest(args.manifest)))
    (args.output / "result.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    raise SystemExit(0 if report["static_contracts_match"] else 1)


if __name__ == "__main__":
    main()
