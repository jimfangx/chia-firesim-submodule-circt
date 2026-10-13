#!/usr/bin/env python3
"""Replay the archived pre-MIDAS U250 inputs through the native compiler.

Run from the sourced FireSim environment. This is a standalone compiler test;
manager/metasim/bitstream verification remains owned by the external harness.
Failed targets retain their artifacts and partial comparisons, never a pass.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import time


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


BRIDGE_CLASSES = ("firesim.lib.bridgeutils.BridgeIOAnnotation",
                  "midas.passes.fame.FAMEChannelConnectionAnnotation")
MODEL_CLASSES = tuple("midas.passes.fame." + name for name in (
    "FAMEChannelConnectionAnnotation", "FAMEChannelFanoutAnnotation",
    "FAMEChannelPortsAnnotation", "FAMEHostClock", "FAMEHostReset",
    "FAMETransformAnnotation"))


def compare_boundary(golden, candidate, classes=BRIDGE_CLASSES):
    result = {"golden": str(golden), "candidate": str(candidate)}
    if not candidate.is_file():
        return dict(result, status="not reached")
    reference = json.loads(golden.read_text())
    actual = json.loads(candidate.read_text())
    # Compare full structured records, including constructors and optional
    # fields, ordered endpoints, channel clocks and target identities.
    def records(annotations, cls):
        return Counter(json.dumps(a, sort_keys=True) for a in annotations
                       if a.get("class") == cls)
    checks = {}
    for cls in classes:
        expected, observed = records(reference, cls), records(actual, cls)
        checks[cls] = {"golden_count": sum(expected.values()),
                       "candidate_count": sum(observed.values()),
                       "match": expected == observed,
                       "missing": list((expected - observed).elements()),
                       "extra": list((observed - expected).elements())}
    return dict(result, status="matched" if all(x["match"] for x in checks.values())
                else "mismatch", checks=checks)


def comparisons(golden, candidate):
    return [compare_boundary(golden / "post-bridge-extraction.json",
                             candidate / "post-bridge-extraction-all.json"),
            compare_boundary(golden / "post-infer-model-ports.json",
                             candidate / "post-infer-model-ports.json", MODEL_CLASSES)]


def write_report(output, report):
    (output / "matrix.json").write_text(json.dumps(report, indent=2) + "\n")
    text = ("| Recipe | SFC | CIRCT | Bridge/channel boundary | Model-port boundary |\n"
            "| --- | --- | --- | --- | --- |\n")
    for row in report["rows"]:
        text += (f"| {row['recipe']} | {row['sfc_status']} | {row['circt_compile_status']} "
                 f"| {row['comparisons'][0]['status']} | {row['comparisons'][1]['status']} |\n")
    text += ("\nA matched annotation boundary is a partial comparison. Additional native SRAM "
             "models can produce model-port differences; these require a separate equivalence "
             "check. Compile success does not establish RTL behavior, platform, or ABI parity.\n")
    (output / "matrix.md").write_text(text)


def replay(compiler, output, item):
    name, target = item
    directory = output / name
    directory.mkdir(parents=True, exist_ok=False)
    inputs = Path(target["input_dir"])
    firrtl, = inputs.glob("*.sfc.fir")
    annotations, = inputs.glob("*.anno.json")
    golden = Path(target["golden_dir"])
    candidate = directory / "candidate"
    command = [str(compiler), str(firrtl), "--annotation-file", str(annotations),
               "--output-dir", str(candidate), "--compile-baseline",
               "--platform-config", target["effective_platform_config"],
               "--output-filename-base", "FireSim-generated"]
    start = time.monotonic()
    with (directory / "stdout.log").open("w") as out, (directory / "stderr.log").open("w") as err:
        process = subprocess.run(command, stdout=out, stderr=err)
    stderr = (directory / "stderr.log").read_text()
    status = ("compiled" if process.returncode == 0 else
              "unsupported platform" if "unsupported Golden Gate platform configuration" in stderr
              else "failed")
    row = {"recipe": name, "sfc_status": target["status"],
           "effective_platform_config": target["effective_platform_config"],
           "firrtl": str(firrtl), "firrtl_sha256": digest(firrtl),
           "annotations": str(annotations), "annotations_sha256": digest(annotations),
           "golden_dir": str(golden), "candidate_dir": str(candidate),
           "command": command, "circt_compile_status": status,
           "returncode": process.returncode, "seconds": round(time.monotonic() - start, 2),
           "stdout": str(directory / "stdout.log"), "stderr": str(directory / "stderr.log"),
           "unsupported_feature": stderr[-4000:].strip() if process.returncode else None,
           "comparisons": comparisons(golden, candidate),
           # Constructor/channel parity alone does not prove final RTL behavior.
           "comparison_status": "partial; behavioral/platform/ABI comparison pending"}
    (directory / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    print(name, status, row["comparisons"][0]["status"], flush=True)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    manifest = json.loads(args.manifest.read_text())
    targets = manifest["targets"]
    if len(targets) != 13 or any(v["status"] != "passed" for v in targets.values()):
        parser.error("expected all thirteen completed SFC target fixtures")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    compiler_hash = digest(args.compiler)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(lambda item: replay(args.compiler.resolve(),
                                                args.output_dir.resolve(), item),
                             targets.items()))
    if digest(args.compiler) != compiler_hash:
        raise RuntimeError("compiler changed during replay; matrix is not one compiler snapshot")
    report = {"manifest": str(args.manifest.resolve()),
              "manifest_sha256": digest(args.manifest),
              "compiler": str(args.compiler.resolve()),
              "compiler_sha256": compiler_hash, "rows": rows}
    write_report(args.output_dir, report)
    return int(any(r["returncode"] or any(c["status"] != "matched" for c in r["comparisons"])
                   for r in rows))


if __name__ == "__main__":
    raise SystemExit(main())
