#!/usr/bin/env python3
"""Scoped differential check of native DDR3 rank RTL against an immutable SFC cone.

This is not full-target equivalence or a Jasper proof. The SFC file is read only;
its RankStateTracker dependency closure is copied without rewriting any module.
Assertions in both designs are disabled for illegal-command stress. Compare all
55 output fields on every cycle, including bank rows after explicit initialization.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def closure(text, top):
    modules = {
        m.group(1): m.group(0) for m in re.finditer(
            r"(?ms)^module\s+(\w+)\b.*?^endmodule\b[^\n]*(?:\n|$)", text)
    }
    if top not in modules:
        raise ValueError(f"missing {top}")
    selected = {}

    def visit(name):
        if name in selected:
            return
        selected[name] = modules[name]
        # SFC Chisel module instantiations are unparameterized. Retain the exact
        # bodies and all known instantiated children, rejecting missing modules
        # through the subsequent Verilator compilation.
        for child in modules:
            if child != name and re.search(r"\b" + re.escape(child) + r"\s+\w+\s*\(", modules[name]):
                visit(child)

    visit(top)
    return "\n".join(selected.values()), list(selected)


def testbench():
    timings = {n: (14 if n == "tREFI" else 10 if n == "tRFC" else 7)
               for n in "tAL tCAS tCMD tCWD tCCD tFAW tRAS tREFI tRC tRCD tRFC tRRD tRP tRTP tRTRS tWR tWTR".split()}
    inputs = dict(reset=1, targetFire=1, selectedCmd=3, cmdUsesThisRank=1,
                  cmdBankOH=8, autoPRE=1, cmdRow=26, tCycle=7)
    inputs.update({"timings_" + n: w for n, w in timings.items()})
    outputs = {n: 1 for n in "canCASW canCASR canPRE canACT canREF wantREF state".split()}
    outputs.update({f"banks_{i}_{n}": 26 if n == "openRow" else 1
                    for i in range(8) for n in "canCASW canCASR canPRE canACT state openRow".split()})
    declarations = ["module tb;", "logic clock = 0;", "wire sfcClock = clock && targetFire;"]
    declarations += [f"logic [{w-1}:0] {n} = 0;" for n, w in inputs.items()]
    declarations += [f"wire [{w-1}:0] native_{n}, sfc_{n};" for n, w in outputs.items()]
    native = [".clock(clock)"] + [f".{n}({n})" for n in inputs]
    native += [f".{n}(native_{n})" for n in outputs]
    sfc = [".clock(sfcClock)", ".reset(reset)"]
    sfc += [f".io_{n}({n})" for n in inputs if n not in {"reset", "targetFire", "timings_tCMD"}]
    sfc += [f".io_rank_{n}(sfc_{n})" for n in outputs]
    declarations += ["GGFASEDRankStateTracker native(" + ",".join(native) + ");",
                     "RankStateTracker sfc(" + ",".join(sfc) + ");"]
    comparisons = [f'if (native_{n} !== sfc_{n}) $fatal(1, "cycle=%0d field={n} native=%0d sfc=%0d", cycles, native_{n}, sfc_{n});'
                   for n in outputs]
    declarations += ["integer cycles=0;", "task step; begin", "#2; clock=1; #2;"]
    declarations += comparisons
    declarations += ["clock=0; #2; cycles=cycles+1; end endtask",
                     "integer i; integer remoteCAS=0,stalls=0,refreshes=0,windows=0;",
                     "initial begin"]
    # Initialize unreset row registers through an ACT under reset. Reset controls
    # the other bank registers, as in the Scala oracle; all rows become defined.
    declarations += ["reset=1;targetFire=1;selectedCmd=1;cmdUsesThisRank=1;cmdBankOH=255;step();",
                     "reset=0;selectedCmd=0;cmdUsesThisRank=0;cmdBankOH=0;"]
    defaults = [0, 14, 1, 10, 4, 25, 33, 7800, 47, 14, 160, 8, 14, 8, 2, 15, 8]
    declarations += [f"timings_{n}={v};" for n, v in zip(timings, defaults)]
    # Explicit FAW queue full, simultaneous full enqueue/dequeue, absolute-time
    # wrap, zero expiry, remote CAS, refresh expiry/REF collision, and stalls.
    declarations += [
        "timings_tRRD=1;timings_tFAW=5;cmdUsesThisRank=1;selectedCmd=1;",
        "for(i=0;i<4;i=i+1) begin tCycle=i;cmdBankOH=8'(1<<i);step();end",
        "tCycle=5;cmdBankOH=16;step();windows=windows+1;",
        "selectedCmd=0;for(i=6;i<150;i=i+1)begin tCycle=7'(i);step();end",
        "reset=1;step();reset=0;timings_tFAW=0;tCycle=127;selectedCmd=1;step();",
        "timings_tFAW=50;for(i=0;i<3;i=i+1)begin tCycle=7'(i);step();end",
        'if(native_canACT!==0)$fatal(1,"zero-expiry enqueue must remain queued (flow=false)");',
        "selectedCmd=0;tCycle=127;step();",
        'if(native_canACT!==1)$fatal(1,"expiry equality must dequeue oldest ACT");',
        "reset=1;selectedCmd=0;step();reset=0;timings_tREFI=1;timings_tRFC=2;",
        "selectedCmd=0;step();selectedCmd=5;step();",
        'if(native_wantREF!==1)$fatal(1,"refresh expiry must override REF clear");',
        "step();",
        'if(native_state!==0)$fatal(1,"refresh exit must override repeated REF state update");',
        "targetFire=0;reset=1;step();targetFire=1;step();reset=0;",
        "selectedCmd=0;timings_tREFI=0;for(i=0;i<16400;i=i+1)begin tCycle=7'(i);step();end",
        'if(native_wantREF!==0)$fatal(1,"zero tREFI must disable refresh across counter wrap");',
        "reset=1;step();reset=0;timings_tREFI=16383;",
        "for(i=0;i<16384;i=i+1)begin tCycle=7'(i);step();end",
        'if(native_wantREF!==1)$fatal(1,"full-width tREFI expiry missing");',
        "for(i=0;i<60000;i=i+1)begin",
        "targetFire=($urandom_range(0,4)!=0);reset=($urandom_range(0,127)==0);",
        "selectedCmd=3'($urandom_range(0,7));cmdUsesThisRank=1'($urandom);",
        "cmdBankOH=8'($urandom);autoPRE=1'($urandom);cmdRow=26'($urandom);",
        "tCycle=7'(i);"]
    for n, w in timings.items():
        # Full-width adversarial timings, plus frequently reachable refreshes.
        declarations += [f"timings_{n}={w}'($urandom);" if n != "tREFI" else
                         "timings_tREFI=14'($urandom_range(0,31));"]
    declarations += ["if(!cmdUsesThisRank && (selectedCmd==3 || selectedCmd==4))remoteCAS=remoteCAS+1;",
                     "if(!targetFire)stalls=stalls+1;",
                     "if(targetFire && cmdUsesThisRank && selectedCmd==5)refreshes=refreshes+1;",
                     "step();end",
                     'if(remoteCAS==0 || stalls==0 || refreshes==0 || windows==0)$fatal(1,"missing coverage");',
                     '$display("PASS rank differential cycles=%0d outputs=55 remoteCAS=%0d stalls=%0d refreshCommands=%0d FAWDirected=%0d",cycles,remoteCAS,stalls,refreshes,windows);',
                     "$finish; end", "endmodule"]
    return "\n".join(declarations) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--fixture", required=True, type=Path)
    p.add_argument("--firtool", required=True, type=Path)
    p.add_argument("--verilator", required=True, type=Path)
    p.add_argument("--golden-receipt", required=True, type=Path)
    p.add_argument("--output-dir", required=True, type=Path)
    p.add_argument("--jobs", default=1, type=int)
    a = p.parse_args()
    a.output_dir.mkdir(parents=True, exist_ok=False)
    d = a.output_dir.resolve()
    receipt = json.loads(a.golden_receipt.read_text())
    manifest_path = Path(receipt["manifest"]["path"])
    if receipt["status"] != "complete" or digest(manifest_path) != receipt["manifest"]["sha256"]:
        raise ValueError("incomplete golden or mismatched manifest identity")
    manifest = json.loads(manifest_path.read_text())
    if manifest["status"] != "complete" or manifest["target"] != receipt["target"]:
        raise ValueError("incomplete/mismatched golden manifest")
    sfc = manifest_path.parent / "sfc/FireSim-generated.sv"
    original = digest(sfc)
    if original != manifest["sfc_artifacts"][sfc.name]["sha256"]:
        raise ValueError("golden RTL does not match its immutable manifest")
    cone, names = closure(sfc.read_text(), "RankStateTracker")
    (d / "sfc-cone.sv").write_text(cone)
    (d / "tb.sv").write_text(testbench())
    commands = [
        [str(a.fixture.resolve()), str(d / "native.mlir")],
        [str(a.firtool.resolve()), str(d / "native.mlir"), "--format=mlir", "--verilog", "-o", str(d / "native.sv")],
        [str(a.verilator.resolve()), "--binary", "--timing", "-DSYNTHESIS", "--top-module", "tb", "-Wno-fatal", "-j", str(a.jobs),
         "--Mdir", str(d / "obj"), str(d / "native.sv"), str(d / "sfc-cone.sv"), str(d / "tb.sv")],
        [str(d / "obj/Vtb")],
    ]
    report = {"scope": "RankStateTracker dependency cone only; arbitrary commands/timings including illegal commands; design assertions disabled in both; targetFire gates SFC clock; rows explicitly initialized; not full-target parity or formal proof",
              "sfc": str(sfc.resolve()), "sfc_sha256": original, "modules": names, "commands": commands,
              "target": receipt["target"], "manifest_sha256": digest(manifest_path),
              "tools": {str(f.resolve()): digest(f) for f in [a.fixture, a.firtool, a.verilator]},
              "passed": False}
    try:
        for i, command in enumerate(commands):
            with (d / f"stage-{i}.log").open("w") as f:
                subprocess.run(command, stdout=f, stderr=subprocess.STDOUT, check=True, timeout=180)
        if "PASS rank differential" not in (d / "stage-3.log").read_text():
            raise ValueError("missing target PASS marker")
        if digest(sfc) != original:
            raise ValueError("SFC source changed during comparison")
        report["passed"] = True
        report["evidence"] = (d / "stage-3.log").read_text()
    finally:
        report["artifacts"] = {str(f.relative_to(d)): digest(f) for f in d.iterdir() if f.is_file()}
        (d / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["evidence"])


if __name__ == "__main__":
    main()
