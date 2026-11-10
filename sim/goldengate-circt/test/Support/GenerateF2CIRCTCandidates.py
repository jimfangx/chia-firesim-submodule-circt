#!/usr/bin/env python3
"""Compile archived, exact-four F2 pre-MIDAS inputs into isolated CIRCT candidates.

CHIA owns invocation and builds the configured compiler before using this helper.
A complete receipt establishes artifact preparation only, never semantic parity,
formal proof, runtime success or a compiler-speed result.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time
import uuid

import yaml


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SFC = load_module("f2_sfc_candidate_dependency", Path(__file__).with_name("GenerateF2SFCGoldens.py"))
TARGETS = tuple(SFC.TARGETS)
BASE = "FireSim-generated"
REQUIRED = tuple(BASE + suffix for suffix in
                 (".sv", ".const.h", ".const.vh", ".partition.const.h", ".peer2peer.const.yaml",
                  ".defines.vh", ".synthesis.xdc", ".implementation.xdc",
                  ".memory-origins.json", ".f2-cpu-stream-protocol.json")) + (
                     "post-bridge-extraction-all.json", "post-fame-xdc-all.json")
ALLOW_EMPTY = {BASE + suffix for suffix in (".defines.vh", ".synthesis.xdc", ".implementation.xdc")}
CLAIM = "none: compiler artifact preparation only; no semantic, Jasper, runtime or speed acceptance"


def digest(path):
    return SFC.digest(path)


def record(path):
    return SFC.file_record(Path(path).resolve())


def write_json(path, value, exclusive=False):
    SFC.write_json(path, value, exclusive)


def json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def checked_golden(repo, golden_root, target):
    """Use the gate's complete immutable SFC receipt/resource validation verbatim."""
    gate = load_module("f2_candidate_golden_validation", repo / "automation/chia_firesim_circt/verification_nodes.py")
    config = {"recipes_file": "automation/chia_firesim_circt/selected_config_build_recipes.yaml",
              "semantic": {"preparation": {"golden_root": str(golden_root)}}}
    receipt = gate._sfc_receipt(config, repo, target)
    if receipt is None:
        raise ValueError(f"no complete immutable SFC receipt: {target}")
    marker = golden_root / target / "complete.json"
    binding = json.loads(marker.read_text())["manifest"]
    manifest_path = Path(binding["path"])
    manifest_path = manifest_path if manifest_path.is_absolute() else repo / manifest_path
    manifest = json.loads(manifest_path.read_text())
    paths, allowed_empty = receipt
    def absolute_records(value):
        if isinstance(value, dict):
            result = {key: absolute_records(child) for key, child in value.items()}
            if {"path", "sha256", "bytes"} <= result.keys():
                source = Path(result["path"])
                result["path"] = str((source if source.is_absolute() else repo / source).resolve())
            return result
        if isinstance(value, list):
            return [absolute_records(child) for child in value]
        return value
    return {"receipt": record(marker), "manifest": record(manifest_path), "recipe": manifest["recipe"],
            "recipe_name": manifest["recipe_name"], "preMIDAS": absolute_records(manifest["preMIDAS"]),
            "files": [record(path) for path in paths], "allowed_empty": sorted(allowed_empty)}


def source_inventory(repo):
    """Track content and membership, including untracked native compiler sources."""
    compiler = repo / "sims/firesim/sim/goldengate-circt"
    paths = {compiler / "CMakeLists.txt", repo / "sims/firesim/sim/make/goldengate.mk",
             Path(__file__).resolve(), Path(__file__).with_name("GenerateF2SFCGoldens.py").resolve(),
             repo / "automation/chia_firesim_circt/verification_nodes.py",
             repo / "automation/chia_firesim_circt/verification.yaml"}
    for directory in ("include", "lib", "tools"):
        paths.update(path for path in (compiler / directory).rglob("*") if path.is_file())
    return [record(path) for path in sorted(paths)]


