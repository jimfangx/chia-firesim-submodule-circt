"""Fail-closed provenance for paired enabled SFC/native compiler receipts."""
from __future__ import annotations

import json
from pathlib import Path

import CompareF2Semantics as semantics


def option(argv, name):
    if not isinstance(argv, list) or not all(isinstance(arg, str) for arg in argv) or argv.count(name) != 1:
        raise ValueError(f"expected exactly one compiler option {name}")
    index = argv.index(name)
    if index + 1 >= len(argv):
        raise ValueError(f"missing compiler option value: {name}")
    return argv[index + 1]


def identity(record):
    return {key: record[key] for key in ("bytes", "sha256")}


def validate(pair, bind):
    directories = {key: Path(pair[key]).resolve() for key in ("golden", "candidate", "candidate_routes")}
    receipts = {}
    for key, directory in directories.items():
        receipts[key] = json.loads(bind(directory / "receipt.json").read_text())
        if type(receipts[key].get("returncode")) is not int or receipts[key]["returncode"] != 0:
            raise ValueError(f"unsuccessful compiler receipt: {directory}")
    golden, candidate, snapshot = (receipts[key] for key in directories)
    archive_path = semantics.bound_file(golden["original_manifest"])
    archive = json.loads(bind(archive_path).read_text())
    if archive.get("status") != "complete" or archive.get("target") != pair["name"]:
        raise ValueError("wrong or incomplete shared elaboration manifest")
    expected_profile = "WithMultiCycleRamModels_" + archive["recipe"]["PLATFORM_CONFIG"]
    for key, receipt in receipts.items():
        if receipt.get("target") != pair["name"]:
            raise ValueError(f"wrong target in {key} compiler receipt")
        profile = option(receipt["argv"], "-ggcs" if key == "golden" else "--platform-config")
        declared_profile = receipt.get("enabled_profile") if key == "golden" else receipt.get("profile")
        if profile != expected_profile or declared_profile != expected_profile:
            raise ValueError(f"wrong enabled profile in {key}: expected {expected_profile}, got {profile}/{declared_profile}")
        output = option(receipt["argv"], "-td" if key == "golden" else "--output-dir")
        if Path(output).resolve() != directories[key]:
            raise ValueError(f"receipt output directory differs: {key}")
        for field, flag in (("input", "-i" if key == "golden" else None),
                            ("annotations", "-faf" if key == "golden" else "--annotation-file")):
            record = receipt[field]
            path = semantics.bound_file(record)
            bind(path)
            selected = option(receipt["argv"], flag) if flag else receipt["argv"][1]
            if Path(selected).resolve() != path.resolve():
                raise ValueError(f"receipt {field} differs from compiler command: {key}")
        compiler_field = "compiler_classpath" if key == "golden" else "compiler"
        compiler = semantics.bound_file(receipt[compiler_field])
        bind(compiler)
        selected = option(receipt["argv"], "-cp") if key == "golden" else receipt["argv"][0]
        if Path(selected).resolve() != compiler.resolve():
            raise ValueError(f"compiler executable/classpath differs from receipt: {key}")

    # SFC uses the archived FIRRTL2 form and native uses the CHIRRTL form from
    # the SAME archived elaboration. A same-named fresh file is insufficient.
    expected_input = archive["preMIDAS"]["firrtl"]
    expected_annotations = archive["preMIDAS"]["annotations"]
    for record in (expected_input, expected_annotations):
        bind(semantics.bound_file(record))
    if identity(golden["input"]) != identity(expected_input) or Path(golden["input"]["path"]).resolve() != Path(expected_input["path"]).resolve():
        raise ValueError("golden input does not match shared pre-MIDAS source")
    for key, receipt in receipts.items():
        if identity(receipt["annotations"]) != identity(expected_annotations):
            raise ValueError(f"annotation input differs from shared elaboration: {key}")
    generation = [command for command in archive["commands"]
                  if command.get("phase") == "target_elaboration" and command.get("exit_code") == 0]
    if len(generation) != 1:
        raise ValueError("expected one successful shared target elaboration")
    inputs_directory = Path(option(generation[0]["argv"], "--target-dir")).resolve()
    for key in ("candidate", "candidate_routes"):
        record = receipts[key]["input"]
        path = Path(record["path"]).resolve()
        expected = archive["artifacts"].get(path.name)
        if path.parent != inputs_directory or expected is None or identity(record) != identity(expected):
            raise ValueError(f"native input is not the hash-bound shared elaboration artifact: {key}")
    if identity(candidate["input"]) != identity(snapshot["input"]):
        raise ValueError("native route replay used different input")
    archived_jar = archive["provenance"]["execution_jars"].get(Path(golden["compiler_classpath"]["path"]).name)
    if archived_jar is None or identity(archived_jar) != identity(golden["compiler_classpath"]):
        raise ValueError("SFC compiler differs from the shared archived golden compiler")

    inventories = {}
    for key, receipt in receipts.items():
        inventory = receipt["artifacts"]
        entries = {}
        if isinstance(inventory, dict):
            for name, record in inventory.items():
                path = (directories[key] / name).resolve()
                if not path.is_relative_to(directories[key]):
                    raise ValueError("compiler artifact inventory escapes output directory")
                entries[str(path)] = identity(record)
        elif isinstance(inventory, list):
            for record in inventory:
                path = Path(record["path"]).resolve()
                if not path.is_relative_to(directories[key]) or str(path) in entries:
                    raise ValueError("duplicate or escaping compiler artifact inventory entry")
                entries[str(path)] = identity(record)
        else:
            raise ValueError("malformed compiler artifact inventory")
        if not entries:
            raise ValueError("empty compiler artifact inventory")
        inventories.update(entries)
    return dict(match=True, target=pair["name"], enabled_profile=expected_profile,
                shared_elaboration_manifest=str(archive_path),
                native_input=candidate["input"], sfc_input=golden["input"],
                shared_annotations=expected_annotations,
                candidate_compiler=candidate["compiler"], snapshot_compiler=snapshot["compiler"],
                snapshot_compiler_identical=identity(candidate["compiler"]) == identity(snapshot["compiler"]),
                snapshot_reuse_rule="Same enabled profile and hash-bound shared inputs, plus exact final sidecar/RTL and infer-stage annotation identity; different compiler binaries remain explicit."), inventories
