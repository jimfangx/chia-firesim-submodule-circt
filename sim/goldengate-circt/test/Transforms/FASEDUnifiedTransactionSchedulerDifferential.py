#!/usr/bin/env python3
"""Immutable SFC frontend cone replay; scoped simulation, not target/formal parity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from FASEDRankStateTrackerDifferential import closure, digest

TARGETS = {"megaboomv3": 20, "shuttle": 10, "saturn_shuttle": 20}


def golden(path):
    r = json.loads(path.read_text())
    mpath = Path(r["manifest"]["path"])
    if r["status"] != "complete" or digest(mpath) != r["manifest"]["sha256"]:
        raise ValueError("bad receipt/manifest identity")
    m = json.loads(mpath.read_text())
    if m["status"] != "complete" or m["target"] != r["target"]:
        raise ValueError("incomplete target")
    sfc = mpath.parent / "sfc/FireSim-generated.sv"
    h = digest(sfc)
    if h != m["sfc_artifacts"][sfc.name]["sha256"]:
        raise ValueError("modified immutable SFC")
    text = sfc.read_text()
    cone, modules = closure(text, "UnifiedFIFOXactionScheduler")
    header = re.search(r"(?ms)^module UnifiedFIFOXactionScheduler\(.*?^\);", cone).group()
    width = int(re.search(r"input\s+\[(\d+):0\] io_pendingWReq", header).group(1))
    if width != TARGETS[r["target"]]:
        raise ValueError("selected constructor maxWrites mismatch")
    if "bits_xaction_len" in header or "bits_len" in header:
        raise ValueError("new observable SFC length requires explicit port comparison")
    return dict(target=r["target"], maxWrites=width, receipt=str(path.resolve()), receipt_sha256=digest(path),
                manifest=str(mpath), manifest_sha256=digest(mpath), sfc=str(sfc), sfc_sha256=h,
                cone_sha256=hashlib.sha256(cone.encode()).hexdigest(), modules=modules, passed=False), cone


def testbench(capacity):
    inputs = dict(reset=1,targetFire=1,arValid=1,arId=4,arLen=8,arAddr=35,awValid=1,awId=4,awLen=8,awAddr=35,
                  wValid=1,wLast=1,pendingWReq=capacity+1,pendingAWReq=capacity+1,nextReady=1)
    outputs = dict(arReady=1,awReady=1,wReady=1,nextValid=1,nextId=4,nextIsWrite=1,nextAddr=35)
    native = [".clock(clock)"]+[f".{n}({n})" for n in inputs]+[f".{n}(native_{n})" for n in outputs]+[".nextLen()"]
    mapping = {"arReady":"io_req_ar_ready", "awReady":"io_req_aw_ready", "wReady":"io_req_w_ready",
               "nextValid":"io_nextXaction_valid", "nextId":"io_nextXaction_bits_xaction_id",
               "nextIsWrite":"io_nextXaction_bits_xaction_isWrite", "nextAddr":"io_nextXaction_bits_addr"}
    sfc = [".clock(sfcClock)",".reset(reset)"]
    for typ in ["ar","aw"]:
        sfc += [f".io_req_{typ}_valid({typ}Valid)",f".io_req_{typ}_bits_id({typ}Id)",f".io_req_{typ}_bits_addr({typ}Addr)"]
    sfc += [".io_req_w_valid(wValid)",".io_req_w_bits_last(wLast)",".io_pendingWReq(pendingWReq)",
            ".io_pendingAWReq(pendingAWReq)",".io_nextXaction_ready(nextReady)"]
    sfc += [f".{mapping[n]}(sfc_{n})" for n in outputs]
    t = ["module tb; logic clock=0;wire sfcClock=clock&&targetFire;"]
    t += [f"logic [{w-1}:0] {n}=0;" for n,w in inputs.items()]
    t += [f"wire [{w-1}:0] native_{n},sfc_{n};" for n,w in outputs.items()]
    t += [f"GGFASEDUnifiedTransactionScheduler{capacity} native("+",".join(native)+");",
          "UnifiedFIFOXactionScheduler sfc("+",".join(sfc)+");",
          "integer cycles=0,arCount=0,awCount=0,wCount=0,releases=0,blocked=0,full=0,stalls=0,resetGrants=0;",
          "integer rng=32'h71a9ac7;",
          "function automatic integer random32;begin rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);random32=rng;end endfunction",
          "task compare;begin #1;"]
    for n in ["arReady","awReady","wReady","nextValid"]:
        t += [f'if(native_{n}!==sfc_{n})$fatal(1,"cycle=%0d {n}: native=%0d sfc=%0d",cycles,native_{n},sfc_{n});']
    for n in ["nextId","nextIsWrite","nextAddr"]:
        t += [f'if(native_nextValid && native_{n}!==sfc_{n})$fatal(1,"cycle=%0d {n}: native=%0d sfc=%0d",cycles,native_{n},sfc_{n});']
    t += ["end endtask", "task step;begin compare();",
          "if(targetFire)begin arCount+=arValid&&native_arReady;awCount+=awValid&&native_awReady;",
          "wCount+=wValid&&wLast&&native_wReady;releases+=native_nextValid&&nextReady;",
          "blocked+=!native_nextValid;full+=(arValid||awValid)&&!native_arReady&&!native_awReady;",
          "resetGrants+=reset&&(arValid&&native_arReady||awValid&&native_awReady);end else stalls++;",
          "clock=1;#2;compare();clock=0;#1;cycles++;end endtask", "integer i;initial begin",
          # RR winner is intentionally unreset. A sole AR grant under reset
          # gives both designs a defined common winner without forcing state.
          "reset=1;targetFire=1;arValid=1;awValid=0;#1;clock=1;#2;clock=0;#1;",
          "reset=0;arValid=1;awValid=1;nextReady=0;",
          "for(i=0;i<20;i++)begin arId=4'(i);awId=4'(i+7);arAddr=35'(i*81);awAddr=35'(i*97);step();end",
          f"arValid=0;awValid=0;wValid=1;wLast=1;for(i=0;i<{capacity+8};i++)step();",
          "nextReady=1;for(i=0;i<24;i++)step();",
          "reset=1;arValid=1;awValid=1;step();targetFire=0;step();reset=0;targetFire=1;",
          "for(i=0;i<60000;i++)begin",
          "arValid=1'(random32());awValid=1'(random32());arId=4'(random32());awId=4'(random32());",
          "arLen=8'(random32());awLen=8'(random32());arAddr={3'(random32()),32'(random32())};awAddr={3'(random32()),32'(random32())};",
          f"pendingWReq={capacity+1}'(random32());pendingAWReq={capacity+1}'(random32());",
          "wValid=1'(random32());wLast=(32'(random32())%4!=0);nextReady=(32'(random32())%3!=0);",
          "reset=(32'(random32())%101==0);targetFire=(32'(random32())%5!=0);step();end",
          "reset=0;targetFire=1;arValid=0;awValid=0;wValid=1;wLast=1;pendingWReq=0;pendingAWReq=0;nextReady=1;",
          "for(i=0;i<40;i++)step();",
          'if(native_nextValid||!arCount||!awCount||!wCount||!releases||!blocked||!full||!stalls||!resetGrants)$fatal(1,"missing coverage/drain");',
          f'$display("PASS unified transaction differential maxWrites={capacity} cycles=%0d ar=%0d aw=%0d completedW=%0d released=%0d blocked=%0d full=%0d stalls=%0d resetGrants=%0d",cycles,arCount,awCount,wCount,releases,blocked,full,stalls,resetGrants);',
          "$finish;end endmodule"]
    return "\n".join(t)+"\n"


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ["fixture","firtool","verilator","output-dir"]:p.add_argument("--"+n,required=True,type=Path)
    p.add_argument("--golden-receipt",required=True,action="append",type=Path)
    p.add_argument("--jobs",type=int,default=1)
    a=p.parse_args();goldens=[golden(p) for p in a.golden_receipt]
    if len(goldens)!=3 or {g[0]["target"] for g in goldens}!=set(TARGETS):raise ValueError("provide all three exact selected FRFCFS receipts")
    if a.jobs<1:raise ValueError("jobs must be positive")
    a.output_dir.mkdir(parents=True,exist_ok=False);d=a.output_dir.resolve()
    report=dict(scope="Eight-entry UnifiedFIFOXactionScheduler only; independent targetFire-gated SFC clock; arbitrary requests/pending counters/reset/stalls; common unreset RR winner established by one AR grant under reset; compare readiness, validity and valid metadata. Native len retained, but absent/pruned in selected SFC cone and checked separately in C++ reference. Not full-target parity or formal proof.",
                targets=[g[0] for g in goldens],commands=[],tools={str(f.resolve()):digest(f) for f in [a.fixture,a.firtool,a.verilator]},cone_results={},passed=False)
    try:
        seen=set()
        for info,cone in goldens:
            identity=info["cone_sha256"]
            if identity in seen:continue
            seen.add(identity);sub=d/f"cone-{len(seen)-1}";sub.mkdir();cap=info["maxWrites"]
            (sub/"sfc.sv").write_text(cone);(sub/"tb.sv").write_text(testbench(cap))
            commands=[[str(a.fixture.resolve()),str(cap),str(sub/"native.mlir")],
                      [str(a.firtool.resolve()),str(sub/"native.mlir"),"--format=mlir","--verilog","-o",str(sub/"native.sv")],
                      [str(a.verilator.resolve()),"--binary","--timing","-DSYNTHESIS","--top-module","tb","-Wno-fatal","-j",str(a.jobs),"--Mdir",str(sub/"obj"),str(sub/"native.sv"),str(sub/"sfc.sv"),str(sub/"tb.sv")],
                      [str(sub/"obj/Vtb")]]
            for i,command in enumerate(commands):
                report["commands"].append(command)
                with (sub/f"stage-{i}.log").open("w") as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=240)
            evidence=(sub/"stage-3.log").read_text()
            if f"PASS unified transaction differential maxWrites={cap} cycles=" not in evidence:raise ValueError("missing target PASS")
            report["cone_results"][identity]=dict(passed=True,directory=str(sub),evidence=evidence)
        for info,_ in goldens:
            for name in ["receipt","manifest","sfc"]:
                if digest(Path(info[name]))!=info[name+"_sha256"]:raise ValueError("golden changed during comparison")
            info["passed"]=report["cone_results"][info["cone_sha256"]]["passed"]
        report["passed"]=all(g[0]["passed"] for g in goldens)
    finally:
        report["artifacts"]={str(f.relative_to(d)):digest(f) for f in d.rglob("*") if f.is_file() and "obj" not in f.parts}
        (d/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(f"PASS unified transaction scheduler: three immutable selected target receipts; {len(seen)} exact cones")

if __name__=="__main__":main()