def prerequisites(repo, golden_root, compiler, threads, target):
    repo, golden_root, compiler = (Path(path).resolve() for path in (repo, golden_root, compiler))
    if target not in TARGETS:
        raise ValueError(f"unsupported target: {target}")
    affinity = sorted(os.sched_getaffinity(0))
    if isinstance(threads, bool) or not isinstance(threads, int) or not 1 <= threads <= len(affinity):
        raise ValueError("threads must be positive and fit the worker CPU affinity")
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        raise ValueError(f"configured native compiler is missing or not executable: {compiler}")
    selected = repo / "automation/chia_firesim_circt/selected_config_build_recipes.yaml"
    sample = repo / "sims/firesim-staging/sample_config_build_recipes.yaml"
    recipes = SFC.validate_recipes(yaml.safe_load(selected.read_text()), yaml.safe_load(sample.read_text()))
    golden = checked_golden(repo, golden_root, target)
    if golden["recipe"] != recipes[target]:
        raise ValueError(f"golden does not match selected recipe: {target}")
    # Compiler-emitted blackboxes can still consume the original annotation
    # filesystem paths. checked_golden binds those originals and archived bytes.
    return {"schema_version": 1, "target": target, "compiler": record(compiler),
            "source_files": source_inventory(repo), "selected_recipes": record(selected),
            "sample_recipes": record(sample), "golden": golden,
            "command_profile": {"compile_mode": "--compile-baseline", "output_filename_base": BASE,
                                "platform_config": recipes[target]["PLATFORM_CONFIG"],
                                "cwd": str(repo / "sims/firesim/sim"),
                                "threads_requested": threads, "cpu_affinity": affinity[:threads],
                                "mlir_threading": "native default; no disabled-threading flag; affinity capped"}}


def command(provenance, output):
    pre = provenance["golden"]["preMIDAS"]
    profile = provenance["command_profile"]
    return {"phase": "firesim_compiler", "timing_scope": "firesim_compiler",
            "cwd": profile["cwd"], "cpu_affinity": profile["cpu_affinity"],
            "argv": [provenance["compiler"]["path"], pre["firrtl"]["path"],
                     "--annotation-file", pre["annotations"]["path"], "--output-dir", str(output),
                     "--compile-baseline", "--platform-config", profile["platform_config"],
                     "--output-filename-base", BASE]}


def inventory(output):
    result = {}
    for path in sorted(output.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"candidate output contains a symlink: {path}")
        if path.is_file():
            result[str(path.relative_to(output))] = {"sha256": digest(path), "bytes": path.stat().st_size}
    return result


def finalize(output, cwd):
    """Include make's required blackbox append without hiding the raw result."""
    for name in REQUIRED:
        if name in ALLOW_EMPTY:
            if not (output / name).is_file():
                raise ValueError(f"missing required artifact: {output / name}")
        else:
            SFC.nonempty(output / name)
    for path in output.rglob("*.json"):
        if not isinstance(json.loads(path.read_text()), (dict, list)):
            raise ValueError(f"invalid generated JSON collateral: {path}")
    rtl = output / (BASE + ".sv")
    shutil.copy2(rtl, output / (BASE + ".raw.sv"))
    listing = output / "firrtl_black_box_resource_files.f"
    resources = []
    if listing.exists():
        for line in listing.read_text().splitlines():
            if not line.strip():
                continue
            source = Path(line.strip())
            source = (source if source.is_absolute() else cwd / source).resolve()
            SFC.nonempty(source)
            resources.append(source)
    external = output / "external-resources"
    external.mkdir()
    mappings = []
    with rtl.open("ab") as stream:
        for index, source in enumerate(resources):
            # Reading a resource from the final file would append recursively.
            if source in {rtl.resolve(), (output / (BASE + ".raw.sv")).resolve()}:
                raise ValueError("blackbox list references simulator RTL")
            before = record(source)
            with source.open("rb") as incoming:
                shutil.copyfileobj(incoming, stream)
            stream.write(b"\n")
            archived = source
            if not source.is_relative_to(output.resolve()):
                archived = external / f"{index}-{source.name}"
                shutil.copy2(source, archived)
            if record(source) != before or digest(archived) != before["sha256"]:
                raise ValueError(f"blackbox resource changed during append: {source}")
            mappings.append({"source": before, "archived": record(archived)})
    return mappings


