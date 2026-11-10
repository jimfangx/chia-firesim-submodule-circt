#!/usr/bin/env python3
"""Compare complete SFC/native F2 simulator products for an enabled Print case.

Both compilers must already have consumed the same private pre-MIDAS input and
annotations. This supporting check archives their emitted products, appends the
actual listed blackbox bodies, compares retained annotations/platform contracts,
executes actual Print constructor metadata interception, and checks complete RTL
and real driver-header syntax. It does not prove RTL equivalence, Jasper LEC/SEC,
Vivado constraints, Linux operation, CSV AutoCounter integration or feature
acceptance. It never writes source compiler outputs or immutable baseline goldens.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys


HERE = Path(__file__).resolve().parent
BASE = "FireSim-generated"
CONSTRUCTORS = re.compile(r'#ifdef (GET_(?:CORE|BRIDGE|MANAGED_STREAM)_CONSTRUCTOR)\n'
                          r'(.*?)#endif // \1', re.S)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SEM = load("production_print_f2_semantics", HERE / "CompareF2Semantics.py")


def print_constructor(block):
    return "new synthesized_prints_t(" in block or "goldengate_print::make_print_bridge_" in block


def filtered_header(text, print_only):
    """Keep all declarations/ABI checks; select actual constructor phases only."""
    return CONSTRUCTORS.sub(lambda match: match[0] if print_constructor(match[2]) == print_only else "", text)


def stage(source, destination, repo):
    destination.mkdir()
    names = [BASE + suffix for suffix in (".const.h", ".const.vh", ".defines.vh",
             ".synthesis.xdc", ".implementation.xdc", ".peer2peer.const.yaml", ".partition.const.h")]
    names += [path.name for path in source.glob("post-*.json")]
    originals = []
    for name in names:
        original = source / name
        if not original.is_file() or original.is_symlink():
            raise ValueError("missing/symlink compiler product: " + str(original))
        shutil.copy2(original, destination / name)
        originals.append({"path": str(original.resolve()), "sha256": SEM.digest(original), "bytes": original.stat().st_size})
    rtl = source / (BASE + ".sv")
    raw = source / (BASE + ".raw.sv")
    if raw.exists():
        raise ValueError("expected unfinalized compiler products; preserve prior finalized receipts separately")
    shutil.copy2(rtl, destination / raw.name)
    shutil.copy2(rtl, destination / rtl.name)
    originals.append(SEM.file_record(rtl))
    resources = []
    listing = source / "firrtl_black_box_resource_files.f"
    if listing.exists():
        originals.append(SEM.file_record(listing))
        archived = destination / "resources"
        archived.mkdir()
        with (destination / rtl.name).open("ab") as output:
            for index, line in enumerate(listing.read_text().splitlines()):
                if not line.strip():
                    continue
                path = Path(line.strip())
                path = (path if path.is_absolute() else repo / "sims/firesim/sim" / path).resolve()
                if path in {rtl.resolve(), raw.resolve()}:
                    raise ValueError("recursive simulator blackbox file list")
                before = SEM.file_record(path)
                saved = archived / (str(index) + "-" + path.name)
                shutil.copy2(path, saved)
                output.write(saved.read_bytes() + b"\n")
                if SEM.file_record(path) != before or SEM.digest(saved) != before["sha256"]:
                    raise ValueError("blackbox body changed during archive")
                resources.append({"source": before, "archived": SEM.file_record(saved)})
                originals.append(before)
    return {"sfc_resources": resources, "resources": resources}, originals


def print_abi(repo, sfc, circt, directory):
    directory.mkdir()
    sys.path.insert(0, str(HERE.parent / "Transforms"))
    interception = load("production_print_actual_constructor", HERE.parent / "Transforms/InstrumentationHeaderCompare.py")
    # Reuse the actual unchanged Print declaration and constructor-body
    # interception. Other constructors are filtered out, while the complete
    # original headers are compiled separately by the paired syntax check.
    driver = interception.DRIVER.replace('#define GET_MANAGED_STREAM_CONSTRUCTOR', '')
    traces, commands, dependencies = [], [], []
    for kind, output in (("sfc", sfc), ("circt", circt)):
        original = output / (BASE + ".const.h")
        text = original.read_text()
        selected = filtered_header(text, True)
        if not any(print_constructor(match[2]) for match in CONSTRUCTORS.finditer(text)):
            raise ValueError("enabled output has no actual Print constructor: " + kind)
        header = directory / (kind + "-print.h")
        header.write_text(selected)
        source = directory / (kind + ".cpp")
        source.write_text(driver.replace('@HEADERS@', '#include ' + json.dumps(str(header)) + '\n'))
        binary = directory / kind
        include = repo / "sims/firesim/sim/midas/src/main/cc"
        compile_command = [repo / ".conda-env/bin/c++", "-std=c++20", "-Wall", "-Werror",
                           "-I", include, "-I", repo / "generators/firechip/bridgestubs/src/main/cc",
                           "-I", repo / ".conda-env/riscv-tools/include", source, "-o", binary]
        built = SEM.logged_command(compile_command, repo, directory / (kind + "-build"), 180)
        commands.append(built)
        if not built["passed"]:
            return {"match": False, "commands": commands, "scope": "Actual Print constructor compilation failed"}
        executed = SEM.logged_command([binary], repo, directory / (kind + "-run"), 30)
        commands.append(executed)
        traces.append(Path(executed["stdout"]).read_text())
        dependencies += [SEM.file_record(path) for path in (header, source, binary)]
    lines = traces[0].splitlines()
    valid = all(command["passed"] for command in commands) and traces[0] == traces[1] and any(line.startswith("print ") for line in lines)
    return {"match": valid, "semantic_rows": len(lines), "commands": commands, "dependencies": dependencies,
            "golden": traces[0], "candidate": traces[1],
            "scope": "Executed actual unchanged Print declaration with intercepted constructor body: MMIO, stream index/depth, clock, format bytes and ordered argument widths; no MMIO/stream/runtime execution"}


def compare(repo, sfc, circt, output):
    helper = SEM.load_module("production_print_base_driver", HERE / "NICDriverHeaderCompare.py")
    rtl = [(path / (BASE + ".sv")).read_text() for path in (sfc, circt)]
    details = {
        "source_bridges": SEM.boundary(sfc / "post-bridge-extraction.json", circt / "post-bridge-extraction-all.json", SEM.BRIDGES),
        # The native pipeline annotates original bridge clocks before debug
        # synthesis; SFC's UpdateBridgeClockInfo follows that checkpoint. Keep
        # channels at the actual synthesis boundary, then compare every bridge
        # field (including clockInfo) at the common post-WrapTop boundary.
        "enabled_channels": SEM.boundary(sfc / "post-debug-synthesis.json", circt / "post-debug-synthesis-all.json", SEM.BRIDGES[1:]),
        "enabled_bridges_with_clocks": SEM.boundary(sfc / "post-wrap-top-all.json", circt / "post-wrap-top-all.json", SEM.BRIDGES[:1]),
        "infer_model_ports": SEM.boundary(sfc / "post-infer-model-ports.json", circt / "post-infer-model-ports.json", SEM.MODELS),
        "interfaces": {name: SEM.compare_values(*(SEM.ports(text, name) for text in rtl)) for name in ("F1Shim", "FPGATop")},
        "other_driver_abi": SEM.compare_values(*(SEM.driver_contract(filtered_header((path / (BASE + ".const.h")).read_text(), False), helper)
                                                    for path in (sfc, circt))),
    }
    enabled = SEM.annotations(circt / "post-debug-synthesis-all.json")
    prints = [row for row in enabled if row.get("class") == SEM.BRIDGES[0] and row.get("widgetClass") == "midas.widgets.PrintBridgeModule"]
    if not prints:
        raise ValueError("Print feature is disabled in candidate annotations")
    details["print_constructors"] = {"match": True, "count": len(prints), "constructors": prints}
    details["print_driver_abi"] = print_abi(repo, sfc, circt, output / "actual-print-abi")
    for extension in ("defines.vh", "synthesis.xdc"):
        details[extension] = SEM.compare_values(*((path / (BASE + "." + extension)).read_text() for path in (sfc, circt)))
    details["implementation.xdc"] = SEM.compare_implementation_clocks(*(
        SEM.implementation_clocks((path / (BASE + ".implementation.xdc")).read_text(), text)
        for path, text in zip((sfc, circt), rtl)))
    for extension, normalize in (("peer2peer.const.yaml", SEM.stream_metadata), ("partition.const.h", SEM.partition_contract)):
        details[extension] = SEM.compare_values(*(normalize((path / (BASE + "." + extension)).read_text()) for path in (sfc, circt)))
    return details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("repo", "sfc", "circt", "input-fir", "input-annotations", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    originals = [SEM.file_record(path) for path in (args.input_fir, args.input_annotations, Path(__file__))]
    result = {"passed": False, "full_feature_acceptance": False,
              "scope": "Supporting matched enabled-Print full F2 platform syntax and static annotation/driver/interface/collateral comparison. No behavioral equivalence, Jasper LEC/SEC, Vivado, Linux or CSV AutoCounter acceptance.",
              "input_pair": originals[:2], "details": {}}
    try:
        gm, sources = stage(args.sfc.resolve(), output / "sfc", repo)
        originals += sources
        cm, sources = stage(args.circt.resolve(), output / "circt", repo)
        originals += sources
        result["details"] = compare(repo, output / "sfc", output / "circt", output)
        result["paired_syntax"] = SEM.paired_syntax(repo, output / "sfc", output / "circt", gm, cm, output / "paired-syntax")
        def matches(value):
            if isinstance(value, dict):
                return value.get("match", True) and all(matches(item) for key, item in value.items() if key not in ("golden", "candidate", "constructors"))
            return all(matches(item) for item in value) if isinstance(value, list) else True
        result["static_semantics"] = bool(matches(result["details"]))
        result["syntax"] = result["paired_syntax"]["passed"]
        result["passed"] = result["static_semantics"] and result["syntax"]
    except (ValueError, OSError, AssertionError, KeyError) as error:
        result["error"] = type(error).__name__ + ": " + str(error)
    finally:
        result["originals"] = originals
        result["originals_unchanged"] = all(SEM.digest(Path(record["path"])) == record["sha256"] for record in originals)
        result["passed"] &= result["originals_unchanged"]
        (output / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: result.get(key) for key in ("passed", "static_semantics", "syntax", "error", "scope")}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
