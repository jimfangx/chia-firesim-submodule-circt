#!/usr/bin/env python3
# See LICENSE for license details.
"""Exercise constructor positional ABI against unchanged SFC target synthesis.

Requires AutoCounterSynthesisOracle.scala compiled on the unchanged MIDAS
classpath. Every case executes the actual native compiler and the actual Scala
AutoCounterTransform, preserving all event list positions in the comparison.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

BRIDGE = "firesim.lib.bridgeutils.BridgeIOAnnotation"
BOOKKEEPING = {"firrtl.options.TargetDirAnnotation",
               "midas.passes.AutoCounterSynthesisOracle$ConstructorHints"}


def annotation(module, index, label, clock="clock"):
    return dict(
        **{"class": "midas.InternalAutoCounterFirrtlAnnotation"},
        target=f"~Top|{module}>data{index}", clock=f"~Top|{module}>{clock}",
        reset=f"~Top|{module}>reset", label=label,
        description=f"ordering case {module} {label}", coverGenerated=False,
        opType={"class": "midas.targetutils.PerfCounterOps$" +
                ("Identity$" if index % 2 else "Accumulate$")})


def flat(labels):
    lines = ["FIRRTL version 1.1.0", "circuit Top :", "  module Top :",
             "    input clock : Clock", "    input other : Clock",
             "    input reset : UInt<1>"]
    annos = []
    for i, label in enumerate(labels):
        lines.append(f"    input data{i} : UInt<{8 + 8 * (i % 3)}>")
        annos.append(annotation("Top", i, label, "other" if i % 3 else "clock"))
    return "\n".join(lines) + "\n", annos


def nested():
    # Aa and BB have the same Java String hash. Definition/annotation order
    # differs from active top instance order, including repeated paths/clocks.
    lines = ["FIRRTL version 1.1.0", "circuit Top :"]
    annos = []
    for module in ["Aa", "BB"]:
        lines += [f"  module {module} :", "    input clock : Clock",
                  "    input reset : UInt<1>", "    input data0 : UInt<8>",
                  "    input data1 : UInt<16>"]
        for i, label in enumerate(["Aa", "BB"]):
            annos.append(annotation(module, i, label))
    lines += ["  module Top :", "    input clock : Clock", "    input other : Clock",
              "    input reset : UInt<1>", "    input data0 : UInt<8>",
              "    input data1 : UInt<16>"]
    for i, module in enumerate(["BB", "Aa", "Aa", "BB"]):
        inst = f"child{i}"
        lines += [f"    inst {inst} of {module}",
                  f"    {inst}.clock <= {'clock' if i % 2 else 'other'}",
                  f"    {inst}.reset <= reset", f"    {inst}.data0 <= data0",
                  f"    {inst}.data1 <= data1"]
    return "\n".join(lines) + "\n", annos


def canonical(annotations):
    # Registry order is not semantic. Array order inside any record, including
    # each constructor's eventMetadata, is retained exactly.
    return sorted(json.dumps(a, sort_keys=True) for a in annotations
                  if a["class"] not in BOOKKEEPING)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--oracle-classpath", required=True)
    parser.add_argument("--firtool", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cases = [(f"sources-{count}", flat([f"event{i}" for i in range(count)]))
             for count in [1, 4, 5, 40, 128]]
    # Full-hash collision nodes, with two different insertion orders.
    for labels in [["Aa", "BB", "AaAa", "BBBB", "AaBB", "BBAa"],
                   ["BB", "Aa", "BBAa", "AaBB", "BBBB", "AaAa"]]:
        cases.append((f"hash-collision-{len(cases)}", flat(labels)))
    cases.append(("nested-module-hash-collision", nested()))
    commands, receipts = [], []

    def run(command, path):
        command = list(map(str, command))
        commands.append(command)
        (root / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with path.open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    for name, (firrtl, annotations) in cases:
        case = root / name
        case.mkdir(parents=True, exist_ok=True)
        fir, anno = case / "input.fir", case / "input.json"
        fir.write_text(firrtl)
        anno.write_text(json.dumps(annotations, indent=2) + "\n")
        native, sfc = case / "native", case / "sfc"
        native.mkdir(exist_ok=True)
        run([args.compiler.resolve(), fir, "--annotation-file", anno,
             "--output-dir", native, "--synthesize-autocounter-csv"], case / "native.log")
        run(["java", "-cp", args.oracle_classpath, "midas.passes.AutoCounterSynthesisOracle",
             fir, anno, sfc], case / "sfc.log")
        na = json.loads((native / "post-autocounter-csv-all.json").read_text())
        sa = json.loads((sfc / "post-autocounter.json").read_text())
        assert canonical(na) == canonical(sa), f"{name}: exact annotation/constructor positions differ"
        nb = [a for a in na if a["class"] == BRIDGE]
        sb = [a for a in sa if a["class"] == BRIDGE]
        assert nb == sb, f"{name}: clock-domain constructor order differs"
        run([args.firtool.resolve(), sfc / "post-autocounter.fir",
             "--disable-all-randomization", "--disable-annotation-unknown",
             "-o", case / "sfc.sv"], case / "sfc-syntax.log")
        events = [e for bridge in nb for e in bridge["widgetConstructorKey"]["eventMetadata"]]
        receipts.append(dict(name=name, passed=True, widgets=len(nb),
                             exported_events=len(events), exact_order=True))
        print("PASS", name, len(events), "events", flush=True)
    files = [p for p in root.rglob("*") if p.is_file() and p != root / "result.json"]
    files += [args.compiler.resolve(), args.firtool.resolve(), Path(__file__).resolve()]
    result = dict(passed=True, scope="exact constructor event positions against unchanged SFC",
                  full_feature_acceptance=False, cases=receipts,
                  normalizations=["SFC TargetDir and supplemental serialization hints only",
                                  "annotation registry order; all constructor and event array positions retained"],
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files})
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
