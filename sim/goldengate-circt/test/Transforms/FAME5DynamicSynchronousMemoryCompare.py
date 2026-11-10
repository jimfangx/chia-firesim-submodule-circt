#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the actual SFC/CIRCT dynamic grouped RTL using the AMD BUFGCE model.

Both sides receive identical protocol-valid public-port traffic. The unreset
selector starts aligned and the edge toggle/tracker have arbitrary concrete Boolean startup (all four
combinations are tested). RAM and read/tag pipelines are never initialized. Response
checks require a prior masked write to the exact thread/address followed by an
enabled target read. Old-data collisions and all controls remain strict.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

LEDGER = r"""`timescale 1ns/1ps
module ledger;
localparam integer N=NUM;
reg hostClock=0,hostReset=1;
reg [N-1:0] in_valid='1,out_ready='1;
wire [N-1:0] in_ready,out_valid;
reg [7:0] in_bits[0:N-1];
wire [7:0] out_bits[0:N-1];
Top dut(PORTS);
reg enabled[0:N-1],fired[0:N-1],defined[0:N-1];
reg [7:0] state[0:N-1];
reg [7:0] lastRead[0:N-1];
integer collisions=0,disabledReads=0;
integer seq[0:N-1];
integer cycle,i,sel,slot,addr,tx=0,outs=0,stalls=0,resets=0,masked=0,writes=0;
reg [7:0] memory[0:N-1][0:3];
reg written[0:N-1][0:3];
reg [7:0] observed;
reg [511:0] control;
integer edgePhase;
reg finishing,ce,handshake[0:N-1],expected_valid;
reg [63:0] checksum=0;
task tick;
 begin #5;hostClock=1;#5;hostClock=0;#1;end
endtask
initial begin
 #200; // Wait for the vendor global startup reset to deassert.
 // Explicit SFC free-running selector startup-phase relation. This initializes
 // only helper scheduling state, never target or memory contents.
 dut.Worker_threaded.threadIdx=0;
 edgePhase=0;if($value$plusargs("edge-phase=%d",edgePhase))begin end
 HELPER_EDGE=edgePhase & 1;
 HELPER_TRACKER=(edgePhase >> 1) & 1;
 control=0; if($value$plusargs("control=%s",control))begin end
 for(i=0;i<N;i=i+1) begin
  for(addr=0;addr<4;addr=addr+1)written[i][addr]=0;
  in_bits[i]=128;enabled[i]=0;fired[i]=0;defined[i]=0;state[i]=0;seq[i]=0;
 end
 repeat(N) tick();
 hostReset=0;
 for(cycle=0;cycle<16384;cycle=cycle+1) begin
  if(cycle==4096) begin
   hostReset=1;in_valid='1;out_ready='1;
   for(i=0;i<N;i=i+1)in_bits[i]=128;
   repeat(N)tick();hostReset=0;
   for(i=0;i<N;i=i+1)begin enabled[i]=0;fired[i]=0;seq[i]=0;defined[i]=0;end
  end
  for(i=0;i<N;i=i+1)begin
   // Only introduce a new payload when the prior irrevocable token fired.
   // Once asserted, valid/payload remain held through arbitrary independent stalls.
   if(cycle==0 || cycle==4096 || handshake[i])begin
    seq[i]=seq[i]+1;
    if(MEMORY)in_bits[i]=(seq[i]*17+i*29)%256;
    else in_bits[i]=(cycle<64 || cycle%97<N)?128:((seq[i]*17+i*29)%127);
    in_valid[i]=(cycle<64 || (cycle*3+i*7)%11!=0);
   end else if(!in_valid[i] && (cycle*3+i*7)%11!=0)in_valid[i]=1;
   out_ready[i]=(cycle<64 || (cycle/(i+1)+i)%7!=0);
  end
  #1;
  slot=cycle%N;sel=(slot==0)?1:(slot==1)?0:5-slot;
  finishing=(fired[sel] || out_ready[sel]) && in_valid[sel];
  ce=enabled[sel] && finishing;
  for(i=0;i<N;i=i+1)begin
   expected_valid=(i==sel) && !fired[sel];
   if((in_ready[i] ^ (control=="corrupt-ready" && cycle==100 && i==sel)) !== ((i==sel) && finishing) ||
      (out_valid[i] ^ (control=="corrupt-valid" && cycle==100 && i==sel)) !== expected_valid)begin
    $display("CONTROL_FAIL cycle=%0d thread=%0d ready=%b valid=%b expectedValid=%b",cycle,i,in_ready[i],out_valid[i],expected_valid);$fatal;
   end
   checksum=checksum*131+in_ready[i];checksum=checksum*131+out_valid[i];
   addr=in_bits[i][1:0];
   observed=out_bits[i];
   if(control=="different-invalid-payload" && (!out_valid[i] || !defined[i])) observed=observed^8'ha5;
   if(out_valid[i] && defined[i]) begin
    if(control=="corrupt-defined-payload" && cycle>=64)observed=observed^8'h1;
    if(observed !== lastRead[i])begin
     $display("STATE_FAIL cycle=%0d thread=%0d actual=%0d expected=%0d ce=%0d",cycle,i,observed,lastRead[i],ce);$fatal;
    end
    checksum=checksum*131+observed;outs=outs+1;
   end
   handshake[i]=in_valid[i] && in_ready[i];
  end
  if(!ce)defined[sel]=0;
  if(ce && MEMORY)begin
   addr=in_bits[sel][1:0];
   if(!in_bits[sel][5])disabledReads=disabledReads+1;
   if(in_bits[sel][5] && in_bits[sel][7] && in_bits[sel][6])collisions=collisions+1;
   defined[sel]=in_bits[sel][5] && written[sel][addr];
   if(defined[sel])lastRead[sel]=memory[sel][addr];
   if(in_bits[sel][7] && in_bits[sel][6])begin
    memory[sel][addr]={3'b0,in_bits[sel][4:0]};written[sel][addr]=1;writes=writes+1;
   end else if(in_bits[sel][7])masked=masked+1;
  end
  if(ce && !MEMORY)begin
   if(in_bits[sel][7])begin state[sel]=0;defined[sel]=1;resets=resets+1;end
   else if(defined[sel]) state[sel]=(state[sel]+in_bits[sel][6:0])&255;
  end
  if(finishing)tx=tx+1;else stalls=stalls+1;
  enabled[sel]=enabled[sel] || finishing;
  fired[sel]=finishing?0:(fired[sel] || out_ready[sel]);
  tick();
 end
 for(i=0;i<N;i=i+1)for(addr=0;addr<4;addr=addr+1)
  if(!written[i][addr])begin $display("UNEXERCISED_WORD thread=%0d address=%0d",i,addr);$fatal;end
 if(collisions==0 || disabledReads==0)$fatal;
 if(tx<1000 || outs<1000 || stalls==0 || (MEMORY ? (writes==0 || masked==0) : resets==0))$fatal;
 $display("F2_DYNAMIC_LEDGER N=%0d memory=MEMORY tx=%0d outputs=%0d stalls=%0d resets=%0d checksum=%0d writes=%0d masked=%0d collisions=%0d disabledReads=%0d",N,tx,outs,stalls,resets,checksum,writes,masked,collisions,disabledReads);
 $finish;
end
endmodule
"""

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--sfc-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--nested-signed", action="store_true", help="exercise the nested Branch/Leaf signed synchronous memory fixture")
    parser.add_argument("--xilinx-root", type=Path, default=Path("/opt/Xilinx/2025.2/Vivado"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    receipt = {"supporting_test_sha256": digest(__file__), "oracle_hashes": {str(p): digest(p) for p in (Path(__file__).with_name("FAME5DynamicSynchronousMemoryOracle.scala"),) if p.is_file()}, "scope": "bounded 2/4-thread canonical FAME5 virtual controller with synchronous target RAM; explicit Xilinx host specialization" + ("; nested Branch/Leaf SInt memory and signed response pipeline" if args.nested_signed else ""), "commands": [], "cases": [], "compiler_sha256": digest(args.compiler), "assumptions": ["model-local unreset selector starts at zero on both sides, aligned with top reset selector after a whole thread rotation", "all four independent concrete Boolean startup combinations for unreset memory edgeCount/edgeCountTracker are checked, followed by whole-rotation host reset; no target/read-data/tag/RAM initialization", "input valid and payload held until accepted", "real AMD BUFGCE default synchronous low-phase CE semantics", "no target state or RAM preclear; only written RAM locations and target-reset-defined registers compared"], "full_fame5": False}
    def run(command, cwd, log):
        receipt["commands"].append({"argv": list(map(str, command)), "cwd": str(cwd), "log": str(log)})
        with log.open("w") as stream:
            result = subprocess.run(list(map(str, command)), cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
        return result.returncode, log.read_text()
    for memory in (True,):
        for n in (2, 4):
            oracle = args.sfc_root / f"sfc-sync{n}"
            case = args.output_dir / f"{'memory' if memory else 'register'}{n}"
            case.mkdir(exist_ok=True)
            native = case / "native"
            native.mkdir(exist_ok=True)
            command = [args.compiler, oracle / "input.fir", "--annotation-file", oracle / "input.anno.json", "--output-dir", native, "--multi-thread-fame5-models-xilinx"]
            code, _ = run(command, case, case / "compiler.log")
            assert code == 0, str(case / "compiler.log")
            if args.nested_signed:
                native_ir = (native / "post-fame5-models.mlir").read_text()
                assert native_ir.count("goldengate.fame5.sharedThreads = " + str(n) + " : i32") == 3
                assert "@Leaf_threaded" in native_ir and "@Branch_threaded" in native_ir
                assert "data flip: sint<8>" in native_ir and "goldengate.fame5.responseBuffer" in native_ir
            annotations = json.loads((native / "post-fame5-models-all.json").read_text())
            oracle_annotations = json.loads((oracle / "threaded.sfc.json").read_text())
            annotation_relation = "exact retained annotations"
            if memory:
                # SFC's ThreadedMem wrapper requests downstream inlining. Native
                # banking directly rewrites the memory in the shared model, so
                # no helper module or corresponding inline target exists.
                inline = {"class": "firrtl.passes.InlineAnnotation", "target": "Top.ThreadedMem"}
                assert oracle_annotations.count(inline) == 1
                assert "module ThreadedMem :" in (oracle / "threaded.sfc.fir").read_text()
                assert "ThreadedMem" not in (native / "post-fame5-models.mlir").read_text()
                oracle_annotations.remove(inline)
                annotation_relation = "exact retained annotations; SFC Top.ThreadedMem InlineAnnotation discharged by native direct memory banking"
            assert annotations == oracle_annotations, "annotation mismatch"
            results = []
            for side, rtl in (("sfc", oracle / "threaded.sfc.v"), ("circt", native / "post-fame5-models.sv")):
                directory = case / side
                directory.mkdir(exist_ok=True)
                ports = [".hostClock(hostClock)", ".hostReset(hostReset)"]
                for i in range(n):
                    ports += [f".{a}{i}_{b}({a}_{b}[{i}])" for a in ("in", "out") for b in ("valid", "ready", "bits")]
                ledger = LEDGER.replace("NUM", str(n)).replace("PORTS", ",\n".join(ports)).replace("MEMORY", str(int(memory)))
                scope = "dut.Worker_threaded." + ("branch.leaf." if args.nested_signed else "")
                helper = scope + ("store." if side == "sfc" else "store_")
                if args.nested_signed:
                    ledger = ledger.replace("dut.Worker_threaded.threadIdx=0;", "dut.Worker_threaded.threadIdx=0;\n dut.Worker_threaded.branch.leaf.threadIdx=0;")
                    ledger = ledger.replace("{3'b0,in_bits[sel][4:0]}", "{{3{in_bits[sel][4]}},in_bits[sel][4:0]}")
                    ledger = ledger.replace("integer collisions=0,disabledReads=0;", "integer collisions=0,disabledReads=0,negativeWrites=0;")
                    ledger = ledger.replace("writes=writes+1;", "writes=writes+1;if(in_bits[sel][4])negativeWrites=negativeWrites+1;")
                    ledger = ledger.replace("if(collisions==0 || disabledReads==0)", "if(negativeWrites==0 || collisions==0 || disabledReads==0)")
                    ledger = ledger.replace('disabledReads=%0d",N', 'disabledReads=%0d negativeWrites=%0d",N')
                    ledger = ledger.replace(",collisions,disabledReads);", ",collisions,disabledReads,negativeWrites);")
                ledger = ledger.replace("HELPER_EDGE", helper + "edgeCount").replace("HELPER_TRACKER", helper + "edgeCountTracker")
                (directory / "ledger.sv").write_text(ledger)
                code, _ = run([args.xilinx_root / "bin/xvlog", "-sv", rtl, directory / "ledger.sv", args.xilinx_root / "data/verilog/src/unisims/BUFGCE.v", args.xilinx_root / "data/verilog/src/glbl.v"], directory, directory / "syntax.log")
                assert code == 0, str(directory / "syntax.log")
                code, _ = run([args.xilinx_root / "bin/xelab", "--timescale", "1ns/1ps", "--mt", "off", "--debug", "typical", "-s", "ledger_snapshot", "work.ledger", "work.glbl"], directory, directory / "elaborate.log")
                assert code == 0, str(directory / "elaborate.log")
                outcomes = {}
                for control in ("none", "edge-phase-1", "edge-phase-2", "edge-phase-3", "different-invalid-payload", "corrupt-ready", "corrupt-valid", "corrupt-defined-payload"):
                    command = [args.xilinx_root / "bin/xsim", "ledger_snapshot", "-runall"]
                    if control.startswith("edge-phase-"): command += ["-testplusarg", "edge-phase=" + control[-1]]
                    elif control != "none": command += ["-testplusarg", "control=" + control]
                    code, output = run(command, directory, directory / (control + ".log"))
                    success = re.search(r"F2_DYNAMIC_LEDGER.*", output)
                    if control in ("none", "edge-phase-1", "edge-phase-2", "edge-phase-3", "different-invalid-payload"):
                        assert code == 0 and success, str(directory / (control + ".log"))
                        outcomes[control] = success.group()
                    else:
                        assert not success and ("CONTROL_FAIL" in output or "STATE_FAIL" in output), "negative control escaped: " + control
                        outcomes[control] = "rejected"
                assert all(outcomes["none"] == outcomes["edge-phase-" + str(phase)] for phase in (1, 2, 3)), "edge helper startup changed qualified response schedule"
                assert outcomes["none"] == outcomes["different-invalid-payload"], "invalid payload changed qualified checksum"
                results.append({"side": side, "rtl": str(rtl), "rtl_sha256": digest(rtl), "results": outcomes})
            assert results[0]["results"]["none"] == results[1]["results"]["none"], "SFC/CIRCT trace mismatch"
            mask_control = None
            if memory:
                fault_controls = []
                for fault in (("wrong-mask", "wrong-sign-extension") if args.nested_signed else ("wrong-mask",)):
                    # A real RTL fault control: compile the identical model with its
                    # target write mask forced high, then replay the same ledger.
                    negative = case / fault
                    negative.mkdir(exist_ok=True)
                    source = (oracle / "input.fir").read_text()
                    if fault == "wrong-mask":
                        original = "store.w.mask <= bits(" + ("data" if args.nested_signed else "input_sink.bits") + ", 6, 6)"
                        replacement = "store.w.mask <= UInt<1>(1)"
                    else:
                        original = "pad(asSInt(bits(data, 4, 0)), 8)"
                        replacement = "asSInt(pad(bits(data, 4, 0), 8))"
                    assert source.count(original) == 1
                    (negative / "input.fir").write_text(source.replace(original, replacement))
                    code, _ = run([args.compiler, negative / "input.fir", "--annotation-file", oracle / "input.anno.json", "--output-dir", negative, "--multi-thread-fame5-models-xilinx"], negative, negative / "compiler.log")
                    assert code == 0, str(negative / "compiler.log")
                    (negative / "ledger.sv").write_text((case / "circt/ledger.sv").read_text())
                    code, _ = run([args.xilinx_root / "bin/xvlog", "-sv", negative / "post-fame5-models.sv", negative / "ledger.sv", args.xilinx_root / "data/verilog/src/unisims/BUFGCE.v", args.xilinx_root / "data/verilog/src/glbl.v"], negative, negative / "syntax.log")
                    assert code == 0, str(negative / "syntax.log")
                    code, _ = run([args.xilinx_root / "bin/xelab", "--timescale", "1ns/1ps", "--mt", "off", "--debug", "typical", "-s", "ledger_snapshot", "work.ledger", "work.glbl"], negative, negative / "elaborate.log")
                    assert code == 0, str(negative / "elaborate.log")
                    _, output = run([args.xilinx_root / "bin/xsim", "ledger_snapshot", "-runall"], negative, negative / (fault + ".log"))
                    assert "STATE_FAIL" in output and "F2_DYNAMIC_LEDGER" not in output, "actual target memory fault escaped: " + fault
                    fault_control = {"status": "rejected", "fault": fault, "input_sha256": digest(negative / "input.fir"), "rtl_sha256": digest(negative / "post-fame5-models.sv"), "log": str(negative / (fault + ".log"))}
                    fault_controls.append(fault_control)
                mask_control = fault_controls[0]
            receipt["cases"].append({"threads": n, "memory": memory, "same_input_sha256": digest(oracle / "input.fir"), "same_annotations_sha256": digest(oracle / "input.anno.json"), "syntax": True, "annotations": True, "annotation_relation": annotation_relation, "wrong_mask_control": mask_control, "actual_fault_controls": fault_controls, "results": results})
    receipt["passed"] = True
    receipt["gaps"] = ["production reset alignment and Linux representative remain incomplete", "separate Jasper LEC/SEC and F2 feature runtime not run"]
    (args.output_dir / "comparison.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"passed": True, "cases": len(receipt["cases"]), "receipt": str(args.output_dir / "comparison.json")}))

if __name__ == "__main__":
    main()