def _read_receipt(output_root, target):
    marker = output_root / target / "complete.json"
    if not marker.exists():
        return None
    completion = json.loads(marker.read_text())
    if completion.get("status") != "complete" or completion.get("target") != target:
        raise ValueError(f"invalid candidate completion marker: {marker}")
    binding = completion.get("manifest", {})
    path = Path(binding.get("path", "")).resolve()
    target_root = (output_root / target).resolve()
    if not path.is_relative_to(target_root) or path.name != "manifest.json":
        raise ValueError("candidate manifest escapes selected target output root")
    if record(path) != binding:
        raise ValueError(f"candidate manifest hash mismatch: {path}")
    manifest = json.loads(path.read_text())
    if manifest.get("status") != "complete" or manifest.get("target") != target:
        raise ValueError(f"incomplete or wrong-target candidate: {path}")
    if manifest.get("input_identity") != json_digest(manifest.get("provenance")):
        raise ValueError(f"candidate input identity is corrupt: {path}")
    return marker, path, manifest


def validate_receipt(repo, golden_root, output_root, compiler, target, threads):
    """Return complete inventory; None is missing/stale, corruption raises.

    A stale marker remains preserved until a new complete attempt is published.
    Validation always rejects missing, changed or additional current outputs.
    """
    output_root = Path(output_root).resolve()
    found = _read_receipt(output_root, target)
    if found is None:
        return None
    marker, path, manifest = found
    expected = prerequisites(Path(repo), Path(golden_root), Path(compiler), threads, target)
    if manifest["input_identity"] != json_digest(expected):
        return None
    output = path.parent / "circt"
    if (manifest.get("recipe") != expected["golden"]["recipe"]
            or manifest.get("recipe_name") != expected["golden"]["recipe_name"]
            or manifest.get("command") != command(expected, output)
            or manifest.get("compiler_result", {}).get("exit_code") != 0):
        raise ValueError(f"complete candidate recipe/command/exit binding is invalid: {target}")
    actual = inventory(output)
    if actual != manifest.get("circt_inventory"):
        raise ValueError(f"complete candidate output membership/hash changed: {target}")
    if not set(REQUIRED) <= actual.keys() or any(actual[name]["bytes"] == 0 for name in REQUIRED if name not in ALLOW_EMPTY):
        raise ValueError(f"complete candidate lacks required outputs: {target}")
    SFC.recheck_recorded_files(manifest)
    files = {str(marker), str(path)}
    empty = set()
    for name, metadata in actual.items():
        artifact = str(output / name)
        files.add(artifact)
        if metadata["bytes"] == 0:
            empty.add(artifact)
    # The gate identity includes source, binary, immutable inputs and original
    # external resources consumed during generation, not just output filenames.
    def collect(value):
        if isinstance(value, dict):
            if {"path", "sha256", "bytes"} <= value.keys():
                files.add(value["path"])
                if value["bytes"] == 0:
                    empty.add(value["path"])
            for child in value.values():
                collect(child)
        elif isinstance(value, list):
            for child in value:
                collect(child)
    collect(manifest)
    return {"target": target, "manifest": str(path), "input_identity": manifest["input_identity"],
            "circt_artifacts": sorted(files), "allowed_empty": sorted(empty), "verification_claim": CLAIM}


