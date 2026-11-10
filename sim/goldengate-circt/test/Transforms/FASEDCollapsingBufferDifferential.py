#!/usr/bin/env python3
"""Scoped RTL differential check against an immutable selected SFC buffer cone.

This is not target parity or formal proof. Compare ready, every valid bit and
payload of valid entries. SFC dead-field elimination removed xaction.len; native
len transport is covered by the C++ helper test, not by this comparison. Both
designs receive identical arbitrary updates, including younger-entry rewrites.
"""
import argparse
import json
from pathlib import Path
import subprocess

from FASEDRankStateTrackerDifferential import closure, digest


FIELDS = {"id": 4, "len": 8, "isWrite": 1, "rowAddr": 26,
          "bankAddrOH": 8, "bankAddr": 3, "rankAddrOH": 4,
          "rankAddr": 2, "isReady": 1, "mayPRE": 1}


def sfc_field(name):
    return "xaction_" + name if name in {"id", "isWrite"} else name


def testbench():
    inputs = {"reset": 1, "targetFire": 1, "enqValid": 1,
              "programmableDepth": 4}
    inputs.update({"enqBits_" + name: width for name, width in FIELDS.items()})
    for i in range(8):
        inputs[f"updates_{i}_valid"] = 1
        inputs.update({f"updates_{i}_bits_{name}": width for name, width in FIELDS.items()})
    outputs = {"enqReady": 1}
    for i in range(8):
        outputs[f"entries_{i}_valid"] = 1
        outputs.update({f"entries_{i}_bits_{name}": width for name, width in FIELDS.items()})
    tb = ["module tb;", "logic clock=0;", "wire sfcClock=clock && targetFire;"]
    tb += [f"logic [{width-1}:0] {name}=0;" for name, width in inputs.items()]
    tb += [f"wire [{width-1}:0] native_{name},sfc_{name};" for name, width in outputs.items()]
    native = [".clock(clock)"] + [f".{name}({name})" for name in inputs]
    native += [f".{name}(native_{name})" for name in outputs]
    sfc = [".clock(sfcClock)", ".reset(reset)", ".io_programmableDepth(programmableDepth)",
           ".io_enq_valid(enqValid)", ".io_enq_ready(sfc_enqReady)"]
    for name in FIELDS:
        if name != "len":
            sfc += [f".io_enq_bits_{sfc_field(name)}(enqBits_{name})"]
    for i in range(8):
        sfc += [f".io_updates_{i}_valid(updates_{i}_valid)",
                f".io_entries_{i}_valid(sfc_entries_{i}_valid)"]
        for name in FIELDS:
            if name != "len":
                sfc += [f".io_updates_{i}_bits_{sfc_field(name)}(updates_{i}_bits_{name})",
                        f".io_entries_{i}_bits_{sfc_field(name)}(sfc_entries_{i}_bits_{name})"]
    tb += ["GGFASEDCollapsingBuffer native(" + ",".join(native) + ");",
           "CollapsingBuffer sfc(" + ",".join(sfc) + ");",
           "integer cycles=0,stalls=0,resets=0,kills=0,rewrites=0;",
           "task compare; begin",
           'if(native_enqReady!==sfc_enqReady)$fatal(1,"cycle=%0d ready differs",cycles);']
    for i in range(8):
        tb += [f'if(native_entries_{i}_valid!==sfc_entries_{i}_valid)$fatal(1,"cycle=%0d entry={i} validity differs",cycles);']
        for name in FIELDS:
            if name != "len":
                tb += [f'if(native_entries_{i}_valid && native_entries_{i}_bits_{name}!==sfc_entries_{i}_bits_{name})'
                       f'$fatal(1,"cycle=%0d entry={i} field={name} native=%0d sfc=%0d",cycles,native_entries_{i}_bits_{name},sfc_entries_{i}_bits_{name});']
    tb += ["end endtask", "task step; begin",
           "#2;compare();clock=1;#2;compare();clock=0;#2;cycles=cycles+1;",
           "if(!targetFire)stalls=stalls+1;if(reset)resets=resets+1;",
           "end endtask", "task defaults; begin"]
    for i in range(8):
        tb += [f"updates_{i}_valid=native_entries_{i}_valid;"]
        tb += [f"updates_{i}_bits_{name}=native_entries_{i}_bits_{name};" for name in FIELDS]
    tb += ["end endtask", "task randomPayload; begin"]
    for name, width in FIELDS.items():
        tb += [f"enqBits_{name}={width}'($urandom);"]
    tb += ["end endtask", "integer depth,mask,i;", "initial begin",
           # A reset edge initializes every valid bit. Invalid data is excluded.
           "reset=1;targetFire=1;programmableDepth=0;step();reset=0;",
           # Exhaust all runtime schedulerWindowSize values (three bits padded
           # to the four-bit CollapsingBuffer port), every update-valid pattern,
           # and arbitrary updates to younger payloads on every cycle.
           "for(depth=0;depth<8;depth=depth+1)begin",
           "programmableDepth=4'(depth);reset=1;targetFire=1;defaults();step();reset=0;",
           "enqValid=1;for(i=0;i<20;i=i+1)begin defaults();randomPayload();step();end",
           "for(mask=0;mask<256;mask=mask+1)begin defaults();randomPayload();",
           "enqValid=1'($urandom);targetFire=(mask%5!=0);reset=(mask%47==0);"]
    for i in range(8):
        tb += [f"updates_{i}_valid=1'(mask>>{i});"]
        for name, width in FIELDS.items():
            tb += [f"updates_{i}_bits_{name}={width}'($urandom);"]
    tb += ["step();end",
           "reset=0;targetFire=1;enqValid=0;",
           "for(i=0;i<20;i=i+1)begin defaults();updates_0_valid=0;step();end",
           "end",
           "for(i=0;i<30000;i=i+1)begin defaults();randomPayload();",
           "programmableDepth=4'($urandom_range(0,7));",
           "enqValid=1'($urandom);targetFire=($urandom_range(0,4)!=0);reset=($urandom_range(0,127)==0);"]
    for i in range(8):
        tb += [f"if($urandom_range(0,7)==0)begin updates_{i}_valid=0;kills=kills+1;end",
               f"if($urandom_range(0,3)==0)begin rewrites=rewrites+1;"]
        for name, width in FIELDS.items():
            tb += [f"updates_{i}_bits_{name}={width}'($urandom);"]
        tb += ["end"]
    tb += ["step();end",
           'if(stalls==0||resets==0||kills==0||rewrites==0)$fatal(1,"missing coverage");',
           '$display("PASS collapsing buffer differential cycles=%0d comparedFields=81 runtimeDepths=8 stalls=%0d resets=%0d kills=%0d youngerRewrites=%0d",cycles,stalls,resets,kills,rewrites);',
           "$finish;end", "endmodule"]
    return "\n".join(tb) + "\n"


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
    cone, module_names = closure(sfc.read_text(), "CollapsingBuffer")
    (d / "sfc-cone.sv").write_text(cone)
    (d / "tb.sv").write_text(testbench())
    commands = [
        [str(a.fixture.resolve()), str(d / "native.mlir")],
        [str(a.firtool.resolve()), str(d / "native.mlir"), "--format=mlir", "--verilog", "-o", str(d / "native.sv")],
        [str(a.verilator.resolve()), "--binary", "--timing", "-DSYNTHESIS", "--top-module", "tb", "-Wno-fatal", "-j", str(a.jobs),
         "--Mdir", str(d / "obj"), str(d / "native.sv"), str(d / "sfc-cone.sv"), str(d / "tb.sv")],
        [str(d / "obj/Vtb")],
    ]
    report = {"scope": "CollapsingBuffer dependency cone only; SFC clock gated by targetFire; all runtime depths0..7; valid payload comparisons exclude unreset invalid bits and SFC-eliminated len; not full-target parity or formal proof",
              "sfc": str(sfc.resolve()), "sfc_sha256": original, "modules": module_names,
              "commands": commands, "target": receipt["target"],
              "manifest_sha256": digest(manifest_path),
              "tools": {str(f.resolve()): digest(f) for f in [a.fixture, a.firtool, a.verilator]},
              "passed": False}
    try:
        for i, command in enumerate(commands):
            with (d / f"stage-{i}.log").open("w") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        if "PASS collapsing buffer differential" not in (d / "stage-3.log").read_text():
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
