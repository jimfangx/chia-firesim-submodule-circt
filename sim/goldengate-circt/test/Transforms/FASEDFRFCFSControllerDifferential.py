#!/usr/bin/env python3
"""Compare the composed native FRFCFS timing controller to immutable SFC.

Only raw LLC memory requests, parent target pending counts and LLC response
readiness cross the replay boundary. Native frontend, decoded metadata, target
cycle, backend availability and completions are computed independently. The SFC
model is untouched and receives no native feedback. This is component evidence,
excluding LLC implementation, full target integration and formal proof.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

from FASEDRankStateTrackerDifferential import closure, digest


def testbench(cone, capacity):
    model = re.search(r"(?ms)^module FirstReadyFCFSModel\(.*?^\);", cone).group()
    ports = re.findall(r"\b(input|output)\s+(?:\[(\d+):0\]\s*)?(\w+)\s*[,)]?", model)
    inputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "input"}
    outputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "output"}
    sv = ["module tb;", "logic clock=0, targetFire=1;", "wire sfcClock=clock && targetFire;"]
    sv += [f"logic [{w-1}:0] {n}=0;" for n, w in inputs.items() if n != "clock"]
    sv += [f"wire [{w-1}:0] {n};" for n, w in outputs.items()]
    connections = [f".{n}({'sfcClock' if n == 'clock' else n})" for _, _, n in ports]
    sv += ["FirstReadyFCFSModel sfc(" + ",".join(connections) + ");"]
    # Replay only the raw memory-request/response boundary of the SFC LLC.
    ni = dict(clock="clock", reset="reset", targetFire="targetFire",
              schedulerWindowSize="tNasti_io_mmReg_schedulerWindowSize",
              openPagePolicy="tNasti_io_mmReg_openPagePolicy",
              backendLatency="tNasti_io_mmReg_backendLatency",
              pendingWReq="sfc.SatUpDownCounter_2_io_value",
              pendingAWReq="sfc.SatUpDownCounter_1_io_value",
              completedReadReady="sfc.backend_io_completedRead_ready",
              completedWriteReady="1'b1",
              wValid="sfc.xactionScheduler_io_req_w_valid",
              wLast="sfc.xactionScheduler_io_req_w_bits_last")
    for ch in ("ar", "aw"):
        for native, oracle in (("Valid","valid"),("Id","bits_id"),("Addr","bits_addr")):
            ni[ch+native] = "sfc.xactionScheduler_io_req_"+ch+"_"+oracle
        ni[ch+"Len"]="8'd0"
    for stem in ("bank","rank","row"):
        for native, oracle in (("Offset","offset"),("Mask","mask")):
            ni[stem+"Addr"+native]="tNasti_io_mmReg_"+stem+"Addr_"+oracle
    fields = dict(id=4, len=8, isWrite=1, rowAddr=26, bankAddrOH=8,
                  bankAddr=3, rankAddrOH=4, rankAddr=2, isReady=1, mayPRE=1)
    for n in "tAL tCAS tCMD tCWD tCCD tFAW tRAS tREFI tRC tRCD tRFC tRRD tRP tRTP tRTRS tWR tWTR".split():
        ni["timings_" + n] = "tNasti_io_mmReg_dramTimings_" + n
    no = dict(nextReady=1, newReadValid=1, newReadId=4, newReadLen=8,
              newWriteValid=1, newWriteId=4, readLatency=12, writeLatency=12,
              selectedCmd=3, cmdRank=2, cmdBank=3, cmdRow=26, autoPRE=1,
              bankHasReadyEntries=32)
    no.update(arReady=1,awReady=1,wReady=1,completedReadValid=1,completedReadID=4,
              completedReadLen=8,completedWriteValid=1,completedWriteID=4,targetCycle=64,
              readCount=3,writeCount=3,backendReadReady=1,backendWriteReady=1,
              nextValid=1,nextId=4,nextLen=8,nextIsWrite=1,nextAddr=35,ranksInUse=4)
    for n,w in fields.items():
        if n not in {"isReady","mayPRE"}: no["decoded_"+n]=w
    for i in range(8):
        no[f"entries_{i}_valid"] = 1
        no.update({f"entries_{i}_bits_{n}": w for n, w in fields.items()})
    rank_fields = "canCASW canCASR canPRE canACT canREF wantREF state".split()
    bank_fields = "canCASW canCASR canPRE canACT state openRow".split()
    for r in range(4):
        no.update({f"rankStati_{r}_{n}": 1 for n in rank_fields})
        for b in range(8):
            no.update({f"rankStati_{r}_banks_{b}_{n}": 26 if n == "openRow" else 1 for n in bank_fields})
    sv += [f"wire [{w-1}:0] native_{n};" for n, w in no.items()]
    sv += [f"GGFASEDFRFCFSController{capacity} native(" + ",".join([f".{n}({v})" for n, v in ni.items()] + [f".{n}(native_{n})" for n in no]) + ");"]
    sv += ["integer cycles=0; integer commands[0:5]; integer accepted=0, stalled=0,backendFull=0,frontendBlocked=0,competing=0;", "task compare; begin"]

    def check(n, ref, enable="1'b1"):
        sv.append(f'if (({enable}) && native_{n} !== {ref}) $fatal(1,"cycle=%0d field={n} native=%0d sfc=%0d cmd=%0d",cycles,native_{n},{ref},sfc.selectedCmd);')

    check("targetCycle", "sfc.tCycle")
    for ch in ("ar","aw","w"): check(ch+"Ready", "sfc.xactionScheduler_io_req_"+ch+"_ready")
    for ch in ("Read","Write"):
        check("completed"+ch+"Valid", "sfc.backend_io_completed"+ch+"_valid")
        check("completed"+ch+"ID", "sfc.backend_io_completed"+ch+"_bits_id", "native_completed"+ch+"Valid")
        check("backend"+ch+"Ready", "sfc.backend_io_new"+ch+"_ready")
    check("nextValid", "sfc.xactionScheduler_io_nextXaction_valid")
    for native, oracle in (("Id","id"),("IsWrite","isWrite"),("Addr","addr")):
        check("next"+native,"sfc.xactionScheduler_io_nextXaction_bits_addr" if oracle=="addr" else "sfc.refBuffer_io_enq_bits_xaction_"+oracle,"native_nextValid")
    check("ranksInUse", "sfc.RefreshUnit_io_ranksInUse")
    for n in fields:
        if n not in {"len","isReady","mayPRE"}:
            check("decoded_"+n, "sfc.refBuffer_io_enq_bits_"+("xaction_" if n in {"id","isWrite"} else "")+n,"native_nextValid")
    check("nextReady", "sfc.refBuffer_io_enq_ready")
    check("newReadValid", "sfc.backend_io_newRead_valid")
    check("newWriteValid", "sfc.backend_io_newWrite_valid")
    check("newReadId", "sfc.backend_io_newRead_bits_id", "native_newReadValid")
    check("newWriteId", "sfc.backend_io_newWrite_bits_id", "native_newWriteValid")
    check("readLatency", "sfc.backend_io_readLatency")
    check("writeLatency", "12'd1")
    check("selectedCmd", "sfc.selectedCmd")
    check("cmdRank", "sfc.cmdRank", "native_selectedCmd!=0")
    check("cmdBank", "sfc.cmdBank", "native_selectedCmd!=0 && native_selectedCmd!=5")
    check("cmdRow", "sfc.rankStateTrackers_0_io_cmdRow", "native_selectedCmd==1")
    check("autoPRE", "sfc.rankStateTrackers_0_io_autoPRE")
    check("bankHasReadyEntries", "{" + ",".join(f"sfc.bankHasReadyEntries_{i}" for i in reversed(range(32))) + "}")
    for i in range(8):
        check(f"entries_{i}_valid", f"sfc.refBuffer_io_entries_{i}_valid")
        for n in fields:
            if n != "len":
                check(f"entries_{i}_bits_{n}", f"sfc.refBuffer_io_entries_{i}_bits_" + ("xaction_" if n in {"id", "isWrite"} else "") + n, f"native_entries_{i}_valid")
    for r in range(4):
        for n in rank_fields:
            check(f"rankStati_{r}_{n}", f"sfc.rankStateTrackers_{r}_io_rank_{n}")
        for b in range(8):
            for n in bank_fields:
                check(f"rankStati_{r}_banks_{b}_{n}", f"sfc.rankStateTrackers_{r}_io_rank_banks_{b}_{n}", f"native_rankStati_{r}_banks_{b}_state" if n == "openRow" else "1'b1")
    sv += ["end endtask", "task step; begin #2; compare();",
           "if(targetFire && !reset)begin commands[sfc.selectedCmd]++; accepted+=sfc.newReference_valid && sfc.newReference_ready;end",
           "if(targetFire && !reset)begin backendFull+=!native_backendReadReady;frontendBlocked+=native_nextValid && !native_nextReady;competing+=sfc.xactionScheduler_io_req_ar_valid && sfc.xactionScheduler_io_req_aw_valid;end if(!targetFire)stalled++; clock=1; #2; compare(); clock=0; #2;cycles++; end endtask",
           "integer i,phase,j; integer reads=0,writes=0,wOwed=0; integer newAR,newAW,newW; integer phaseStart=0,phaseCoverage=0; logic [15:0] readIDs=0,writeIDs=0;",
           # The functional egress holds the requested ID until its response
           # is consumed; echoing the latest request combinationally is wrong
           # when the releaser's one-entry pipe is stalled or replaced.
           "logic [3:0] readEgressIDs[0:3],writeEgressIDs[0:3]; logic [1:0] reEnq=0,reDeq=0,weEnq=0,weDeq=0;",
           "always_comb begin tNasti_io_egressResp_bBits_id=writeEgressIDs[weDeq];",
           "tNasti_io_egressResp_rBits_id=readEgressIDs[reDeq];tNasti_io_egressResp_rBits_last=1;end",
           "always @(posedge sfcClock)begin if(reset)begin reEnq<=0;reDeq<=0;weEnq<=0;weDeq<=0;end else begin",
           "if(tNasti_io_egressReq_r_valid)begin readEgressIDs[reEnq]<=tNasti_io_egressReq_r_bits;reEnq<=reEnq+1'b1;end",
           "if(tNasti_io_egressReq_b_valid)begin writeEgressIDs[weEnq]<=tNasti_io_egressReq_b_bits;weEnq<=weEnq+1'b1;end",
           "if(tNasti_io_tNasti_r_valid && tNasti_io_tNasti_r_ready)reDeq<=reDeq+1'b1;",
           "if(tNasti_io_tNasti_b_valid && tNasti_io_tNasti_b_ready)weDeq<=weDeq+1'b1;end end",
           "initial begin for(i=0;i<6;i++)commands[i]=0;",
           "tNasti_io_mmReg_llc_wayBits=1;tNasti_io_mmReg_llc_setBits=2;tNasti_io_mmReg_llc_blockBits=6;tNasti_io_mmReg_llc_activeMSHRs=8;",
           "tNasti_io_mmReg_bankAddr_offset=6;tNasti_io_mmReg_bankAddr_mask=7;",
           "tNasti_io_mmReg_rankAddr_offset=9;tNasti_io_mmReg_rankAddr_mask=3;",
           "tNasti_io_mmReg_rowAddr_offset=11;tNasti_io_mmReg_rowAddr_mask=26'h3ffffff;",
           "tNasti_io_mmReg_backendLatency=12;tNasti_io_tNasti_ar_bits_burst=1;tNasti_io_tNasti_aw_bits_burst=1;",
           "tNasti_io_tNasti_ar_bits_len=0;tNasti_io_tNasti_aw_bits_len=0;tNasti_io_tNasti_w_bits_last=1;"]
    defaults = [0, 14, 1, 10, 4, 25, 33, 7800, 47, 14, 160, 8, 14, 8, 2, 15, 8]
    for n, v in zip("tAL tCAS tCMD tCWD tCCD tFAW tRAS tREFI tRC tRCD tRFC tRRD tRP tRTP tRTRS tWR tWTR".split(), defaults):
        sv.append(f"tNasti_io_mmReg_dramTimings_{n}={v};")
    # Multiple outstanding requests stress row hits/conflicts/refresh, writes,
    # backpressure and the actual LLC/transaction/backend boundary environment.
    sv += ["reset=1;step();reset=0;",
           "for(phase=0;phase<16;phase++)begin",
           "phaseStart=accepted;tNasti_io_mmReg_backendLatency=(phase==0)?1200:12;",
           "tNasti_io_mmReg_dramTimings_tREFI=(phase<8)?7800:600;",
           "if(phase%4==0 && phase!=0)begin targetFire=0;reset=1;step();targetFire=1;step();reset=0;reads=0;writes=0;wOwed=0;readIDs=0;writeIDs=0;tNasti_io_tNasti_ar_valid=0;tNasti_io_tNasti_aw_valid=0;tNasti_io_tNasti_w_valid=0;end",
           "tNasti_io_mmReg_schedulerWindowSize=3'(phase);tNasti_io_mmReg_openPagePolicy=(phase<8);",
           "for(i=0;i<3000;i++)begin",
           "targetFire=($urandom_range(0,4)!=0);",
           "tNasti_io_tNasti_r_ready=($urandom_range(0,7)!=0);tNasti_io_tNasti_b_ready=($urandom_range(0,7)!=0);",
           "if(!tNasti_io_tNasti_ar_valid && reads<8 && (i<800 || (i>=1600 && i<2400)))begin tNasti_io_tNasti_ar_valid=($urandom_range(0,7)==0);tNasti_io_tNasti_ar_bits_addr=({3'($urandom()),32'($urandom())} & 35'h7ffffffc0);for(j=0;j<16;j++)if(!readIDs[j])tNasti_io_tNasti_ar_bits_id=4'(j);end",
           "if(!tNasti_io_tNasti_aw_valid && writes<4 && i>=800 && i<2400)begin tNasti_io_tNasti_aw_valid=($urandom_range(0,7)==0);tNasti_io_tNasti_aw_bits_addr=({3'($urandom()),32'($urandom())} & 35'h7ffffffc0);for(j=0;j<16;j++)if(!writeIDs[j])tNasti_io_tNasti_aw_bits_id=4'(j);end",
           "tNasti_io_tNasti_w_valid=(wOwed>0);#1;",
           "newAR=targetFire && tNasti_io_tNasti_ar_valid && tNasti_io_tNasti_ar_ready;newAW=targetFire && tNasti_io_tNasti_aw_valid && tNasti_io_tNasti_aw_ready;",
           "newW=targetFire && tNasti_io_tNasti_w_valid && tNasti_io_tNasti_w_ready;wOwed+=newAW-newW;",
           "if(newAR)begin if(readIDs[tNasti_io_tNasti_ar_bits_id])$fatal(1,\"read ID illegally reused\");readIDs[tNasti_io_tNasti_ar_bits_id]=1;end",
           "if(newAW)begin if(writeIDs[tNasti_io_tNasti_aw_bits_id])$fatal(1,\"write ID illegally reused\");writeIDs[tNasti_io_tNasti_aw_bits_id]=1;end",
           "if(targetFire && tNasti_io_tNasti_r_valid && tNasti_io_tNasti_r_ready && tNasti_io_tNasti_r_bits_last)begin if(!readIDs[tNasti_io_tNasti_r_bits_id])$fatal(1,\"unsolicited read response\");readIDs[tNasti_io_tNasti_r_bits_id]=0;end",
           "if(targetFire && tNasti_io_tNasti_b_valid && tNasti_io_tNasti_b_ready)begin if(!writeIDs[tNasti_io_tNasti_b_bits_id])$fatal(1,\"unsolicited write response\");writeIDs[tNasti_io_tNasti_b_bits_id]=0;end",
           "reads+=newAR-(targetFire && tNasti_io_tNasti_r_valid && tNasti_io_tNasti_r_ready && tNasti_io_tNasti_r_bits_last);",
           "writes+=newAW-(targetFire && tNasti_io_tNasti_b_valid && tNasti_io_tNasti_b_ready);",
           "step();if(newAR)tNasti_io_tNasti_ar_valid=0;if(newAW)tNasti_io_tNasti_aw_valid=0;",
           "end if(accepted>phaseStart)phaseCoverage|=(1<<phase);end",
           "targetFire=1;tNasti_io_tNasti_ar_valid=0;tNasti_io_tNasti_aw_valid=0;tNasti_io_tNasti_r_ready=1;tNasti_io_tNasti_b_ready=1;",
           "for(i=0;i<6000;i++)begin tNasti_io_tNasti_w_valid=(wOwed>0);#1;",
           "if(tNasti_io_tNasti_w_valid && tNasti_io_tNasti_w_ready)wOwed--;",
           "if(tNasti_io_tNasti_r_valid && tNasti_io_tNasti_r_ready && tNasti_io_tNasti_r_bits_last)reads--;",
           "if(tNasti_io_tNasti_b_valid && tNasti_io_tNasti_b_ready)writes--;step();end",
           '$display("coverage accepted=%0d readsOutstanding=%0d writesOutstanding=%0d ACT=%0d PRE=%0d CASW=%0d CASR=%0d REF=%0d",accepted,reads,writes,commands[1],commands[2],commands[3],commands[4],commands[5]);',
           "for(i=1;i<6;i++)if(commands[i]==0)$fatal(1,\"missing command coverage cmd=%0d\",i);",
           'if(phaseCoverage!=65535)$fatal(1,"missing per-depth/page-policy accepted traffic coverage mask=%h",phaseCoverage);',
           'if(reads!=0 || writes!=0 || wOwed!=0)$fatal(1,"incomplete final drain reads=%0d writes=%0d Wdata=%0d",reads,writes,wOwed);',
           'if(accepted<100 || stalled<100)$fatal(1,"insufficient traffic/fire coverage accepted=%0d stalled=%0d",accepted,stalled);',
           '$display("composition coverage backendFull=%0d frontendBlocked=%0d competing=%0d",backendFull,frontendBlocked,competing);',
           'if(backendFull==0 || frontendBlocked==0 || competing==0)$fatal(1,"missing composition backpressure coverage");',
           '$display("PASS FRFCFS controller differential cycles=%0d accepted=%0d stalls=%0d phases=%h ACT=%0d PRE=%0d CASW=%0d CASR=%0d REF=%0d",cycles,accepted,stalled,phaseCoverage,commands[1],commands[2],commands[3],commands[4],commands[5]);$finish;end endmodule']
    return "\n".join(sv) + "\n"


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
    report = dict(scope=__doc__, targets=targets, passed=False, cones=[], comparator_sha256=digest(Path(__file__)), tools={str(t.resolve()): digest(t) for t in (a.fixture,a.firtool,a.verilator)}, normalizations=["SFC dead-field eliminated len: native len input fixed zero, len output not compared", "invalid entry payload and command rank/bank/row fields ignored only where command does not consume them", "unreset openRow compared only in active banks", "SFC target clock gated by targetFire"], assumptions=["SFC LLC BRAM initialized to zero by Verilator, matching documented programming initialization", "single-beat target AXI requests, at most eight reads/four writes and one outstanding per channel/ID (legal for all three constructors), functional-egress ID FIFOs hold single-beat responses until target consumption", "fixed full-35-bit runtime address layout; backend latency 1200 then 12; default timings then shorter legal refresh interval", "unreset RR lastGrant starts zero in both Verilator models", "SFC design assertions disabled by SYNTHESIS; rank assertion predicates separately tested"], excluded=["LLC implementation, functional target memory/releaser, power/MMIO, full target parity, formal proof"])
    try:
        for sha, (cone, modules, capacity) in cones.items():
            out = d / sha[:12]; out.mkdir()
            (out / "sfc-cone.sv").write_text(cone); (out / "tb.sv").write_text(testbench(cone, capacity))
            commands = [[str(a.fixture.resolve()),str(capacity),str(out/"native.mlir")], [str(a.firtool.resolve()),str(out/"native.mlir"),"--format=mlir","--verilog","-o",str(out/"native.sv")], [str(a.verilator.resolve()),"--binary","--timing","-DSYNTHESIS","--top-module","tb","-Wno-fatal","-j",str(a.jobs),"--Mdir",str(out/"obj"),str(out/"native.sv"),str(out/"sfc-cone.sv"),str(out/"tb.sv")], [str(out/"obj/Vtb")]]
            result = dict(cone_sha256=sha,modules=modules,commands=commands,passed=False); report["cones"].append(result)
            for i,cmd in enumerate(commands):
                with (out/f"stage-{i}.log").open("w") as log:
                    subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=240)
            result["evidence"]=(out/"stage-3.log").read_text()
            if "PASS FRFCFS controller differential" not in result["evidence"]:
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