def generate_target(repo, golden_root, output_root, compiler, target, threads):
    repo, golden_root, output_root, compiler = (Path(path).resolve() for path in
                                               (repo, golden_root, output_root, compiler))
    if output_root == golden_root or output_root.is_relative_to(golden_root) or golden_root.is_relative_to(output_root):
        raise ValueError("candidate and immutable SFC roots must be separate, non-nested directories")
    provenance = prerequisites(repo, golden_root, compiler, threads, target)
    target_root = output_root / target
    target_root.mkdir(parents=True, exist_ok=True)
    attempt = target_root / (time.strftime("attempt-%Y%m%dT%H%M%SZ-", time.gmtime()) + uuid.uuid4().hex[:8])
    attempt.mkdir()
    output = attempt / "circt"
    output.mkdir()
    planned = command(provenance, output)
    manifest = {"schema_version": 1, "status": "building", "target": target,
                "recipe_name": provenance["golden"]["recipe_name"], "recipe": provenance["golden"]["recipe"],
                "provenance": provenance, "input_identity": json_digest(provenance),
                "verification_claim": CLAIM, "command": planned,
                "startup_conditions": "fresh native process; OS page cache uncontrolled; no JVM invoked by helper",
                "benchmark_claim": "none: preparation can reuse receipts; not a repeated matched speed measurement"}
    path = attempt / "manifest.json"
    write_json(path, manifest, exclusive=True)
    started = time.monotonic()
    try:
        log = attempt / "compiler.log"
        with log.open("x") as stream:
            stream.write(json.dumps(planned) + "\n")
            stream.flush()
            result = subprocess.run(planned["argv"], cwd=planned["cwd"], stdout=stream, stderr=subprocess.STDOUT,
                                    preexec_fn=lambda: os.sched_setaffinity(0, planned["cpu_affinity"]))
        manifest["compiler_result"] = {"exit_code": result.returncode,
                                       "compiler_process_seconds": time.monotonic() - started,
                                       "peak_rss_kib_cumulative_children": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}
        manifest["compiler_log"] = record(log)
        if result.returncode:
            raise RuntimeError(f"native compiler exited {result.returncode}: {log}")
        manifest["resources"] = finalize(output, Path(planned["cwd"]))
        manifest["firesim_compiler_seconds_including_resource_append"] = time.monotonic() - started
        manifest["circt_inventory"] = inventory(output)
        if prerequisites(repo, golden_root, compiler, threads, target) != provenance:
            raise ValueError("compiler/source/config/input state changed during candidate generation")
        SFC.recheck_recorded_files(manifest)
        manifest["status"] = "complete"
        write_json(path, manifest)
        completion = attempt / "completion.json"
        write_json(completion, {"status": "complete", "target": target, "manifest": record(path)}, exclusive=True)
        # Old complete attempts remain immutable; only the target's current
        # pointer is replaced after the new attempt is completely validated.
        temporary = target_root / (".complete-" + uuid.uuid4().hex)
        os.link(completion, temporary)
        temporary.replace(target_root / "complete.json")
        return validate_receipt(repo, golden_root, output_root, compiler, target, threads)
    except BaseException as error:
        manifest.update(status="failed", error=str(error))
        try:
            manifest["partial_circt_inventory"] = inventory(output)
        except (OSError, ValueError) as inventory_error:
            manifest["partial_inventory_error"] = str(inventory_error)
        write_json(path, manifest)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[6])
    parser.add_argument("--golden-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--target", action="append", choices=TARGETS)
    parser.add_argument("--threads", type=int, default=len(os.sched_getaffinity(0)))
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args(argv)
    targets = list(dict.fromkeys(args.target or TARGETS))
    output_root = args.output_root.resolve()
    golden_root = args.golden_root.resolve()
    if output_root == golden_root or output_root.is_relative_to(golden_root) or golden_root.is_relative_to(output_root):
        raise ValueError("candidate and immutable SFC roots must be separate, non-nested directories")
    if args.check_only:
        checked = {}
        for target in targets:
            try:
                prerequisites(args.repo, args.golden_root, args.compiler, args.threads, target)
                checked[target] = {"status": "prerequisites_ready"}
            except Exception as error:
                checked[target] = {"status": "failed", "error": str(error)}
        print(json.dumps({"generation_run": False, "targets": checked, "verification_claim": CLAIM}, indent=2))
        return 0 if all(item["status"] == "prerequisites_ready" for item in checked.values()) else 1
    output_root.mkdir(parents=True, exist_ok=True)
    results = {}
    with (output_root / ".generation.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for target in targets:
            try:
                existing = validate_receipt(args.repo, args.golden_root, output_root, args.compiler, target, args.threads)
                receipt = existing or generate_target(args.repo, args.golden_root, output_root, args.compiler,
                                                      target, args.threads)
                results[target] = dict(status="complete", reused=existing is not None, **receipt)
                print("COMPLETE", target, receipt["manifest"], flush=True)
            except Exception as error:
                results[target] = {"status": "failed", "error": str(error)}
                print("FAILED", target, str(error), file=sys.stderr, flush=True)
        write_json(output_root / "generation-result.json", {"targets": results, "verification_claim": CLAIM})
    return 0 if all(item["status"] == "complete" for item in results.values()) else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
