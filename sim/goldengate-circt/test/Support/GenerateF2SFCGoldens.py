#!/usr/bin/env python3
"""Generate compiler-only SFC archives for the fixed four selected F2 recipes.

No Vivado, manager launch, or CIRCT invocation occurs here. An exclusive
complete.json is the sole completion marker; incomplete attempts are evidence
only. See GenerateF2SFCGoldens.md for provenance limits and invocation.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time
import uuid
from zipfile import ZipFile

import yaml


TARGETS = {
    "rocket": ("firesim_rocket_singlecore_no_nic_l2_lbp",
               "WithDefaultFireSimBridges_WithFireSimHighPerfConfigTweaks_chipyard.RocketConfig",
               "BaseF2Config"),
    "megaboomv3": ("firesim_megaboom_singlecore_no_nic_l2_llc4mb_ddr3",
                   "WithDefaultFireSimBridges_WithFireSimTestChipConfigTweaks_chipyard.MegaBoomV3Config",
                   "WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config"),
    "shuttle": ("firesim_shuttle_singlecore_no_nic_l2_llc4mb_ddr3",
                "WithDefaultFireSimBridges_WithFireSimTestChipConfigTweaks_chipyard.ShuttleConfig",
                "WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config"),
    "saturn_shuttle": ("firesim_saturn_GENV256D128_shuttle_singlecore_no_nic_l2_llc4mb_ddr3",
                       "FireSimGENV256D128ShuttleConfig",
                       "WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config"),
}
JAR_HASHES = {
    "firechip.jar": "92fbf3a96d8da096b14543d019f6bbd1815a6f707cbc22535ea9ec82c8a65aff",
    "firesim-main.jar": "16ae60498397d40c57008cf86d3d6ec4e5b2a0c097783b52380e3ecd02357057",
}
SOURCE_PINS = {
    "chipyard": "a4e4afff6794d1fee7cfbab00708535f80aeaf10",
    "firesim": "5d8ebb741325ba12e5820e58d50088cf97f07310",
}
SOURCE_SCOPES = {
    "chipyard": ["build.sbt", "project", "generators", "tools/stage", "tools/cde",
                 "tools/dsptools", "tools/firrtl", "tools/fixedpoint", "tools/rocket-dsp-utils"],
    "firesim": ["sim/build.sbt", "sim/project", "sim/midas", "sim/firesim-lib", "sim/src",
                "sim/berkeley-hardfloat", "sim/cde", "sim/diplomacy", "sim/rocket-chip"],
}
BOOTROM_RESOURCES = [
    "generators/testchipip/src/main/resources/testchipip/bootrom/bootrom.rv32.img",
    "generators/testchipip/src/main/resources/testchipip/bootrom/bootrom.rv64.img",
    "generators/rocket-chip/bootrom/bootrom.img",
]
REQUIRED_SFC = ["FireSim-generated" + suffix for suffix in
                [".sv", ".const.h", ".const.vh", ".defines.vh", ".synthesis.xdc",
                 ".implementation.xdc", ".partition.const.h", ".peer2peer.const.yaml"]] + [
                     "post-bridge-extraction.json", "post-fame5-transform.json",
                     "post-sim-mapping.fir"]
ALLOW_EMPTY_SFC = {"FireSim-generated.peer2peer.const.yaml"}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def git(cwd, *args):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


def file_record(path):
    return {"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size}


def recheck_recorded_files(value):
    if isinstance(value, dict):
        if {"path", "sha256", "bytes"} <= value.keys():
            path = Path(value["path"])
            if not path.is_file() or digest(path) != value["sha256"] or path.stat().st_size != value["bytes"]:
                raise ValueError(f"recorded file changed during generation: {path}")
        for child in value.values():
            recheck_recorded_files(child)
    elif isinstance(value, list):
        for child in value:
            recheck_recorded_files(child)


def validate_recipes(selected, sample):
    if set(selected) != {value[0] for value in TARGETS.values()}:
        raise ValueError("selected YAML must contain exactly the four fixed F2 recipes")
    for target, (name, config, platform) in TARGETS.items():
        recipe = selected[name]
        if (recipe.get("PLATFORM"), recipe.get("DESIGN"), recipe.get("TARGET_PROJECT"),
                recipe.get("TARGET_CONFIG"), recipe.get("PLATFORM_CONFIG")) != (
                    "f2", "FireSim", "firesim", config, platform):
            raise ValueError(f"incorrect fixed recipe/platform semantics: {target}")
        if target == "shuttle":
            expected = dict(sample[TARGETS["megaboomv3"][0]], TARGET_CONFIG=config)
        else:
            expected = sample[name]
        if recipe != expected:
            raise ValueError(f"recipe differs from sample/explicit Shuttle derivation: {target}")
    return {target: selected[data[0]] for target, data in TARGETS.items()}


def source_state(repo, receipt):
    """Corroborate the recorded source receipt, without claiming jar rebuilds."""
    result = {}
    historical = receipt["commits"]
    for label, cwd in [("chipyard", repo), ("firesim", repo / "sims/firesim")]:
        pin = SOURCE_PINS[label]
        receipt_key = "/scratch/jfx/fsim-circt" + ("/sims/firesim" if label == "firesim" else "")
        if historical.get(receipt_key) != pin:
            raise ValueError(f"historical receipt source pin mismatch: {label}")
        scopes = SOURCE_SCOPES[label]
        # This compares tracked source content and dependency gitlinks with the
        # archived revision, including current working-tree changes.
        drift = git(cwd, "diff", "--name-only", "--ignore-submodules=dirty", pin, "--", *scopes)
        untracked = git(cwd, "ls-files", "--others", "--exclude-standard", "--", *scopes)
        if drift or untracked:
            raise ValueError(f"source differs from archived jar receipt ({label}): {drift or untracked}")
        old_dirty = receipt.get("worktree_status", {}).get(receipt_key, "").splitlines()
        for line in old_dirty:
            path = line[3:]
            if any(path == scope or path.startswith(scope + "/") for scope in scopes):
                raise ValueError(f"historical receipt has unrecorded source modifications: {line}")
        dependencies = []
        for entry in git(cwd, "ls-tree", "-r", pin, "--", *scopes).splitlines():
            metadata, path = entry.split("\t", 1)
            mode, kind, revision = metadata.split()
            if mode != "160000":
                continue
            dependency = cwd / path
            if not (dependency / ".git").exists():
                raise ValueError(f"source dependency is not initialized: {dependency}")
            actual = git(dependency, "rev-parse", "HEAD")
            status = git(dependency, "status", "--porcelain", "--untracked-files=normal", "--ignore-submodules=none")
            if actual != revision or status:
                raise ValueError(f"dependency differs from source receipt: {dependency}: {actual} {status}")
            dependencies.append({"path": path, "revision": revision, "tracked_and_untracked_clean": True})
        result[label] = {"repository": str(cwd), "archived_source_revision": pin,
                         "current_revision": git(cwd, "rev-parse", "HEAD"),
                         "source_scopes": scopes, "source_diff": [], "dependencies": dependencies}
    return result


def java_runtime_record(java):
    java = java.resolve(strict=True)
    version = subprocess.run([str(java), "-version"], text=True, capture_output=True, check=True)
    home = java.parent.parent
    # Conda's OpenJDK package omits the otherwise customary release file.
    # Bind the actual runtime image and VM library regardless of that metadata.
    runtime = [file_record(home / path) for path in ["lib/modules", "lib/server/libjvm.so"]]
    release = home / "release"
    return dict(file_record(java), version=version.stdout + version.stderr,
                runtime_files=runtime, release_metadata=file_record(release) if release.is_file() else None)


def prerequisites(repo, java, archive):
    selected_path = repo / "automation/chia_firesim_circt/selected_config_build_recipes.yaml"
    sample_path = repo / "sims/firesim-staging/sample_config_build_recipes.yaml"
    recipes = validate_recipes(yaml.safe_load(selected_path.read_text()),
                               yaml.safe_load(sample_path.read_text()))
    receipt_path = archive / "manifest.json"
    receipt = json.loads(receipt_path.read_text())
    jars = {name: archive / "toolchain" / name for name in JAR_HASHES}
    for name, path in jars.items():
        if receipt["toolchain"].get(name) != JAR_HASHES[name] or digest(path) != JAR_HASHES[name]:
            raise ValueError(f"jar differs from pinned historical receipt: {path}")
    classes = {}
    for name, path in jars.items():
        with ZipFile(path) as jar:
            classes[name] = set(jar.namelist())
    required_target_classes = {"chipyard/Generator.class", "firechip/chip/FireSim.class"}
    for _, config, _ in TARGETS.values():
        for fragment in config.split("_"):
            qualified = fragment if "." in fragment else "firechip.chip." + fragment
            required_target_classes.add(qualified.replace(".", "/") + ".class")
    if not required_target_classes <= classes["firechip.jar"]:
        raise ValueError("archived generator jar lacks exact selected target classes")
    for fragment in {part for _, _, platform in TARGETS.values() for part in platform.split("_")}:
        if not any(f"{package}/{fragment}.class" in classes["firesim-main.jar"] for package in
                   ["firesim/firesim", "firesim/configs", "firesim/util", "midas"]):
            raise ValueError(f"archived SFC jar lacks platform fragment: {fragment}")
    sources = source_state(repo, receipt)
    return recipes, jars, {
        "schema_version": 1, "scope": "exact-four F2 pre-MIDAS and SFC compiler artifact generation",
        "verification_claim": "none: no syntax, semantic, Jasper, runtime, or speed acceptance",
        "provenance_level": "historical jar receipt plus current scoped source/dependency identity",
        "provenance_limitation": "Jars were not independently rebuilt; the historical receipt is not a reproducible-build attestation.",
        "historical_receipt": file_record(receipt_path), "sources": sources,
        "recipes": {"selected": file_record(selected_path), "sample": file_record(sample_path),
                    "shuttle_derivation": "copy exact sample MegaBoom recipe; change only TARGET_CONFIG"},
        "java": java_runtime_record(java),
        "jars": {name: file_record(path) for name, path in jars.items()},
        "elaboration_resources": [file_record(repo / path) for path in BOOTROM_RESOURCES],
        "generator_script": file_record(Path(__file__).resolve()),
        "affinity_cores": sorted(os.sched_getaffinity(0)),
    }


def commands(repo, java, jars, recipe, attempt, heap_gb, threads):
    inputs = attempt / "inputs"
    output = attempt / "sfc"
    long_name = "firechip.chip.FireSim." + recipe["TARGET_CONFIG"]
    jvm = [str(java.resolve()), f"-Xmx{heap_gb}G", "-Xss8M", f"-XX:ActiveProcessorCount={threads}",
           "-Djava.io.tmpdir=" + str(attempt / "java-tmp")]
    return [
        {"phase": "target_elaboration", "cwd": str(repo / "sims/firesim-staging"),
         "argv": jvm + ["-cp", str(jars["firechip.jar"]), "chipyard.Generator",
                         "--target-dir", str(inputs), "--name", long_name,
                         "--top-module", "firechip.chip.FireSim", "--legacy-configs",
                         "firechip.chip:" + recipe["TARGET_CONFIG"], "--emit-legacy-sfc"]},
        {"phase": "firesim_compiler", "timing_scope": "firesim_compiler",
         "cwd": str(repo / "sims/firesim/sim"),
         "argv": jvm + ["-cp", str(jars["firesim-main.jar"]), "midas.stage.GoldenGateMain",
                         "-i", str(inputs / (long_name + ".sfc.fir")), "-td", str(output),
                         "-faf", str(inputs / (long_name + ".anno.json")), "-ggcp", "firesim.firesim",
                         "-ggcs", recipe["PLATFORM_CONFIG"], "--output-filename-base", "FireSim-generated",
                         "--allow-unrecognized-annotations", "--no-dedup"]},
    ]


def inventory(root):
    return {str(path.relative_to(root)): {"sha256": digest(path), "bytes": path.stat().st_size}
            for path in sorted(root.rglob("*")) if path.is_file()}


def nonempty(path):
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"missing/empty required artifact: {path}")


def validate_inputs(inputs, recipe):
    long_name = "firechip.chip.FireSim." + recipe["TARGET_CONFIG"]
    fir = inputs / (long_name + ".sfc.fir")
    annos = inputs / (long_name + ".anno.json")
    nonempty(fir)
    nonempty(annos)
    if not isinstance(json.loads(annos.read_text()), list):
        raise ValueError("pre-MIDAS annotations are not an array")
    with fir.open() as stream:
        if "circuit FireSim" not in "".join(next(stream, "") for _ in range(8)):
            raise ValueError("pre-MIDAS FIRRTL does not declare circuit FireSim")
    return fir, annos


def archive_input_resources(annos, destination, cwd, jars):
    """Retain inline, classpath and filesystem dependencies without changing input."""
    result = []
    for index, anno in enumerate(json.loads(annos.read_text())):
        kind = anno.get("class", "").rsplit(".", 1)[-1]
        if kind == "BlackBoxInlineAnno":
            content = anno["text"].encode()
            origin = {"annotation_index": index, "inline_name": anno["name"]}
        elif kind == "BlackBoxResourceAnno":
            name = anno["resourceId"].lstrip("/")
            matches = []
            for jar in jars.values():
                with ZipFile(jar) as source:
                    if name in source.namelist():
                        matches.append((jar, source.read(name)))
            if not matches or len({hashlib.sha256(data).hexdigest() for _, data in matches}) != 1:
                raise ValueError(f"missing/ambiguous classpath resource: {name}")
            content = matches[0][1]
            origin = {"annotation_index": index, "classpath_resource": name,
                      "source_jars": [str(path) for path, _ in matches]}
        elif kind in {"BlackBoxPathAnno", "MemoryFileInlineAnnotation", "LoadMemoryAnnotation"}:
            name = next((anno[key] for key in ["path", "filename", "fileName"] if key in anno), None)
            if not name:
                raise ValueError(f"unknown file annotation schema: {anno}")
            path = Path(name)
            path = path if path.is_absolute() else cwd / path
            nonempty(path)
            content = path.read_bytes()
            origin = {"annotation_index": index, "source": file_record(path.resolve())}
        else:
            continue
        path = destination / f"annotation-{index}.resource"
        path.write_bytes(content)
        result.append(dict(origin, archived=file_record(path)))
    return result


def finalize_sfc(output, cwd):
    for name in REQUIRED_SFC:
        if name in ALLOW_EMPTY_SFC:
            if not (output / name).is_file():
                raise ValueError(f"missing required artifact: {output / name}")
        else:
            nonempty(output / name)
    for path in output.glob("*.json"):
        if not isinstance(json.loads(path.read_text()), (list, dict)):
            raise ValueError(f"invalid SFC JSON collateral: {path}")
    rtl = output / "FireSim-generated.sv"
    # Preserve the raw compiler result too. FireSim's SFC make rule appends
    # these resources to the simulator RTL; that required step is timed here.
    shutil.copy2(rtl, output / "FireSim-generated.raw.sv")
    resources = []
    filelist = output / "firrtl_black_box_resource_files.f"
    if filelist.exists():
        for line in filelist.read_text().splitlines():
            if not line.strip():
                continue
            path = Path(line.strip())
            path = path if path.is_absolute() else cwd / path
            nonempty(path)
            resources.append(path.resolve())
    with rtl.open("ab") as stream:
        for path in resources:
            with path.open("rb") as source:
                shutil.copyfileobj(source, stream)
            stream.write(b"\n")
    # Most resources already reside in output; retain external ones as well.
    external = output / "external-resources"
    external.mkdir()
    result = []
    for index, path in enumerate(resources):
        archived = path
        if not path.is_relative_to(output.resolve()):
            archived = external / f"{index}-{path.name}"
            shutil.copy2(path, archived)
        result.append({"source": file_record(path), "archived": file_record(archived)})
    return result


def write_json(path, value, exclusive=False):
    with path.open("x" if exclusive else "w") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def run_phase(command, log):
    started = time.monotonic()
    with log.open("x") as stream:
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        result = subprocess.run(command["argv"], cwd=command["cwd"], stdout=stream,
                                stderr=subprocess.STDOUT, env={key: value for key, value in os.environ.items()
                                                             if key not in {"JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"}})
    record = dict(command, exit_code=result.returncode, seconds=time.monotonic() - started,
                  peak_rss_kib_cumulative_children=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    if result.returncode:
        raise RuntimeError(f"{command['phase']} exited {result.returncode}: {log}")
    return record


def reserve_attempt(root, target):
    target_root = root / target
    if (target_root / "complete.json").exists():
        raise ValueError(f"completed golden is immutable: {target_root}; use a fresh output root")
    target_root.mkdir(parents=True, exist_ok=True)
    attempt = target_root / (time.strftime("attempt-%Y%m%dT%H%M%SZ-", time.gmtime()) + uuid.uuid4().hex[:8])
    attempt.mkdir()
    return attempt


def generate_target(repo, java, jars, root, target, recipe, provenance, heap_gb, threads):
    attempt = reserve_attempt(root, target)
    for name in ["inputs", "sfc", "java-tmp", "preMIDAS-resources"]:
        (attempt / name).mkdir()
    record = {"status": "building", "target": target, "recipe_name": TARGETS[target][0],
              "recipe": recipe, "attempt": str(attempt), "provenance": provenance,
              "jvm_conditions": "fresh JVM for target elaboration and SFC; OS page cache uncontrolled",
              "threads": threads, "heap_gb": heap_gb, "commands": [],
              "removed_environment_overrides": ["JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"]}
    path = attempt / "manifest.json"
    write_json(path, record, exclusive=True)
    try:
        argv = commands(repo, java, jars, recipe, attempt, heap_gb, threads)
        record["planned_commands"] = argv
        write_json(path, record)
        record["commands"].append(run_phase(argv[0], attempt / "elaboration.log"))
        fir, annos = validate_inputs(attempt / "inputs", recipe)
        record["preMIDAS"] = {"firrtl": file_record(fir), "annotations": file_record(annos)}
        resources = attempt / "preMIDAS-resources"
        record["preMIDAS"]["annotation_resources"] = archive_input_resources(
            annos, resources, repo / "sims/firesim/sim", jars)
        record["preMIDAS"]["bootrom_resources"] = []
        for index, name in enumerate(BOOTROM_RESOURCES):
            archived = resources / f"bootrom-{index}.img"
            shutil.copy2(repo / name, archived)
            record["preMIDAS"]["bootrom_resources"].append(
                {"source": file_record(repo / name), "archived": file_record(archived)})
        compiler_start = time.monotonic()
        record["commands"].append(run_phase(argv[1], attempt / "sfc.log"))
        record["sfc_resources"] = finalize_sfc(attempt / "sfc", repo / "sims/firesim/sim")
        record["firesim_compiler_seconds_including_collateral_append"] = time.monotonic() - compiler_start
        record["benchmark_claim"] = "none; single untuned generation, not a speed comparison"
        if digest(fir) != record["preMIDAS"]["firrtl"]["sha256"] or digest(annos) != record["preMIDAS"]["annotations"]["sha256"]:
            raise ValueError("pre-MIDAS inputs changed during SFC execution")
        record["artifacts"] = inventory(attempt / "inputs")
        record["sfc_artifacts"] = inventory(attempt / "sfc")
        record["resource_artifacts"] = inventory(resources)
        record["logs"] = {name: file_record(attempt / name) for name in ["elaboration.log", "sfc.log"]}
        recheck_recorded_files(record)
        receipt = json.loads(Path(provenance["historical_receipt"]["path"]).read_text())
        if source_state(repo, receipt) != provenance["sources"]:
            raise ValueError("Scala source/dependency state changed during generation")
        record["status"] = "complete"
        write_json(path, record)
        # Publish an exclusive immutable marker only after every output was
        # produced and hashed. Partial/failed attempts never receive a marker.
        completion = attempt / "completion.json"
        write_json(completion, {"status": "complete", "target": target,
                   "manifest": file_record(path)}, exclusive=True)
        os.link(completion, attempt.parent / "complete.json")
        return record
    except BaseException as error:
        record.update(status="failed", error=str(error))
        write_json(path, record)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[6])
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--target", choices=list(TARGETS), action="append")
    parser.add_argument("--java", type=Path)
    parser.add_argument("--historical-archive", type=Path)
    parser.add_argument("--check-only", action="store_true", help="validate prerequisites and print argv; no generation or writes")
    parser.add_argument("--heap-gb", type=int, default=24)
    parser.add_argument("--threads", type=int, default=len(os.sched_getaffinity(0)))
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    root = args.output_root.resolve()
    java = args.java or repo / ".conda-env/bin/java"
    archive = args.historical_archive or repo / "automation/chia_firesim_circt/golden/u250-sfc-20261013"
    if args.threads < 1 or args.threads > len(os.sched_getaffinity(0)) or args.heap_gb < 1:
        parser.error("threads must fit affinity and heap-gb must be positive")
    recipes, jars, provenance = prerequisites(repo, java, archive.resolve())
    selected = list(dict.fromkeys(args.target or TARGETS))
    if args.check_only:
        print(json.dumps({"status": "prerequisites_ready", "generation_run": False, "provenance": provenance,
              "targets": {target: {"recipe": recipes[target], "commands": commands(repo, java, jars,
                          recipes[target], root / target / "ATTEMPT-ALLOCATED-AT-RUN", args.heap_gb, args.threads)}
                          for target in selected}}, indent=2))
        return 0
    if os.environ.get("FIRESIM_SOURCED") != "1":
        parser.error("initialize Conda and source sims/firesim/sourceme-manager.sh before generation")
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".generation.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if any((root / target / "complete.json").exists() for target in selected):
            raise ValueError("selected output is already complete and immutable; use a fresh output root or another --target")
        toolchain = root / "toolchain"
        toolchain.mkdir(exist_ok=True)
        for name, source in jars.items():
            destination = toolchain / name
            if destination.exists():
                if digest(destination) != JAR_HASHES[name]:
                    raise ValueError(f"existing archived jar changed: {destination}")
            else:
                shutil.copy2(source, destination)
        jars = {name: toolchain / name for name in jars}
        provenance["execution_jars"] = {name: file_record(path) for name, path in jars.items()}
        for target in selected:
            print("GENERATING", target, flush=True)
            generate_target(repo, java, jars, root, target, recipes[target], provenance, args.heap_gb, args.threads)
            print("COMPLETE", target, root / target / "complete.json", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
