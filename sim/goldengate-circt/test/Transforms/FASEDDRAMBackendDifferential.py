#!/usr/bin/env python3
"""Scoped DDR3 backend replay against immutable selected SFC dependency cones.

SFC dead-field elimination removes read length; selected write latency is1 and
completedWrite is always ready. Those fields are fixed/excluded only here. The
native helper's retained generic fields receive independent C++ IR testing.
"""
import argparse
import json
from pathlib import Path
import subprocess
from FASEDRankStateTrackerDifferential import closure, digest


def testbench():
    return r"""module tb;
logic clock=0;wire sfcClock=clock&&targetFire;
logic reset=0,targetFire=1,newReadValid=0,newWriteValid=0,completedReadReady=0;
logic [3:0] newReadID=0,newWriteID=0;
logic [7:0] newReadLen=0;
logic [11:0] readLatency=1,tCycle=0;
wire nr,nw,rv,wv,sr,sw,srv,swv;
wire [3:0] rid,wid,srid,swid;wire [7:0] rlen;wire [2:0] readCount,writeCount;
GGFASEDDRAMBackend native(.clock(clock),.reset(reset),.targetFire(targetFire),
.newReadValid(newReadValid),.newReadReady(nr),.newReadID(newReadID),.newReadLen(newReadLen),
.newWriteValid(newWriteValid),.newWriteReady(nw),.newWriteID(newWriteID),
.completedReadValid(rv),.completedReadReady(completedReadReady),.completedReadID(rid),.completedReadLen(rlen),
.completedWriteValid(wv),.completedWriteReady(1'b1),.completedWriteID(wid),
.readLatency(readLatency),.writeLatency(12'd1),.tCycle(tCycle),.readCount(readCount),.writeCount(writeCount));
DRAMBackend sfc(.clock(sfcClock),.reset(reset),.io_newRead_valid(newReadValid),.io_newRead_ready(sr),
.io_newRead_bits_id(newReadID),.io_newWrite_valid(newWriteValid),.io_newWrite_ready(sw),.io_newWrite_bits_id(newWriteID),
.io_completedRead_ready(completedReadReady),.io_completedRead_valid(srv),.io_completedRead_bits_id(srid),
.io_completedWrite_valid(swv),.io_completedWrite_bits_id(swid),.io_readLatency(readLatency),.io_tCycle(tCycle));
integer cycles=0,reads=0,writes=0,completes=0,fullRejects=0,fullPops=0,simultaneous=0,stalls=0,resets=0,wraps=0;
task compare;begin
if(nr!==sr||nw!==sw||rv!==srv||wv!==swv)$fatal(1,"cycle=%0d handshake differs native=%b%b%b%b sfc=%b%b%b%b",cycles,nr,nw,rv,wv,sr,sw,srv,swv);
if(rv&&rid!==srid)$fatal(1,"cycle=%0d read ID differs",cycles);
if(wv&&wid!==swid)$fatal(1,"cycle=%0d write ID differs",cycles);
end endtask
task step;begin
#2;compare();
if(targetFire&&!reset)begin
reads+=int'(nr&&newReadValid);writes+=int'(nw&&newWriteValid);completes+=int'(rv&&completedReadReady);
fullRejects+=int'(!nr&&newReadValid);fullPops+=int'(!nr&&newReadValid&&rv&&completedReadReady);
simultaneous+=int'(nr&&newReadValid&&rv&&completedReadReady);
end
stalls+=int'(!targetFire);resets+=int'(reset);clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
task clear;begin reset=1;targetFire=1;newReadValid=0;newWriteValid=0;completedReadReady=0;step();reset=0;end endtask
integer i,j;logic[11:0]oldCycle;
initial begin
clear();
// Four arrivals cannot flow through; a full pop must reject a fifth arrival.
readLatency=1;newReadValid=1;newWriteValid=1;
for(i=0;i<4;i++)begin tCycle=12'(i);newReadID=4'(i);newWriteID=4'(15-i);step();end
completedReadReady=1;tCycle=4;step();newReadValid=0;newWriteValid=0;
for(i=5;i<10;i++)begin tCycle=12'(i);step();end
// Later request matures off-head and must stay mature after its deadline.
clear();tCycle=100;readLatency=20;newReadValid=1;newReadID=3;step();
tCycle=101;readLatency=2;newReadID=9;step();newReadValid=0;tCycle=103;step();
tCycle=120;completedReadReady=1;step();tCycle=121;step();
// Equality skipped at wrap is not >=; later exact equality releases it.
clear();tCycle=4090;readLatency=10;newReadValid=1;newReadID=11;step();
newReadValid=0;tCycle=3;step();tCycle=5;step();if(rv)$fatal(1,"skipped equality released early");
tCycle=4;step();tCycle=5;completedReadReady=1;step();wraps++;
// Expiring unoccupied slot and enqueue target the same slot: enqueue wins.
clear();tCycle=0;readLatency=9;newReadValid=1;step();clear();
tCycle=9;readLatency=7;newReadValid=1;step();newReadValid=0;tCycle=10;step();
if(rv)$fatal(1,"expiry overrode enqueue pending bit");
// Dense short-latency traffic exercises all pointers and full/simultaneous edges.
for(j=1;j<33;j++)begin
clear();readLatency=12'(j);
for(i=0;i<500;i++)begin
newReadValid=(i%3!=0);newWriteValid=(i%5!=0);completedReadReady=(i%4!=0);
newReadID=4'($urandom);newWriteID=4'($urandom);newReadLen=8'($urandom);
targetFire=(i%7!=0);reset=(i%197==0);tCycle=12'(4090+i);step();
end end
// Arbitrary cycle jumps preserve exact modulo semantics, including large latencies.
clear();for(i=0;i<40000;i++)begin
newReadValid=1'($urandom);newWriteValid=1'($urandom);completedReadReady=1'($urandom);
newReadID=4'($urandom);newWriteID=4'($urandom);newReadLen=8'($urandom);
readLatency=12'($urandom_range(1,4095));targetFire=($urandom_range(0,4)!=0);reset=($urandom_range(0,127)==0);
oldCycle=tCycle;if(i%97==0)tCycle=12'($urandom);else tCycle=tCycle+1;
if(tCycle<oldCycle)wraps++;step();
end
if(reads==0||writes==0||completes==0||fullRejects==0||fullPops==0||simultaneous==0||stalls==0||resets==0||wraps==0)$fatal(1,"missing coverage");
$display("PASS DRAM backend differential cycles=%0d reads=%0d writes=%0d completes=%0d fullRejects=%0d fullPops=%0d simultaneous=%0d stalls=%0d resets=%0d wraps=%0d",cycles,reads,writes,completes,fullRejects,fullPops,simultaneous,stalls,resets,wraps);$finish;
end endmodule
"""

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
    cone, module_names = closure(sfc.read_text(), "DRAMBackend")
    (d / "sfc-cone.sv").write_text(cone)
    (d / "tb.sv").write_text(testbench())
    commands = [
        [str(a.fixture.resolve()), str(d / "native.mlir")],
        [str(a.firtool.resolve()), str(d / "native.mlir"), "--format=mlir", "--verilog", "-o", str(d / "native.sv")],
        [str(a.verilator.resolve()), "--binary", "--timing", "-DSYNTHESIS", "--top-module", "tb", "-Wno-fatal", "-j", str(a.jobs),
         "--Mdir", str(d / "obj"), str(d / "native.sv"), str(d / "sfc-cone.sv"), str(d / "tb.sv")],
        [str(d / "obj/Vtb")],
    ]
    report = {"scope": "DRAMBackend dependency cone only; SFC clock gated by targetFire; write latency1 and always-ready completion match selected SFC specialization; read len eliminated by SFC excluded; compare readiness, validity and valid IDs; not full-target parity or formal proof",
              "sfc": str(sfc.resolve()), "sfc_sha256": original, "modules": module_names,
              "commands": commands, "target": receipt["target"],
              "manifest_sha256": digest(manifest_path),
              "tools": {str(f.resolve()): digest(f) for f in [a.fixture, a.firtool, a.verilator]},
              "passed": False}
    try:
        for i, command in enumerate(commands):
            with (d / f"stage-{i}.log").open("w") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        if "PASS DRAM backend differential" not in (d / "stage-3.log").read_text():
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
