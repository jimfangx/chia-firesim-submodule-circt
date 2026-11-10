#!/usr/bin/env python3
"""Supporting same-input normal NIC full F2 compiler-product comparison.

Run only after both complete compilers finish. Archive/finalize private products
for complete RTL and real driver-header syntax, retaining original outputs.
Compare clocked bridge constructors, channels, model annotations, driver ABI,
interfaces, streams and XDC through the existing four-target comparison helpers.
No packet behavior, LEC/SEC, timing, Vivado, Linux or feature acceptance follows.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ProductionPrintF2Compare as PRODUCTS

SEM, BASE = PRODUCTS.SEM, PRODUCTS.BASE
NIC = "firechip.goldengateimplementations.SimpleNICBridgeModule"
PRINT = "midas.widgets.PrintBridgeModule"


def compare(sfc, circt, expected_nics):
    helper = SEM.load_module("production_nic_driver", HERE / "NICDriverHeaderCompare.py")
    rtl = [(path / (BASE + ".sv")).read_text() for path in (sfc, circt)]
    details = {
        "bridge_extraction": SEM.boundary(sfc / "post-bridge-extraction.json", circt / "post-bridge-extraction-all.json", SEM.BRIDGES),
        # Native enriches original bridge clockInfo before SFC. Compare all
        # clock fields strictly after the common UpdateBridgeClockInfo stage.
        "clocked_bridges": SEM.boundary(sfc / "post-wrap-top-all.json", circt / "post-wrap-top-all.json", SEM.BRIDGES[:1]),
        "debug_channels": SEM.boundary(sfc / "post-debug-synthesis.json", circt / "post-debug-synthesis-all.json", SEM.BRIDGES[1:]),
        "infer_model_ports": SEM.boundary(sfc / "post-infer-model-ports.json", circt / "post-infer-model-ports.json", SEM.MODELS),
        "interfaces": {name: SEM.compare_values(*(SEM.ports(text, name) for text in rtl)) for name in ("F1Shim", "FPGATop")},
        "driver_abi": SEM.compare_values(*(SEM.driver_contract((path / (BASE + ".const.h")).read_text(), helper) for path in (sfc, circt))),
    }
    # Assert the intended feature is present in both complete outputs; a
    # baseline-only compiler product must not silently satisfy this fixture.
    nics = []
    for path in (sfc, circt):
        bridges = [row for row in SEM.annotations(path / "post-wrap-top-all.json") if row.get("class") == SEM.BRIDGES[0]]
        selected = [row for row in bridges if row.get("widgetClass") == NIC]
        if len(selected) != expected_nics or any(row.get("widgetClass") == PRINT for row in bridges):
            raise ValueError("expected normal NIC count and no enabled Print constructors: " + str(path))
        if any(row.get("widgetConstructorKey") is not None for row in selected):
            raise ValueError("normal NIC expects constructor-free original BridgeIO")
        nics.append(selected)
    details["enabled_nics"] = SEM.compare_values(*nics)
    for extension in ("defines.vh", "synthesis.xdc"):
        details[extension] = SEM.compare_values(*((path / (BASE + "." + extension)).read_text() for path in (sfc, circt)))
    details["implementation.xdc"] = SEM.compare_implementation_clocks(*(
        SEM.implementation_clocks((path / (BASE + ".implementation.xdc")).read_text(), text)
        for path, text in zip((sfc, circt), rtl)))
    for extension, normalize in (("peer2peer.const.yaml", SEM.stream_metadata), ("partition.const.h", SEM.partition_contract)):
        details[extension] = SEM.compare_values(*(normalize((path / (BASE + "." + extension)).read_text()) for path in (sfc, circt)))
    return details


def matches(value):
    if isinstance(value, dict):
        return value.get("match", True) and all(matches(item) for key, item in value.items() if key not in ("golden", "candidate"))
    return all(matches(item) for item in value) if isinstance(value, list) else True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("repo", "sfc", "circt", "input-fir", "input-annotations", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--expected-nics", type=int, choices=(1, 2), default=1)
    parser.add_argument("--compile-receipt", type=Path, action="append", default=[],
                        help="archive binding for caller's actual completed same-input compiler command/provenance receipt")
    args = parser.parse_args()
    repo, output = args.repo.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    originals = [SEM.file_record(path) for path in (args.input_fir, args.input_annotations)]
    dependencies = [Path(__file__), HERE / "ProductionPrintF2Compare.py", HERE / "CompareF2Semantics.py", HERE / "NICDriverHeaderCompare.py"]
    originals += [SEM.file_record(path) for path in dependencies + args.compile_receipt]
    result = {"passed": False, "full_feature_acceptance": False,
              "scope": "Supporting matched normal NIC full F2 platform syntax and static annotations/driver/interface/collateral comparison. No behavioral equivalence, packet execution, Jasper LEC/SEC, Vivado, Linux or feature acceptance.",
              "argv": sys.argv, "cwd": str(Path.cwd()), "input_pair": originals[:2],
              "compile_receipts": [SEM.file_record(path) for path in args.compile_receipt], "details": {}}
    try:
        gm, sources = PRODUCTS.stage(args.sfc.resolve(), output / "sfc", repo)
        originals += sources
        cm, sources = PRODUCTS.stage(args.circt.resolve(), output / "circt", repo)
        originals += sources
        result["details"] = compare(output / "sfc", output / "circt", args.expected_nics)
        result["paired_syntax"] = SEM.paired_syntax(repo, output / "sfc", output / "circt", gm, cm, output / "paired-syntax")
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
