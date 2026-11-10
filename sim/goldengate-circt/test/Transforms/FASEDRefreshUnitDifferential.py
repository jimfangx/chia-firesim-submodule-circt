#!/usr/bin/env python3
"""Compare the native refresh arbiter with exact immutable selected SFC cones.

Scoped component simulation, not full-target equivalence or Jasper proof. Validate
all three FRFCFS target receipts. Identical literal cones share one simulation;
each receipt is independently checked and bound to that cone's full result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


TARGETS = {"megaboomv3", "shuttle", "saturn_shuttle"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_golden(path):
    receipt = json.loads(path.read_text())
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
    matches = re.findall(r"(?ms)^module\s+RefreshUnit\b.*?^endmodule\b[^\n]*(?:\n|$)", sfc.read_text())
    if len(matches) != 1:
        raise ValueError("expected exactly one literal SFC RefreshUnit module")
    cone = matches[0]
    return {"target": receipt["target"], "receipt": str(path.resolve()),
            "receipt_sha256": digest(path), "manifest": str(manifest_path.resolve()),
            "manifest_sha256": digest(manifest_path), "sfc": str(sfc.resolve()),
            "sfc_sha256": original, "cone_sha256": hashlib.sha256(cone.encode()).hexdigest(),
            "passed": False}, cone


def testbench():
    inputs = {n: 4 for n in ["wantREF", "canREF", "canPRE", "ranksInUse"]}
    inputs.update({f"bankCanPRE_{i}": 8 for i in range(4)})
    outputs = dict(suggestREF=1, refRankAddr=2, suggestPRE=1, preRankAddr=2, preBankAddr=3)
    text = ["module tb;"]
    text += [f"logic [{w-1}:0] {n}=0;" for n, w in inputs.items()]
    text += [f"wire [{w-1}:0] native_{n}, sfc_{n};" for n, w in outputs.items()]
    native = [f".{n}({n})" for n in inputs]
    native += [f".{n}(native_{n})" for n in outputs]
    sfc = [".io_ranksInUse(ranksInUse)"]
    for i in range(4):
        sfc += [f".io_rankStati_{i}_{n}({n}[{i}])" for n in ["wantREF", "canREF", "canPRE"]]
        sfc += [f".io_rankStati_{i}_banks_{j}_canPRE(bankCanPRE_{i}[{j}])" for j in range(8)]
    sfc += [f".io_{n}(sfc_{n})" for n in outputs]
    text += ["GGFASEDRefreshUnit native(" + ",".join(native) + ");",
             "RefreshUnit sfc(" + ",".join(sfc) + ");",
             "integer cases=0, dualProposals=0, noPreProposals=0, invalidBankNonDefault=0;",
             "integer rng=32'h52454634;",
             "function automatic integer random32; begin",
             "rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);random32=rng;end endfunction",
             "task randomBanks; begin",
             "bankCanPRE_0=8'(random32());bankCanPRE_1=8'(random32());",
             "bankCanPRE_2=8'(random32());bankCanPRE_3=8'(random32());end endtask",
             "task sample; begin #1;"]
    text += [f'if(native_{n} !== sfc_{n}) $fatal(1,"case=%0d field={n} native=%0d sfc=%0d",cases,native_{n},sfc_{n});'
             for n in outputs]
    text += ["if(native_suggestREF && native_suggestPRE)dualProposals=dualProposals+1;",
             "if(!native_suggestPRE)begin noPreProposals=noPreProposals+1;",
             "if(native_preBankAddr!=7)invalidBankNonDefault=invalidBankNonDefault+1;end",
             "cases=cases+1;end endtask",
             "integer want,refMask,preMask,useMask,rank,bank,i;",
             "initial begin",
             # Exhaust the independent four rank masks; bank masks use a fixed
             # xorshift seed, including arbitrary multi-bank requests.
             "for(want=0;want<16;want=want+1)for(refMask=0;refMask<16;refMask=refMask+1)",
             "for(preMask=0;preMask<16;preMask=preMask+1)for(useMask=0;useMask<16;useMask=useMask+1)begin",
             "wantREF=4'(want);canREF=4'(refMask);canPRE=4'(preMask);ranksInUse=4'(useMask);randomBanks();sample();end",
             # Every bank mask and ranksInUse mask, independently at each rank,
             # with valid proposals and a deliberately invalid rank mux output.
             "for(rank=0;rank<4;rank=rank+1)for(bank=0;bank<256;bank=bank+1)",
             "for(useMask=0;useMask<16;useMask=useMask+1)begin",
             "bankCanPRE_0=0;bankCanPRE_1=0;bankCanPRE_2=0;bankCanPRE_3=0;",
             "case(rank)0:bankCanPRE_0=8'(bank);1:bankCanPRE_1=8'(bank);2:bankCanPRE_2=8'(bank);3:bankCanPRE_3=8'(bank);endcase",
             "wantREF=15;canREF=15;canPRE=15;ranksInUse=4'(useMask);sample();wantREF=0;sample();end",
             "for(i=0;i<20000;i=i+1)begin",
             "wantREF=4'(random32());canREF=4'(random32());canPRE=4'(random32());ranksInUse=4'(random32());",
             "randomBanks();sample();end",
             'if(cases!=118304 || dualProposals==0 || noPreProposals==0 || invalidBankNonDefault==0)$fatal(1,"missing coverage");',
             '$display("PASS refresh differential cases=%0d outputs=5 dualProposals=%0d invalidPRE=%0d invalidBankNonDefault=%0d",cases,dualProposals,noPreProposals,invalidBankNonDefault);',
             "$finish;end", "endmodule"]
    return "\n".join(text) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--fixture", required=True, type=Path)
    p.add_argument("--firtool", required=True, type=Path)
    p.add_argument("--verilator", required=True, type=Path)
    p.add_argument("--golden-receipt", required=True, action="append", type=Path)
    p.add_argument("--output-dir", required=True, type=Path)
    p.add_argument("--jobs", default=1, type=int)
    a = p.parse_args()
    if a.jobs < 1:
        raise ValueError("jobs must be positive")
    goldens = [read_golden(path) for path in a.golden_receipt]
    if len(goldens) != 3 or {g[0]["target"] for g in goldens} != TARGETS:
        raise ValueError("provide one immutable receipt for each exact selected FRFCFS target")
    a.output_dir.mkdir(parents=True, exist_ok=False)
    d = a.output_dir.resolve()
    cones = {info["cone_sha256"]: cone for info, cone in goldens}
    (d / "tb.sv").write_text(testbench())
    commands = [
        [str(a.fixture.resolve()), str(d / "native.mlir")],
        [str(a.firtool.resolve()), str(d / "native.mlir"), "--format=mlir", "--verilog", "-o", str(d / "native.sv")],
    ]
    report = {"scope": "RefreshUnit pure combinational cone only; arbitrary rank/bank status and masks; all five outputs including invalid proposal defaults; no clock/reset assumptions; not full-target parity, FRFCFS integration or formal proof",
              "targets": [info for info, _ in goldens], "commands": commands,
              "tools": {str(f.resolve()): digest(f) for f in [a.fixture, a.firtool, a.verilator]},
              "testbench_seed": "0x52454634", "vectors_per_unique_cone": 118304,
              "unique_literal_cones": len(cones), "cone_results": {}, "passed": False}
    try:
        for i, command in enumerate(commands):
            with (d / f"stage-{i}.log").open("w") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        for index, (identity, cone) in enumerate(cones.items()):
            name = f"cone-{index}"
            (d / f"{name}.sv").write_text(cone)
            build = [str(a.verilator.resolve()), "--binary", "--timing", "-DSYNTHESIS", "--top-module", "tb", "-Wno-fatal", "-j", str(a.jobs),
                     "--Mdir", str(d / f"{name}-obj"), str(d / "native.sv"), str(d / f"{name}.sv"), str(d / "tb.sv")]
            run = [str(d / f"{name}-obj/Vtb")]
            commands += [build, run]
            for stage, command in [("build", build), ("run", run)]:
                with (d / f"{name}-{stage}.log").open("w") as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            evidence = (d / f"{name}-run.log").read_text()
            if "PASS refresh differential cases=118304 outputs=5" not in evidence:
                raise ValueError("missing complete target PASS marker")
            report["cone_results"][identity] = {"passed": True, "literal_sfc_cone": f"{name}.sv", "evidence": evidence}
        for info, _ in goldens:
            if digest(Path(info["sfc"])) != info["sfc_sha256"] or digest(Path(info["manifest"])) != info["manifest_sha256"] or digest(Path(info["receipt"])) != info["receipt_sha256"]:
                raise ValueError("golden source/manifest/receipt changed during comparison")
            info["passed"] = report["cone_results"][info["cone_sha256"]]["passed"]
        report["passed"] = all(info["passed"] for info, _ in goldens)
    finally:
        report["artifacts"] = {str(f.relative_to(d)): digest(f) for f in d.iterdir() if f.is_file()}
        (d / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS refresh differential: all three exact immutable target receipts; "
          f"{len(cones)} unique literal cone(s); 118304 vectors/cone; all five outputs")


if __name__ == "__main__":
    main()
