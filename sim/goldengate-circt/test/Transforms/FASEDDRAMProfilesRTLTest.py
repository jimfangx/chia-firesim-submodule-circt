#!/usr/bin/env python3
"""Lower both no-LLC pipelines and simulate FIFO command ordering and policies."""
import argparse
from pathlib import Path
import re
import subprocess


def fifo_testbench(rtl):
    header = re.search(r"(?ms)^module GGFASEDFCFSCommandScheduler\(.*?^\);", rtl).group()
    header = re.sub(r"//[^\n]*", "", header).split("(", 1)[1].rsplit(")", 1)[0]
    ports = []
    direction, width = None, 1
    for part in header.split(","):
        match = re.fullmatch(r"\s*(?:(input|output)\s*)?(?:\[(\d+):0\]\s*)?(\w+)\s*", part)
        if not match:
            raise ValueError(f"unsupported port declaration: {part}")
        if match[1]:
            direction, width = match[1], int(match[2]) + 1 if match[2] else 1
        ports.append((direction, width, match[3]))
    sv = ["module tb;"]
    sv += [f"{'logic' if d == 'input' else 'wire'} [{w-1}:0] {n}" +
           ("=0;" if d == "input" else ";") for d, w, n in ports]
    sv += ["GGFASEDFCFSCommandScheduler dut(" + ",".join(f".{n}({n})" for _, _, n in ports) + ");"]
    # An independent single-entry Queue(pipe=true, flow=false) scoreboard.
    sv += ["integer count=0, headId=0, headRow=0, headBank=0, headRank=0, headLen=0; bit headWrite=0;",
           "integer accepted=0, completed=0, stalls=0, replacements=0, commands[0:5], autoPres=0;",
           "bit rowHit, canRead, canWrite, canAct, canPre; integer expectedCmd;",
           "bit wantsRef, rankCanRead, rankCanWrite, rankCanAct, rankCanPre;",
           "bit bankActive, bankCanRead, bankCanWrite, bankCanAct, bankCanPre; integer openRow;"]
    # Drive checker rank/bank status from public observations; this tests FIFO
    # arbitration against the FIFOMAS Scala conditions, using real bank timers.
    sv += ["always_comb begin wantsRef=0;rankCanRead=0;rankCanWrite=0;rankCanAct=0;rankCanPre=0;",
           "bankActive=0;bankCanRead=0;bankCanWrite=0;bankCanAct=0;bankCanPre=0;openRow=0;",
           "case(headRank)"]
    for rank in range(4):
        stem = f"rankStati_{rank}_"
        sv += [f"{rank}: begin wantsRef={stem}wantREF;rankCanRead={stem}canCASR;rankCanWrite={stem}canCASW;",
               f"rankCanAct={stem}canACT;rankCanPre={stem}canPRE;case(headBank)"]
        for bank in range(8):
            stem = f"rankStati_{rank}_banks_{bank}_"
            sv += [f"{bank}:begin bankActive={stem}state;openRow={stem}openRow;",
                   f"bankCanRead={stem}canCASR;bankCanWrite={stem}canCASW;bankCanAct={stem}canACT;bankCanPre={stem}canPRE;end"]
        sv += ["endcase end"]
    sv += ["endcase end"]
    sv += [r'''
integer i, phase, steps=0;
bit takeIn, takeOut;
task step; begin
  #2;
  if(!reset) begin
    if(entries_0_valid !== (count==1)) $fatal(1,"FIFO occupancy mismatch");
    if(count==1 && (entries_0_bits_id!=headId || entries_0_bits_rowAddr!=headRow ||
       entries_0_bits_rankAddr!=headRank || entries_0_bits_bankAddr!=headBank))
      $fatal(1,"younger request passed FIFO head");
    rowHit=bankActive && openRow==headRow;
    canRead=count==1 && rowHit && !headWrite && bankCanRead && rankCanRead && !wantsRef && backendReadReady;
    canWrite=count==1 && rowHit && headWrite && bankCanWrite && rankCanWrite && !wantsRef && backendWriteReady;
    canAct=count==1 && bankCanAct && rankCanAct && !wantsRef;
    canPre=count==1 && !rowHit && bankCanPre && rankCanPre && openPagePolicy;
    expectedCmd=canRead?4:canWrite?3:canAct?1:canPre?2:0;
    // REF and refresh PRE have absolute priority. Check transaction commands
    // separately whenever refresh arbitration is inactive.
    if(selectedCmd!=5 && !(selectedCmd==2 && (count==0 || cmdRank!=headRank || cmdBank!=headBank || wantsRef)))
      if(selectedCmd!=expectedCmd) $fatal(1,"FIFO command priority differs cmd=%0d expected=%0d",selectedCmd,expectedCmd);
    if(newReadValid && (headWrite || newReadId!=headId || newReadLen!=headLen)) $fatal(1,"wrong read completion");
    if(newWriteValid && (!headWrite || newWriteId!=headId)) $fatal(1,"wrong write completion");
    if(autoPRE !== (!openPagePolicy && (newReadValid || newWriteValid))) $fatal(1,"wrong close-page autoPRE");
    if(nextReady !== (count==0 || newReadValid || newWriteValid)) $fatal(1,"FIFO pipe replacement readiness differs");
    if(readLatency != (((timings_tCAS+timings_tAL)&127)+backendLatency)%4096 || writeLatency!=1)
      $fatal(1,"wrong response latency");
  end
  takeIn=targetFire && nextValid && nextReady;
  takeOut=targetFire && (newReadValid || newWriteValid);
  if(targetFire && !reset) begin
    commands[selectedCmd]++;
    autoPres+=autoPRE;
    if(takeOut) begin
      if(count!=1) $fatal(1,"unsolicited completion");
      count=0;completed++;
    end
    if(takeIn) begin
      if(count!=0) $fatal(1,"FIFO overwrite");
      count=1;headId=nextBits_id;headRow=nextBits_rowAddr;
      headRank=nextBits_rankAddr;headBank=nextBits_bankAddr;
      headWrite=nextBits_isWrite;headLen=nextBits_len;accepted++;
    end
    if(takeIn && takeOut) replacements++;
  end
  if(!targetFire) stalls++;
  clock=1;#2;clock=0;#2;
  if(targetFire) begin
    if(reset) begin count=0;tCycle=0;end
    else tCycle=tCycle+1;
  end
  steps++;
end endtask
initial begin
  for(i=0;i<6;i++) commands[i]=0;
  timings_tAL=0;timings_tCAS=14;timings_tCMD=1;timings_tCWD=10;
  timings_tCCD=4;timings_tFAW=25;timings_tRAS=33;timings_tREFI=1000;
  timings_tRC=47;timings_tRCD=14;timings_tRFC=160;timings_tRRD=8;
  timings_tRP=14;timings_tRTP=8;timings_tRTRS=2;timings_tWR=15;timings_tWTR=8;
  backendLatency=2;ranksInUse=15;schedulerWindowSize=0;
  reset=1;targetFire=1;step();reset=0;
  for(phase=0;phase<2;phase++) begin
    openPagePolicy=(phase==0);
    for(i=0;i<5000;i++) begin
      targetFire=($urandom_range(0,4)!=0);
      backendReadReady=($urandom_range(0,4)!=0);
      backendWriteReady=($urandom_range(0,4)!=0);
      // Hold an input stable until accepted, including host-clock stalls.
      if(!nextValid) begin
        nextValid=1;nextBits_id=4'(accepted);nextBits_len=8'(accepted%8);
        nextBits_isWrite=(accepted%3==0);nextBits_rowAddr=26'(accepted%5);
        nextBits_rankAddr=2'(accepted%4);nextBits_bankAddr=3'((accepted/4)%8);
        nextBits_rankAddrOH=4'(1<<nextBits_rankAddr);nextBits_bankAddrOH=8'(1<<nextBits_bankAddr);
      end
      step();if(takeIn)nextValid=0;
    end
    nextValid=0;backendReadReady=1;backendWriteReady=1;targetFire=1;
    for(i=0;i<2000 && count!=0;i++)step();
    if(count!=0)$fatal(1,"FIFO failed to drain");
  end
  if(accepted!=completed || completed<50 || stalls<100 || replacements<10 || autoPres<10)
    $fatal(1,"insufficient traffic count=%0d/%0d replacements=%0d autoPRE=%0d",accepted,completed,replacements,autoPres);
  for(i=1;i<6;i++)if(commands[i]==0)$fatal(1,"missing command coverage %0d",i);
  $display("PASS FIFO DDR3 ordering, both page policies, refresh, backend and targetFire stalls: %0d requests, %0d replacements, %0d cycles",completed,replacements,steps);
  $finish;
end
endmodule
''']
    return "\n".join(sv)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("fixture", "firtool", "verilator", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(command, name):
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(list(map(str, command)), check=True, stdout=log,
                           stderr=subprocess.STDOUT, timeout=240)

    cases = [("fcfs", 64, 8, 2, 20), ("frfcfs", 64, 8, 2, 20)]
    cases += [(profile, 256, 2, 15, 232) for profile in ("fcfs", "frfcfs", "frfcfs-llc")]
    cases += [("frfcfs-llc", width, 64 // (width // 8), 15, 232) for width in (128, 512)]
    for profile, width, beats, reuse, flight in cases:
        name = profile if width == 64 else f"{profile}-{width}"
        mlir, rtl = out / (name + ".mlir"), out / (name + ".sv")
        run([args.fixture.resolve(), "--fixture", str(flight), mlir, profile,
             str(width), str(beats), str(reuse)], name + "-fixture")
        run([args.firtool.resolve(), mlir, "--format=mlir", "--verilog", "-o", rtl], name + "-lower")
        run([args.verilator.resolve(), "--lint-only", "--timing", "-Wno-fatal",
             "--top-module", "GGFASEDBridgeControlWrapper", rtl], name + "-lint")
    tb = out / "tb.sv"
    tb.write_text(fifo_testbench((out / "fcfs.sv").read_text()))
    run([args.verilator.resolve(), "--binary", "--timing", "--assert", "-Wno-fatal", "-j", "4",
         "--top-module", "tb", "--Mdir", out / "obj", out / "fcfs.sv", tb], "fifo-build")
    run([out / "obj/Vtb"], "fifo-run")
    print((out / "fifo-run.log").read_text())


if __name__ == "__main__":
    main()
