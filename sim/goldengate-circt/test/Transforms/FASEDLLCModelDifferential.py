#!/usr/bin/env python3
"""Scoped cycle comparison of native LLC against immutable selected SFC LLCModel.

Both implementations receive the same legal target requests and independent
queued memory environment. No SFC internal value drives the native design.
This checks the LLC component, excluding the surrounding FRFCFS controller,
functional memory, MMIO, platform composition and formal proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from FASEDRankStateTrackerDifferential import closure, digest


def testbench(cone):
    model = re.search(r"(?ms)^module LLCModel\(.*?^\);", cone).group()
    ports = re.findall(r"\b(input|output)\s+(?:\[(\d+):0\]\s*)?(\w+)\s*[,)]?", model)
    inputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "input"}
    outputs = {n: int(w) + 1 if w else 1 for d, w, n in ports if d == "output"}
    sv = ["module tb;", "logic clock=0,targetFire=1;wire sfcClock=clock && targetFire;"]
    sv += [f"logic [{w-1}:0] {n}=0;" for n, w in inputs.items() if n != "clock"]
    sv += [f"wire [{w-1}:0] {n};" for n, w in outputs.items()]
    sv += ["LLCModel sfc(" + ",".join(f".{n}({'sfcClock' if n == 'clock' else n})" for _, _, n in ports) + ");"]
    ni = dict(clock="clock", reset="reset", targetFire="targetFire",
              wValid="io_req_w_valid",wLast="io_req_w_bits_last",
              rRespReady="io_rResp_ready",wRespReady="io_wResp_ready",
              memWReady="io_memReq_w_ready",memRValid="io_memRResp_valid",
              memRId="io_memRResp_bits_id",memBValid="io_memWResp_valid",
              memBId="io_memWResp_bits_id")
    for ch in ("ar", "aw"):
        for n, suffix in (("Valid","valid"),("Id","bits_id"),("Len","bits_len"),("Addr","bits_addr")):
            ni[ch+n]="io_req_"+ch+"_"+suffix
        ni["mem"+ch.capitalize()+"Ready"]="io_memReq_"+ch+"_ready"
    for n in ("wayBits","setBits","blockBits","activeMSHRs"):
        ni[n]="io_settings_"+n
    no = dict(arReady=1,awReady=1,wReady=1,rRespValid=1,rRespId=4,rRespLen=8,
              wRespValid=1,wRespId=4,memArValid=1,memArId=4,memArLen=8,memArAddr=35,
              memAwValid=1,memAwId=4,memAwLen=8,memAwAddr=35,memWValid=1,memWLast=1,
              memRReady=1,memBReady=1,misses=32,writebacks=32,refills=32,peakMSHRs=4,
              state=3,busy=8,hitValid=1,emptyValid=1,evictDirty=1,readStart=1,
              writeStart=1,refillStart=1,allocated=4,nextMshr=3,stage1Set=12,
              stage1Tag=35,lfsr=16,selectedWayOH=8)
    for i in range(8):
        for n,w in dict(tag=32,valid=1,dirty=1).items():no[f"metadata_{i}_{n}"]=w
        for n,w in dict(setAddr=12,id=4,len=8,isWrite=1,addr=35,writeInFlight=1,readInFlight=1,enabled=1).items():
            no[f"mshrEntries_{i}_{n}"]=w
    sv += [f"wire [{w-1}:0] native_{n};" for n,w in no.items()]
    sv += ["GGFASEDLLCModel native("+",".join([f".{n}({v})" for n,v in ni.items()]+[f".{n}(native_{n})" for n in no])+");"]
    sv += ["integer cycles=0,acceptedAR=0,acceptedAW=0,respondedR=0,respondedW=0;",
           "integer hits=0,missesSeen=0,dirtyEvictions=0,refillStarts=0,resourceHazards=0,setHazards=0,stalls=0,concurrent=0,shortWriteRefills=0,fullWriteMisses=0,longReadHits=0,highSetAccesses=0,highAddressRequests=0;",
           "integer stateCoverage=0,wayCoverage=0,setCoverage=0,blockCoverage=0,mshrCoverage=0;",
           "integer savedMisses=0,savedWritebacks=0,savedRefills=0;",
           "task compare; begin"]

    def check(n, ref, guard="1'b1"):
        sv.append(f'if(({guard}) && native_{n} !== {ref})$fatal(1,"cycle=%0d field={n} native=%0h sfc=%0h state=%0d",cycles,native_{n},{ref},sfc.state);')

    for ch in ("ar","aw","w"):check(ch+"Ready","io_req_"+ch+"_ready")
    for ch in ("r","w"):
        check(ch+"RespValid","io_"+ch+"Resp_valid")
        check(ch+"RespId","io_"+ch+"Resp_bits_id","native_"+ch+"RespValid")
    for ch in ("ar","aw"):
        nn="mem"+ch.capitalize()
        check(nn+"Valid","io_memReq_"+ch+"_valid")
        for n,suffix in (("Id","id"),("Addr","addr")):
            check(nn+n,"io_memReq_"+ch+"_bits_"+suffix,"native_"+nn+"Valid")
    check("memWValid","io_memReq_w_valid")
    check("memWLast","io_memReq_w_bits_last","native_memWValid")
    check("memRReady","io_memRResp_ready")
    check("memBReady","1'b1")
    for n,ref in dict(misses="misses",writebacks="writebacks",refills="refills",peakMSHRs="peakMSHRsUsed").items():
        check(n,"io_settings_"+ref)
    for n,ref in dict(state="state",busy="d_array_busy_io_current",hitValid="hit_valid",emptyValid="empty_valid",evictDirty="evict_dirty_way",readStart="read_start",writeStart="write_start",refillStart="refill_start",allocated="mshrs_allocated",nextMshr="mshr_next_idx",stage1Set="s1_set_addr",stage1Tag="s1_tag_addr",lfsr="lsfr").items():
        check(n,"sfc."+ref,"native_state==1 || native_state==4" if n in {"hitValid","emptyValid","evictDirty","stage1Set","stage1Tag"} else "1'b1")
    check("selectedWayOH","sfc._selected_way_OH_T_1[7:0]","native_state==1 || native_state==4")
    for i in range(8):
        for n in ("tag","valid","dirty"):
            check(f"metadata_{i}_{n}",f"sfc.md_array_{i}_{n}_s1_metadata_data","native_state==1 || native_state==4")
        for n,ref in dict(writeInFlight="wb_in_flight",readInFlight="acq_in_flight",enabled="enabled",setAddr="set_addr",id="xaction_id",isWrite="xaction_isWrite").items():
            check(f"mshrEntries_{i}_{n}",f"sfc.mshrs_{i}_{ref}",f"native_mshrEntries_{i}_enabled && (native_mshrEntries_{i}_writeInFlight || native_mshrEntries_{i}_readInFlight)" if n in {"setAddr","id","isWrite"} else "1'b1")
    sv += ["end endtask", "task step;begin #2;compare();",
           "if(!targetFire)stalls++;",
           "if(targetFire && !reset)begin stateCoverage|=(1<<sfc.state);",
           "if(sfc.s1_valid)begin hits+=sfc.hit_valid;missesSeen+=!sfc.hit_valid;dirtyEvictions+=sfc.evict_dirty_way;end",
           "shortWriteRefills+=sfc.write_triggered_refill;fullWriteMisses+=(sfc.state==4 && !sfc.hit_valid && !sfc.write_triggered_refill);longReadHits+=(sfc.state==1 && sfc.hit_valid && sfc.reads_q_io_deq_bits_len!=0);",
           "highSetAccesses+=(sfc.s1_valid && sfc.s1_set_addr==4095);highAddressRequests+=(io_req_ar_valid && io_req_ar_ready && io_req_ar_bits_addr[34]) || (io_req_aw_valid && io_req_aw_ready && io_req_aw_bits_addr[34]);",
           "refillStarts+=sfc.refill_start;resourceHazards+=sfc.miss_resource_hazard;setHazards+=sfc.read_set_collision || sfc.write_set_collision;",
           "concurrent+=io_req_ar_valid && io_req_aw_valid;end",
           "clock=1;#2;compare();clock=0;#2;cycles++;end endtask",
           # Legal independent environment. Both implementations see identical
           # inputs; oracle output consumption follows their checked equality.
           "integer tick=0;integer rDue[0:15],bDue[0:15];bit rPending[0:15],bPending[0:15];",
           "integer memAwIDs[0:31];integer memAwEnq=0,memAwDeq=0;",
           "integer targetWLens[0:31];integer targetWEnq=0,targetWDeq=0;",
           "bit readIDs[0:15],writeIDs[0:15];integer reads=0,writes=0;",
           "integer i,j,k,phase,chosenR,chosenB,freeR,freeW,addrIndex;integer doneAR,doneAW,doneW;bit drain=0;",
           "task prepare;begin",
           "io_memReq_ar_ready=drain || ($urandom_range(0,3)!=0);",
           "io_memReq_aw_ready=drain || ($urandom_range(0,3)!=0);",
           "io_memReq_w_ready=(memAwEnq!=memAwDeq) && (drain || ($urandom_range(0,3)!=0));",
           "io_rResp_ready=1;io_wResp_ready=1;",
           "if(!io_memRResp_valid)begin chosenR=-1;for(j=15;j>=0;j--)if(rPending[j] && rDue[j]<=tick)chosenR=j;",
           "if(chosenR>=0)begin io_memRResp_valid=1;io_memRResp_bits_id=4'(chosenR);end end",
           "io_memWResp_valid=0;chosenB=-1;for(j=15;j>=0;j--)if(bPending[j] && bDue[j]<=tick)chosenB=j;",
           "if(chosenB>=0)begin io_memWResp_valid=1;io_memWResp_bits_id=4'(chosenB);end",
           "if(!io_req_w_valid && targetWEnq!=targetWDeq && (drain || $urandom_range(0,3)!=0))io_req_w_valid=1;",
           "io_req_w_bits_last=(targetWEnq!=targetWDeq && targetWLens[targetWDeq%32]==0);",
           "end endtask",
           "task cycle;begin prepare();#1;doneAR=0;doneAW=0;doneW=0;",
           "if(targetFire && !reset)begin tick++;",
           "if(io_memReq_ar_valid && io_memReq_ar_ready)begin",
           "if(rPending[io_memReq_ar_bits_id])$fatal(1,\"memory read ID reused before completion\");",
           "rPending[io_memReq_ar_bits_id]=1;rDue[io_memReq_ar_bits_id]=tick+((phase%4==0)?300:$urandom_range(3,60));end",
           "if(io_memReq_aw_valid && io_memReq_aw_ready)begin memAwIDs[memAwEnq%32]=io_memReq_aw_bits_id;memAwEnq++;end",
           "if(io_memReq_w_valid && io_memReq_w_ready && io_memReq_w_bits_last)begin",
           "if(memAwEnq==memAwDeq)$fatal(1,\"write data before AW\");k=memAwIDs[memAwDeq%32];memAwDeq++;",
           "if(bPending[k])$fatal(1,\"memory write ID reused before completion\");bPending[k]=1;bDue[k]=tick+$urandom_range(2,50);end",
           "if(io_memRResp_valid && io_memRResp_ready)begin rPending[io_memRResp_bits_id]=0;end",
           "if(io_memWResp_valid)begin bPending[io_memWResp_bits_id]=0;end",
           "if(io_req_ar_valid && io_req_ar_ready)begin doneAR=1;acceptedAR++;reads++;",
           "if(readIDs[io_req_ar_bits_id])$fatal(1,\"target read ID reused\");readIDs[io_req_ar_bits_id]=1;end",
           "if(io_req_aw_valid && io_req_aw_ready)begin doneAW=1;acceptedAW++;writes++;",
           "if(writeIDs[io_req_aw_bits_id])$fatal(1,\"target write ID reused\");writeIDs[io_req_aw_bits_id]=1;targetWLens[targetWEnq%32]=io_req_aw_bits_len;targetWEnq++;end",
           "if(io_req_w_valid && io_req_w_ready)begin doneW=1;if(io_req_w_bits_last)targetWDeq++;else targetWLens[targetWDeq%32]--;end",
           "if(io_rResp_valid && io_rResp_ready)begin if(!readIDs[io_rResp_bits_id])$fatal(1,\"unsolicited target read\");readIDs[io_rResp_bits_id]=0;reads--;respondedR++;end",
           "if(io_wResp_valid && io_wResp_ready)begin if(!writeIDs[io_wResp_bits_id])$fatal(1,\"unsolicited target write\");writeIDs[io_wResp_bits_id]=0;writes--;respondedW++;end end",
           "step();if(doneAR)io_req_ar_valid=0;if(doneAW)io_req_aw_valid=0;if(doneW)io_req_w_valid=0;",
           "if(targetFire && !reset && io_memRResp_valid && !rPending[io_memRResp_bits_id])io_memRResp_valid=0;",
           "end endtask",
           "task drain_all;integer n;bit pending;begin drain=1;targetFire=1;",
           "for(n=0;n<8000;n++)begin cycle();pending=0;for(j=0;j<16;j++)pending|=rPending[j] || bPending[j];",
           "if(reads==0 && writes==0 && !pending && targetWEnq==targetWDeq && memAwEnq==memAwDeq && native_state==0 && native_allocated==0)break;end",
           "if(n==8000)$fatal(1,\"incomplete drain reads=%0d writes=%0d state=%0d allocated=%0d targetW=%0d memW=%0d\",reads,writes,native_state,native_allocated,targetWEnq-targetWDeq,memAwEnq-memAwDeq);",
           "repeat(5)cycle();drain=0;end endtask",
           "initial begin io_settings_wayBits=0;io_settings_setBits=0;io_settings_blockBits=3;io_settings_activeMSHRs=1;",
           "reset=1;step();reset=0;",
           # Preserve the oracle's one-beat-line corner: a refill sets busy=0
           # and state7 waits forever for busy=1. Exercise it, then recover by
           # reset rather than hiding the mismatch behind an impossible drain.
           "phase=0;io_req_ar_valid=1;io_req_ar_bits_id=0;io_req_ar_bits_addr=35'h400000000;io_req_ar_bits_len=0;",
           "repeat(700)cycle();if(native_state!=7 || native_busy!=0 || reads!=0)$fatal(1,\"missing one-beat refill corner\");",
           "targetFire=0;reset=1;step();targetFire=1;step();reset=0;blockCoverage|=1<<3;",
           "for(phase=0;phase<24;phase++)begin",
           "if(phase!=0)begin drain_all();savedMisses+=io_settings_misses;savedWritebacks+=io_settings_writebacks;savedRefills+=io_settings_refills;",
           "targetFire=0;reset=1;step();targetFire=1;step();reset=0;end",
           "io_settings_wayBits=2'(phase%4);io_settings_blockBits=3'(4+phase%4);io_settings_activeMSHRs=4'(1+phase%8);",
           "case(phase%6)0:io_settings_setBits=0;1:io_settings_setBits=1;2:io_settings_setBits=2;3:io_settings_setBits=5;4:io_settings_setBits=8;5:io_settings_setBits=12;endcase",
           "wayCoverage|=1<<io_settings_wayBits;setCoverage|=1<<io_settings_setBits;blockCoverage|=1<<io_settings_blockBits;mshrCoverage|=1<<io_settings_activeMSHRs;",
           "for(i=0;i<3000;i++)begin targetFire=($urandom_range(0,4)!=0);",
           "freeR=-1;freeW=-1;for(j=15;j>=0;j--)begin if(!readIDs[j])freeR=j;if(!writeIDs[j])freeW=j;end",
           "if(!io_req_ar_valid && reads<8 && freeR>=0 && $urandom_range(0,7)==0)begin",
           "io_req_ar_valid=1;io_req_ar_bits_id=4'(freeR);io_req_ar_bits_len=8'($urandom_range(0,(1<<(io_settings_blockBits-3))-1));",
           "addrIndex=$urandom_range(0,31);if(addrIndex==30)addrIndex=4095;else if(addrIndex==31)addrIndex=4096;io_req_ar_bits_addr=(35'(addrIndex) << io_settings_blockBits) | (35'(phase%5) << 32);end",
           "if(!io_req_aw_valid && writes<8 && freeW>=0 && $urandom_range(0,7)==0)begin",
           "io_req_aw_valid=1;io_req_aw_bits_id=4'(freeW);io_req_aw_bits_len=8'($urandom_range(0,(1<<(io_settings_blockBits-3))-1));",
           "addrIndex=$urandom_range(0,31);if(addrIndex==30)addrIndex=4095;else if(addrIndex==31)addrIndex=4096;io_req_aw_bits_addr=(35'(addrIndex) << io_settings_blockBits) | (35'(phase%5) << 32);end",
           "cycle();end end drain_all();",
           "if(stateCoverage!=255 || wayCoverage!=15 || blockCoverage!=248 || mshrCoverage!=510)$fatal(1,\"missing runtime/state coverage states=%h ways=%h blocks=%h mshrs=%h\",stateCoverage,wayCoverage,blockCoverage,mshrCoverage);",
           "if(hits<50 || missesSeen<50 || dirtyEvictions<10 || refillStarts<50 || resourceHazards<20 || setHazards<20 || stalls<100 || concurrent<100)$fatal(1,\"insufficient LLC behavior coverage\");",
           "if(shortWriteRefills==0 || fullWriteMisses==0 || longReadHits==0 || highSetAccesses==0 || highAddressRequests==0)$fatal(1,\"missing burst/high-set/high-address coverage\");",
           '$display("burst/address coverage partialWriteRefills=%0d fullWriteMisses=%0d multiBeatReadHits=%0d set4095Accesses=%0d addrBit34Requests=%0d",shortWriteRefills,fullWriteMisses,longReadHits,highSetAccesses,highAddressRequests);',
           '$display("PASS LLC differential cycles=%0d AR=%0d AW=%0d R=%0d B=%0d hits=%0d misses=%0d dirtyEvictions=%0d refills=%0d resourceHazards=%0d setHazards=%0d stalls=%0d concurrent=%0d stateMask=%h ways=%h sets=%h blocks=%h mshrs=%h",cycles,acceptedAR,acceptedAW,respondedR,respondedW,hits,missesSeen,dirtyEvictions,refillStarts,resourceHazards,setHazards,stalls,concurrent,stateCoverage,wayCoverage,setCoverage,blockCoverage,mshrCoverage);',
           "$finish;end endmodule"]
    return "\n".join(sv)+"\n"


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ("fixture","firtool","verilator","output-dir"):
        p.add_argument("--"+n,type=Path,required=True)
    p.add_argument("--golden-receipt",type=Path,action="append",required=True)
    p.add_argument("--jobs",type=int,default=4)
    a=p.parse_args();d=a.output_dir.resolve();d.mkdir(parents=True,exist_ok=False)
    targets=[];cones={}
    for rp in a.golden_receipt:
        r=json.loads(rp.read_text());m=Path(r["manifest"]["path"])
        if r["status"]!="complete" or digest(m)!=r["manifest"]["sha256"]:
            raise ValueError("incomplete or changed SFC receipt")
        manifest=json.loads(m.read_text());source=m.parent/"sfc/FireSim-generated.sv"
        if manifest["status"]!="complete" or manifest["target"]!=r["target"] or digest(source)!=manifest["sfc_artifacts"][source.name]["sha256"]:
            raise ValueError("incomplete, mismatched or changed SFC manifest/RTL")
        cone,modules=closure(source.read_text(),"LLCModel");sha=hashlib.sha256(cone.encode()).hexdigest()
        cones.setdefault(sha,(cone,modules))
        targets.append(dict(target=r["target"],receipt=str(rp.resolve()),receipt_sha256=digest(rp),manifest=str(m),manifest_sha256=digest(m),sfc=str(source),sfc_sha256=digest(source),cone_sha256=sha))
    if {t["target"] for t in targets}!={"megaboomv3","shuttle","saturn_shuttle"} or len(targets)!=3:
        raise ValueError("require exactly the three selected DDR3 F2 receipts")
    report=dict(scope=__doc__,passed=False,targets=targets,cones=[],comparator_sha256=digest(Path(__file__)),tools={str(t.resolve()):digest(t) for t in (a.fixture,a.firtool,a.verilator)},
                assumptions=["Verilator initializes all metadata BRAM cells to zero once at programming; metadata is preserved across runtime reset", "target bursts fit configured cache block, one outstanding request per channel/ID; W beats follow accepted AW order", "LLC response consumers remain ready: SFC checks readiness at admission and emits nonholding response pulses; backing-memory AR/AW/W remain independently backpressured", "memory read completions follow accepted AR, write completions follow accepted AW and W.last, with independent delays and backpressure", "targetFire gates SFC clock and native state/reset; mainstream settings change only after full drain and reset", "blockBits3 read-miss corner is compared for 700 cycles then reset after its response: SFC remains in refill state7 with busy0 and cannot drain; randomized draining profiles use blockBits4..7", "SFC assertions disabled by SYNTHESIS; this is finite simulation evidence, not proof"],
                normalizations=["invalid response and queue payload ignored", "SFC dead-field eliminated response len, backing request len/size/constants and memWResp.ready; native len outputs not compared to absent ports", "metadata and stage1 tag/set compared only in metadata-access states, where consumed; unreset data ignored before first lookup"],
                excluded=["surrounding timing controller, functional memory/releaser, MMIO/power, full target/platform parity and Jasper LEC/SEC"])
    try:
        for sha,(cone,modules) in cones.items():
            out=d/sha[:12];out.mkdir();(out/"sfc-cone.sv").write_text(cone);(out/"tb.sv").write_text(testbench(cone))
            commands=[[str(a.fixture.resolve()),str(out/"native.mlir")],[str(a.firtool.resolve()),str(out/"native.mlir"),"--format=mlir","--verilog","-o",str(out/"native.sv")],[str(a.verilator.resolve()),"--binary","--timing","-DSYNTHESIS","--top-module","tb","-Wno-fatal","-j",str(a.jobs),"--Mdir",str(out/"obj"),str(out/"native.sv"),str(out/"sfc-cone.sv"),str(out/"tb.sv")],[str(out/"obj/Vtb")]]
            result=dict(cone_sha256=sha,modules=modules,commands=commands,passed=False);report["cones"].append(result)
            for i,cmd in enumerate(commands):
                with(out/f"stage-{i}.log").open("w") as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
            result["evidence"]=(out/"stage-3.log").read_text()
            if "PASS LLC differential" not in result["evidence"]:raise ValueError("missing PASS marker")
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
