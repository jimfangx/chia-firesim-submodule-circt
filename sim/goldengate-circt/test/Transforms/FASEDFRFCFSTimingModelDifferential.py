#!/usr/bin/env python3
"""Finite full timing-model comparison against the three immutable F2 DDR3 goldens.

Identical external target requests, response readiness, settings and functional
response metadata drive both designs. No SFC internal state drives native inputs.
The scope includes LLC, controller, release scheduling, counters and power, but
excludes functional memory implementation, host/MMIO and full-platform composition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from FASEDRankStateTrackerDifferential import closure, digest
from FASEDFRFCFSControllerDifferential import testbench as controller_tb
from FASEDLLCModelDifferential import testbench as llc_tb


def testbench(cone, capacity):
    model = re.search(r"(?ms)^module FirstReadyFCFSModel\(.*?^\);", cone).group()
    ports = re.findall(r"\b(input|output)\s+(?:\[(\d+):0\]\s*)?(\w+)\s*[,)]?", model)
    inputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "input"}
    outputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "output"}
    sv = ["module tb;", "logic clock=0,targetFire=1;wire sfcClock=clock && targetFire;"]
    sv += [f"logic [{w-1}:0] {n}=0;" for n,w in inputs.items() if n != "clock"]
    sv += [f"wire [{w-1}:0] {n};" for n,w in outputs.items()]
    sv += ["FirstReadyFCFSModel sfc(" + ",".join(f".{n}({'sfcClock' if n == 'clock' else n})" for _,_,n in ports) + ");"]
    ni = dict(clock="clock",reset="reset",targetFire="targetFire",
              wValid="tNasti_io_tNasti_w_valid",wLast="tNasti_io_tNasti_w_bits_last",
              rReady="tNasti_io_tNasti_r_ready",bReady="tNasti_io_tNasti_b_ready",
              egressRId="tNasti_io_egressResp_rBits_id",egressRLast="tNasti_io_egressResp_rBits_last",
              egressRData="tNasti_io_egressResp_rBits_data",egressRResp="2'd2",egressRUser="1'b1",
              egressBId="tNasti_io_egressResp_bBits_id",egressBResp="2'd1",egressBUser="1'b1")
    for ch in ("ar","aw"):
        for n,s in (("Valid","valid"),("Id","bits_id"),("Len","bits_len"),("Addr","bits_addr"),("Burst","bits_burst")):
            ni[ch+n]="tNasti_io_tNasti_"+ch+"_"+s
    for n in ("schedulerWindowSize","openPagePolicy","backendLatency","wayBits","setBits","blockBits","activeMSHRs"):
        ni[n]="tNasti_io_mmReg_"+("llc_" if n in {"wayBits","setBits","blockBits","activeMSHRs"} else "")+n
    for stem in ("bank","rank","row"):
        for n,s in (("Offset","offset"),("Mask","mask")):
            ni[stem+"Addr"+n]="tNasti_io_mmReg_"+stem+"Addr_"+s
    for n in "tAL tCAS tCMD tCWD tCCD tFAW tRAS tREFI tRC tRCD tRFC tRRD tRP tRTP tRTRS tWR tWTR".split():
        ni["timings_"+n]="tNasti_io_mmReg_dramTimings_"+n
    no = dict(arReady=1,awReady=1,wReady=1,rValid=1,rId=4,rLast=1,rData=64,rResp=2,rUser=1,
              bValid=1,bId=4,bResp=2,bUser=1,egressReadValid=1,egressReadId=4,
              egressWriteValid=1,egressWriteId=4,egressRReady=1,egressBReady=1,
              targetCycle=64,pendingReads=4 if capacity==10 else 5,
              pendingAWReq=4 if capacity==10 else 5,pendingWReq=4 if capacity==10 else 5,
              totalReads=32,totalWrites=32,totalReadBeats=32,totalWriteBeats=32)
    for i in range(5):
        no[f"readOutstandingHistogram_{i}"]=32
        no[f"writeOutstandingHistogram_{i}"]=32
    for i in range(4):
        for n in ("allPreCycles","numCASR","numCASW","numACT"):
            no[f"rankPower_{i}_{n}"]=32
    # Existing component comparisons are reused only as output assertions.
    # Their replay inputs and environments are deliberately not reused.
    checks = []
    for prefix, old in (("dram",controller_tb(cone,capacity)),("llc",llc_tb(cone))):
        for w,n in re.findall(r"wire \[(\d+):0\] native_(\w+);",old):
            no[prefix+"_"+n]=int(w)+1
        block=old.split("task compare; begin",1)[1].split("end endtask",1)[0]
        block=block.replace("native_","native_"+prefix+"_")
        if prefix=="llc":
            block=block.replace("sfc.","sfc.llc_model.")
            block=re.sub(r"(?<![\w.])io_(\w+)",r"sfc.llc_model.io_\1",block)
            # Emitted SFC ports use flat io_* names on the instance.
        checks.append(block)
    sv += [f"wire [{w-1}:0] native_{n};" for n,w in no.items()]
    sv += [f"GGFASEDFRFCFSTimingModel{capacity} native("+",".join([f".{n}({v})" for n,v in ni.items()]+[f".{n}(native_{n})" for n in no])+");"]
    sv += ["integer cycles=0;integer commands[0:5];integer acceptedAR=0,acceptedAW=0,respondedR=0,respondedB=0,readBeats=0,writeBeats=0;",
           "integer stalls=0,rStalls=0,bStalls=0,rReplace=0,bReplace=0,multiReads=0,multiWrites=0,hits=0,missesSeen=0,dirty=0;integer phaseMask=0,stateMask=0;",
           "task compare;begin"]

    def check(n,ref,guard="1'b1"):
        sv.append(f'if(({guard}) && native_{n} !== {ref})$fatal(1,"cycle=%0d field={n} native=%0h sfc=%0h",cycles,native_{n},{ref});')
    for ch in ("ar","aw","w"):
        check(ch+"Ready","tNasti_io_tNasti_"+ch+"_ready")
    for ch,fields in (("r",("Id","Last","Data")),("b",("Id",))):
        check(ch+"Valid","tNasti_io_tNasti_"+ch+"_valid")
        for n in fields:check(ch+n,"tNasti_io_tNasti_"+ch+"_bits_"+n.lower(),"native_"+ch+"Valid")
    for n,ref in dict(egressReadValid="r_valid",egressReadId="r_bits",egressWriteValid="b_valid",egressWriteId="b_bits").items():
        check(n,"tNasti_io_egressReq_"+ref,"native_egress"+("Read" if "Read" in n else "Write")+"Valid" if n.endswith("Id") else "1'b1")
    check("egressRReady","tNasti_io_egressResp_rReady")
    check("egressBReady","tNasti_io_egressResp_bReady")
    for n,ref in dict(pendingReads="SatUpDownCounter_io_value",pendingAWReq="SatUpDownCounter_1_io_value",pendingWReq="SatUpDownCounter_2_io_value",targetCycle="tCycle").items():
        check(n,"sfc."+ref)
    for n in ("totalReads","totalWrites","totalReadBeats","totalWriteBeats"):
        check(n,"tNasti_io_mmReg_"+n)
    for ch in ("read","write"):
        for i in range(4):check(f"{ch}OutstandingHistogram_{i}",f"tNasti_io_mmReg_{ch}OutstandingHistogram_{i}")
        check(f"{ch}OutstandingHistogram_4","32'd0")
    for i in range(4):
        for n in ("allPreCycles","numCASR","numCASW","numACT"):
            check(f"rankPower_{i}_{n}",f"tNasti_io_mmReg_rankPower_{i}_{n}")
    check("rResp","2'd2","native_rValid");check("rUser","1'b1","native_rValid")
    check("bResp","2'd1","native_bValid");check("bUser","1'b1","native_bValid")
    sv += checks
    sv += ["end endtask", "task step;begin #2;compare();",
           "if(!targetFire)stalls++;if(targetFire && !reset)begin commands[sfc.selectedCmd]++;stateMask|=1<<native_llc_state;",
           "hits+=sfc.llc_model.s1_valid && sfc.llc_model.hit_valid;missesSeen+=sfc.llc_model.s1_valid && !sfc.llc_model.hit_valid;dirty+=sfc.llc_model.s1_valid && sfc.llc_model.evict_dirty_way;",
           "rStalls+=native_rValid && !tNasti_io_tNasti_r_ready;bStalls+=native_bValid && !tNasti_io_tNasti_b_ready;",
           "rReplace+=native_rValid && tNasti_io_tNasti_r_ready && native_rLast && native_egressReadValid;",
           "bReplace+=native_bValid && tNasti_io_tNasti_b_ready && native_egressWriteValid;end",
           "clock=1;#2;compare();clock=0;#2;cycles++;end endtask",
           "bit readIDs[0:15],writeIDs[0:15];integer readLens[0:15];integer reads=0,writes=0;",
           "integer targetWLens[0:31];integer targetWEnq=0,targetWDeq=0;",
           "integer rIDs[0:31],rRemain[0:31],rBeat[0:31],bIDs[0:31];integer re=0,rd=0,be=0,bd=0;",
           "always_comb begin tNasti_io_egressResp_rBits_id=4'(rIDs[rd%32]);tNasti_io_egressResp_rBits_last=(rRemain[rd%32]==0);",
           "tNasti_io_egressResp_rBits_data={24'hc1ac73,4'(rIDs[rd%32]),4'(rBeat[rd%32]),32'h5000a00a};tNasti_io_egressResp_bBits_id=4'(bIDs[bd%32]);end",
           "integer i,j,k,phase,freeR,freeW,addrIndex,phaseStart;bit drain=0;bit takeAR,takeAW,takeW,takeR,takeB,reqR,reqB;integer savedRId,savedBId;",
           "task cycle;begin",
           "if(!tNasti_io_tNasti_w_valid && targetWEnq!=targetWDeq && (drain || $urandom_range(0,3)!=0))tNasti_io_tNasti_w_valid=1;",
           "tNasti_io_tNasti_w_bits_last=(targetWEnq!=targetWDeq && targetWLens[targetWDeq%32]==0);#1;",
           "takeAR=targetFire && !reset && tNasti_io_tNasti_ar_valid && tNasti_io_tNasti_ar_ready;",
           "takeAW=targetFire && !reset && tNasti_io_tNasti_aw_valid && tNasti_io_tNasti_aw_ready;",
           "takeW=targetFire && !reset && tNasti_io_tNasti_w_valid && tNasti_io_tNasti_w_ready;",
           "takeR=targetFire && !reset && tNasti_io_tNasti_r_valid && tNasti_io_tNasti_r_ready;",
           "takeB=targetFire && !reset && tNasti_io_tNasti_b_valid && tNasti_io_tNasti_b_ready;",
           "reqR=targetFire && !reset && tNasti_io_egressReq_r_valid;reqB=targetFire && !reset && tNasti_io_egressReq_b_valid;",
           "savedRId=tNasti_io_egressReq_r_bits;savedBId=tNasti_io_egressReq_b_bits;step();",
           "if(takeR)begin if(re==rd)$fatal(1,\"read egress underflow\");readBeats++;",
           "if(rRemain[rd%32]==0)begin k=rIDs[rd%32];if(!readIDs[k])$fatal(1,\"unsolicited R\");readIDs[k]=0;reads--;respondedR++;rd++;end",
           "else begin rRemain[rd%32]--;rBeat[rd%32]++;end end",
           "if(takeB)begin if(be==bd)$fatal(1,\"write egress underflow\");k=bIDs[bd%32];if(!writeIDs[k])$fatal(1,\"unsolicited B\");writeIDs[k]=0;writes--;respondedB++;bd++;end",
           "if(takeAR)begin k=tNasti_io_tNasti_ar_bits_id;if(readIDs[k])$fatal(1,\"read ID reused\");readIDs[k]=1;readLens[k]=tNasti_io_tNasti_ar_bits_len;reads++;acceptedAR++;multiReads+=(readLens[k]!=0);tNasti_io_tNasti_ar_valid=0;end",
           "if(takeAW)begin k=tNasti_io_tNasti_aw_bits_id;if(writeIDs[k])$fatal(1,\"write ID reused\");writeIDs[k]=1;writes++;acceptedAW++;multiWrites+=(tNasti_io_tNasti_aw_bits_len!=0);targetWLens[targetWEnq%32]=tNasti_io_tNasti_aw_bits_len;targetWEnq++;tNasti_io_tNasti_aw_valid=0;end",
           "if(takeW)begin writeBeats++;if(targetWLens[targetWDeq%32]==0)targetWDeq++;else targetWLens[targetWDeq%32]--;tNasti_io_tNasti_w_valid=0;end",
           "if(reqR)begin if(re-rd>=2)$fatal(1,\"functional read queue overflow\");rIDs[re%32]=savedRId;rRemain[re%32]=readLens[savedRId];rBeat[re%32]=0;re++;end",
           "if(reqB)begin if(be-bd>=2)$fatal(1,\"functional write queue overflow\");bIDs[be%32]=savedBId;be++;end end endtask",
           "task drain_all;integer n;begin drain=1;targetFire=1;tNasti_io_tNasti_r_ready=1;tNasti_io_tNasti_b_ready=1;",
           "for(n=0;n<12000;n++)begin cycle();if(reads==0 && writes==0 && !tNasti_io_tNasti_ar_valid && !tNasti_io_tNasti_aw_valid && targetWEnq==targetWDeq && native_llc_state==0 && native_llc_allocated==0)break;end",
           "if(n==12000)$fatal(1,\"incomplete drain R=%0d W=%0d LLC=%0d MSHR=%0d\",reads,writes,native_llc_state,native_llc_allocated);repeat(5)cycle();drain=0;end endtask",
           "initial begin for(i=0;i<6;i++)commands[i]=0;",
           "tNasti_io_mmReg_bankAddr_offset=6;tNasti_io_mmReg_bankAddr_mask=7;tNasti_io_mmReg_rankAddr_offset=9;tNasti_io_mmReg_rankAddr_mask=3;",
           "tNasti_io_mmReg_rowAddr_offset=11;tNasti_io_mmReg_rowAddr_mask=26'h3ffffff;",
           "tNasti_io_tNasti_ar_bits_burst=1;tNasti_io_tNasti_aw_bits_burst=1;"]
    defaults=[0,14,1,10,4,25,33,7800,47,14,160,8,14,8,2,15,8]
    for n,v in zip("tAL tCAS tCMD tCWD tCCD tFAW tRAS tREFI tRC tRCD tRFC tRRD tRP tRTP tRTRS tWR tWTR".split(),defaults):
        sv.append(f"tNasti_io_mmReg_dramTimings_{n}={v};")
    sv += ["for(phase=0;phase<16;phase++)begin",
           "if(phase!=0)drain_all();targetFire=0;reset=1;step();targetFire=1;step();reset=0;",
           "tNasti_io_mmReg_llc_wayBits=2'(phase%4);tNasti_io_mmReg_llc_setBits=4'(phase%3);tNasti_io_mmReg_llc_blockBits=3'(6+phase%2);tNasti_io_mmReg_llc_activeMSHRs=4'(1+phase%8);",
           "tNasti_io_mmReg_schedulerWindowSize=3'(phase);tNasti_io_mmReg_openPagePolicy=(phase<8);tNasti_io_mmReg_backendLatency=(phase==0)?1200:12;tNasti_io_mmReg_dramTimings_tREFI=(phase<8)?7800:600;",
           "phaseStart=acceptedAR+acceptedAW;",
           "for(i=0;i<3000;i++)begin targetFire=($urandom_range(0,4)!=0);",
           "tNasti_io_tNasti_r_ready=(i%600<500) && ($urandom_range(0,7)!=0);tNasti_io_tNasti_b_ready=(i%500<420) && ($urandom_range(0,7)!=0);",
           "freeR=-1;freeW=-1;for(j=15;j>=0;j--)begin if(!readIDs[j])freeR=j;if(!writeIDs[j])freeW=j;end",
           "if(!tNasti_io_tNasti_ar_valid && reads<8 && freeR>=0 && $urandom_range(0,7)==0)begin tNasti_io_tNasti_ar_valid=1;tNasti_io_tNasti_ar_bits_id=4'(freeR);tNasti_io_tNasti_ar_bits_len=8'($urandom_range(0,7));",
           "addrIndex=$urandom_range(0,23);tNasti_io_tNasti_ar_bits_addr=(35'(addrIndex)<<tNasti_io_mmReg_llc_blockBits)|(35'(phase%5)<<32);end",
           "if(!tNasti_io_tNasti_aw_valid && writes<8 && freeW>=0 && $urandom_range(0,7)==0)begin tNasti_io_tNasti_aw_valid=1;tNasti_io_tNasti_aw_bits_id=4'(freeW);tNasti_io_tNasti_aw_bits_len=8'($urandom_range(0,7));",
           "addrIndex=$urandom_range(0,23);tNasti_io_tNasti_aw_bits_addr=(35'(addrIndex)<<tNasti_io_mmReg_llc_blockBits)|(35'(phase%5)<<32);end cycle();end",
           "if(acceptedAR+acceptedAW>phaseStart)phaseMask|=1<<phase;end drain_all();",
           "for(i=1;i<6;i++)if(commands[i]==0)$fatal(1,\"missing DRAM command %0d\",i);",
           "if(phaseMask!=65535 || stateMask!=255)$fatal(1,\"missing phase/state coverage phases=%h states=%h\",phaseMask,stateMask);",
           "if(multiReads<100 || multiWrites<100 || hits<100 || missesSeen<100 || dirty<20 || rStalls<100 || bStalls<100 || rReplace==0)$fatal(1,\"insufficient burst/cache/releaser coverage mr=%0d mw=%0d hits=%0d miss=%0d dirty=%0d rst=%0d bst=%0d rr=%0d br=%0d\",multiReads,multiWrites,hits,missesSeen,dirty,rStalls,bStalls,rReplace,bReplace);",
           '$display("PASS FRFCFS timing-model differential cycles=%0d AR=%0d AW=%0d R=%0d B=%0d rBeats=%0d wBeats=%0d multiReads=%0d multiWrites=%0d hits=%0d misses=%0d dirty=%0d stalls=%0d rStalls=%0d bStalls=%0d rReplace=%0d bReplace=%0d phases=%h states=%h ACT=%0d PRE=%0d CASW=%0d CASR=%0d REF=%0d",cycles,acceptedAR,acceptedAW,respondedR,respondedB,readBeats,writeBeats,multiReads,multiWrites,hits,missesSeen,dirty,stalls,rStalls,bStalls,rReplace,bReplace,phaseMask,stateMask,commands[1],commands[2],commands[3],commands[4],commands[5]);$finish;end endmodule']
    return "\n".join(sv)+"\n"


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for n in ("fixture","firtool","verilator","output-dir"):
        parser.add_argument("--"+n,type=Path,required=True)
    parser.add_argument("--golden-receipt",type=Path,action="append",required=True)
    parser.add_argument("--jobs",type=int,default=4)
    a=parser.parse_args();d=a.output_dir.resolve();d.mkdir(parents=True,exist_ok=False)
    if a.jobs<1 or a.jobs>8:raise ValueError("component comparison supports 1..8 Verilator jobs")
    targets=[];cones={}
    for rp in a.golden_receipt:
        r=json.loads(rp.read_text());m=Path(r["manifest"]["path"])
        if r["status"]!="complete" or digest(m)!=r["manifest"]["sha256"]:
            raise ValueError("incomplete or changed SFC receipt")
        manifest=json.loads(m.read_text());source=m.parent/"sfc/FireSim-generated.sv"
        if manifest["status"]!="complete" or manifest["target"]!=r["target"] or digest(source)!=manifest["sfc_artifacts"][source.name]["sha256"]:
            raise ValueError("incomplete, mismatched or changed SFC manifest/RTL")
        cone,modules=closure(source.read_text(),"FirstReadyFCFSModel");sha=hashlib.sha256(cone.encode()).hexdigest()
        cones.setdefault(sha,(cone,modules,10 if r["target"]=="shuttle" else 20))
        targets.append(dict(target=r["target"],receipt=str(rp.resolve()),receipt_sha256=digest(rp),manifest=str(m),manifest_sha256=digest(m),sfc=str(source),sfc_sha256=digest(source),cone_sha256=sha))
    if {t["target"] for t in targets}!={"megaboomv3","shuttle","saturn_shuttle"} or len(targets)!=3:
        raise ValueError("require exactly the selected three DDR3 F2 receipts")
    report=dict(scope=__doc__,passed=False,targets=targets,cones=[],comparator_sha256=digest(Path(__file__)),tools={str(t.resolve()):digest(t) for t in (a.fixture,a.firtool,a.verilator)}, comparator_dependencies={n:digest(Path(__file__).with_name(n)) for n in ("FASEDRankStateTrackerDifferential.py","FASEDFRFCFSControllerDifferential.py","FASEDLLCModelDifferential.py")},
                assumptions=["Verilator zero initialization models programming-time metadata BRAM initialization; runtime reset preserves RAM", "at most eight reads/eight writes, one outstanding per channel/ID, ordered W beats after AW, incrementing bursts <=8 beats within blockBits6/7 cache lines", "functional response environment queues checked egress IDs, preserves read burst length, beat/data/ID under target stalls, and consumes only on valid-ready; it does not implement actual functional memory", "settings change after complete drain and reset; all sixteen runtime window-depth/page-policy phases exercised", "SFC clock gated by targetFire, native state/reset gated by same targetFire; unreset RR starts zero in both simulations", "SFC assertions disabled by SYNTHESIS; finite simulation is not formal proof"],
                normalizations=["invalid payload ignored; unreset metadata compared only where consumed", "SFC prunes response Resp/User fields: native pass-through checked against external constants, not independent golden fields", "fifth occupancy histogram element is undriven/dead in SFC and explicitly zero native; only four actual SFC bins compared", "SFC dead-eliminated controller/LLC len fields not compared; valid target burst len is exercised by external functional-egress adapter"],
                excluded=["actual functional target memory/data contents, host bridge/MMIO ABI, platform composition, full target semantic parity, Jasper LEC/SEC, F2 runtime and speed"])
    try:
        for sha,(cone,modules,capacity) in cones.items():
            out=d/sha[:12];out.mkdir();(out/"sfc-cone.sv").write_text(cone);(out/"tb.sv").write_text(testbench(cone,capacity))
            commands=[[str(a.fixture.resolve()),str(capacity),str(out/"native.mlir")],[str(a.firtool.resolve()),str(out/"native.mlir"),"--format=mlir","--verilog","-o",str(out/"native.sv")],[str(a.verilator.resolve()),"--binary","--timing","-DSYNTHESIS","--top-module","tb","-Wno-fatal","-j",str(a.jobs),"--Mdir",str(out/"obj"),str(out/"native.sv"),str(out/"sfc-cone.sv"),str(out/"tb.sv")],[str(out/"obj/Vtb")]]
            result=dict(cone_sha256=sha,modules=modules,commands=commands,passed=False);report["cones"].append(result)
            for i,cmd in enumerate(commands):
                with(out/f"stage-{i}.log").open("w") as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
            result["evidence"]=(out/"stage-3.log").read_text()
            if "PASS FRFCFS timing-model differential" not in result["evidence"]:raise ValueError("missing target PASS marker")
            result["passed"]=True;print(result["evidence"])
        for t in targets:
            for k in ("sfc","manifest","receipt"):
                if digest(Path(t[k]))!=t[k+"_sha256"]:raise ValueError("immutable SFC input changed during comparison")
            t["passed"]=True
        report["passed"]=True
    finally:
        report["artifacts"]={str(f.relative_to(d)):digest(f) for f in d.rglob("*") if f.is_file() and "obj" not in f.relative_to(d).parts}
        (d/"result.json").write_text(json.dumps(report,indent=2)+"\n")


if __name__=="__main__":main()
