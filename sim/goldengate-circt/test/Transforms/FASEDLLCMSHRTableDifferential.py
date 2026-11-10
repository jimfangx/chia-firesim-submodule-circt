#!/usr/bin/env python3
"""Replay native LLC MSHR state from immutable complete selected SFC models.

Only allocation, completion, decoded-set and settings inputs cross the replay
boundary. The SFC model remains unmodified and receives no native feedback.
Compares allocation availability/index/count, set hazards, retained metadata and
all enabled/flight flags. This is a component comparison, not target parity.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
from FASEDRankStateTrackerDifferential import closure, digest
from FASEDFRFCFSControllerDifferential import testbench as traffic_testbench


def testbench(cone, capacity):
    sv=traffic_testbench(cone,capacity)
    sv=re.sub(r"(?m)^wire \[.*? native_.*?;\n", "", sv)
    sv=re.sub(r"(?m)^GGFASEDFRFCFSController\d+ native\(.*?;\n", "", sv)
    ni=dict(clock="clock",reset="reset",targetFire="targetFire",activeMSHRs="tNasti_io_mmReg_llc_activeMSHRs",readSet="sfc.llc_model.read_set",writeSet="sfc.llc_model.write_set",allocate="sfc.llc_model.allocate_mshr",allocateSet="sfc.llc_model.s1_set_addr",allocateId="sfc.llc_model.w_id",allocateLen="8'd0",allocateIsWrite="sfc.llc_model.state!=1",allocateAddr="35'd0",allocateRead="sfc.llc_model.need_refill",allocateWrite="sfc.llc_model.need_writeback",completedRead="sfc.llc_model.refill_start",completedReadId="sfc.llc_model.io_memRResp_bits_id",completedWrite="sfc.llc_model.io_memWResp_valid",completedWriteId="sfc.llc_model.io_memWResp_bits_id")
    ni["allocateId"]="sfc.llc_model.state==1 ? sfc.llc_model.w_id : sfc.llc_model.w_1_id"
    no=dict(available=1,nextIndex=3,allocated=4,readCollision=1,writeCollision=1,responseId=4,responseLen=8,responseIsWrite=1,responseAddr=35)
    fields=dict(setAddr=12,id=4,len=8,isWrite=1,addr=35,writeInFlight=1,readInFlight=1,enabled=1)
    for i in range(8):no.update({f"entries_{i}_{n}":w for n,w in fields.items()})
    decl="\n".join(f"wire [{w-1}:0] native_{n};" for n,w in no.items())+"\n"
    decl+="GGFASEDLLCMSHRTable native("+",".join([f".{n}({v})" for n,v in ni.items()]+[f".{n}(native_{n})" for n in no])+ ");\n"
    sv=sv.replace("integer cycles=0;",decl+"integer cycles=0;")
    checks=[]
    def check(n,ref,guard="1'b1"):
        checks.append(f'if(({guard}) && native_{n} !== {ref})$fatal(1,"cycle=%0d field={n} native=%0d sfc=%0d",cycles,native_{n},{ref});')
    for n,ref in dict(available="mshr_available",nextIndex="mshr_next_idx",allocated="mshrs_allocated",readCollision="read_set_collision",writeCollision="write_set_collision").items():check(n,"sfc.llc_model."+ref)
    check("responseId","sfc.llc_model.io_rResp_bits_readMetaData_id","sfc.llc_model.refill_start")
    for i in range(8):
        stem=f"sfc.llc_model.mshrs_{i}_"
        for n,ref in dict(writeInFlight="wb_in_flight",readInFlight="acq_in_flight",enabled="enabled",setAddr="set_addr",id="xaction_id",isWrite="xaction_isWrite").items():
            guard=f"({stem}wb_in_flight || {stem}acq_in_flight)" if n in {"setAddr","id","isWrite"} else "1'b1"
            check(f"entries_{i}_{n}",stem+ref,guard)
    sv=re.sub(r"task compare; begin.*?end endtask", "task compare; begin\n"+"\n".join(checks)+"\nend endtask",sv,flags=re.S)
    sv=sv.replace("native_backendReadReady","sfc.backend_io_newRead_ready").replace("native_nextValid","sfc.xactionScheduler_io_nextXaction_valid").replace("native_nextReady","sfc.refBuffer_io_enq_ready")
    sv=sv.replace("PASS FRFCFS controller differential","PASS LLC MSHR differential")
    return sv


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ("fixture", "firtool", "verilator", "output-dir"):
        p.add_argument("--" + n, type=Path, required=True)
    p.add_argument("--golden-receipt", type=Path, action="append", required=True)
    p.add_argument("--jobs", type=int, default=4)
    a = p.parse_args()
    d = a.output_dir.resolve(); d.mkdir(parents=True, exist_ok=False)
    targets = []; cones = {}
    for receipt_path in a.golden_receipt:
        receipt = json.loads(receipt_path.read_text()); m = Path(receipt["manifest"]["path"])
        if receipt["status"] != "complete" or digest(m) != receipt["manifest"]["sha256"]:
            raise ValueError("incomplete or changed SFC receipt")
        manifest = json.loads(m.read_text()); source = m.parent / "sfc/FireSim-generated.sv"
        if manifest["status"] != "complete" or manifest["target"] != receipt["target"] or digest(source) != manifest["sfc_artifacts"][source.name]["sha256"]:
            raise ValueError("incomplete, mismatched or changed SFC manifest/RTL")
        cone, modules = closure(source.read_text(), "FirstReadyFCFSModel")
        import hashlib
        sha = hashlib.sha256(cone.encode()).hexdigest()
        capacity=10 if receipt["target"]=="shuttle" else 20
        cones.setdefault(sha, (cone, modules, capacity))
        targets.append(dict(target=receipt["target"], receipt=str(receipt_path.resolve()), receipt_sha256=digest(receipt_path), manifest=str(m), manifest_sha256=digest(m), sfc=str(source), sfc_sha256=digest(source), cone_sha256=sha))
    if {t["target"] for t in targets} != {"megaboomv3", "shuttle", "saturn_shuttle"} or len(targets) != 3:
        raise ValueError("require exactly the three fixed DDR3 F2 targets")
    report = dict(scope=__doc__, targets=targets, passed=False, cones=[], comparator_sha256=digest(Path(__file__)), traffic_adapter_sha256=digest(Path(__file__).with_name("FASEDFRFCFSControllerDifferential.py")), tools={str(t.resolve()):digest(t) for t in (a.fixture,a.firtool,a.verilator)}, normalizations=["native len/addr payload fixed zero because immutable SFC eliminates both from MSHRs", "unreset set/id/isWrite metadata checked only while either flight flag is set", "SFC target clock gated by targetFire"], assumptions=["SFC LLC BRAM initialized to zero by Verilator, matching documented programming initialization", "single-beat target AXI requests, at most eight reads/four writes and one outstanding per channel/ID; functional-egress ID FIFOs hold responses until consumption", "SFC design assertions disabled by SYNTHESIS; native capacity assertion predicates separately tested"], excluded=["full LLC implementation, DRAM scheduler/backend, functional target memory/releaser, MMIO/power, full target parity, formal proof"])

    try:
        for sha, (cone, modules, capacity) in cones.items():
            out = d / sha[:12]; out.mkdir()
            (out / "sfc-cone.sv").write_text(cone); (out / "tb.sv").write_text(testbench(cone, capacity))
            commands = [[str(a.fixture.resolve()),str(out/"native.mlir")], [str(a.firtool.resolve()),str(out/"native.mlir"),"--format=mlir","--verilog","-o",str(out/"native.sv")], [str(a.verilator.resolve()),"--binary","--timing","-DSYNTHESIS","--top-module","tb","-Wno-fatal","-j",str(a.jobs),"--Mdir",str(out/"obj"),str(out/"native.sv"),str(out/"sfc-cone.sv"),str(out/"tb.sv")], [str(out/"obj/Vtb")]]
            result = dict(cone_sha256=sha,modules=modules,commands=commands,passed=False); report["cones"].append(result)
            for i,cmd in enumerate(commands):
                with (out/f"stage-{i}.log").open("w") as log:
                    subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=240)
            result["evidence"]=(out/"stage-3.log").read_text()
            if "PASS LLC MSHR differential" not in result["evidence"]:
                raise ValueError("missing target PASS marker")
            result["passed"]=True
            print(result["evidence"])
        for t in targets:
            if digest(Path(t["sfc"])) != t["sfc_sha256"] or digest(Path(t["manifest"])) != t["manifest_sha256"] or digest(Path(t["receipt"])) != t["receipt_sha256"]:
                raise ValueError("immutable SFC input changed during comparison")
            t["passed"]=True
        report["passed"]=True
    finally:
        report["artifacts"]={str(f.relative_to(d)):digest(f) for f in d.rglob("*") if f.is_file() and "obj" not in f.relative_to(d).parts}
        (d/"result.json").write_text(json.dumps(report,indent=2)+"\n")


if __name__ == "__main__":
    main()
