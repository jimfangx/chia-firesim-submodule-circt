#!/usr/bin/env python3
"""Compare emitted native NIC queue RTL to the immutable SFC Queue_17/18.

Standalone compiler-unit RTL simulation; never invokes FireSim manager gates.
Run from the sourced project environment, using a fresh repository output dir.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + ".log")).open("w") as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    run([args.fixture.resolve(), out / "queue.mlir"], "fixture")
    run(["firtool", out / "queue.mlir", "--format=mlir", "--disable-all-randomization", "-o", out / "native.sv"], "firtool")
    source = args.golden.read_text()
    modules = []
    for name in ("Queue_17", "Queue_18"):
        match = re.search(r"^module " + name + r"\(.*?^endmodule", source, re.M | re.S)
        if not match:
            raise ValueError("missing SFC " + name)
        modules.append(match.group(0))
    # The C++ fixture evaluates these same exclusion predicates on emitted
    # operations, including each isolated source/sink stall and queue stall.
    sources = {"nic_out_bits_data", "nic_out_bits_keep", "nic_out_bits_last", "nic_out_valid"}
    sinks = {"nic_in_bits_data", "nic_in_bits_keep", "nic_in_bits_last", "nic_in_valid", "nic_macAddr",
             "nic_rlimit_inc", "nic_rlimit_period", "nic_rlimit_size", "nic_pauser_threshold",
             "nic_pauser_quanta", "nic_pauser_refresh"}
    gate_checks = {}
    for group, direction, signal, host in ((sources, "source", "valid", "toHost_hReady"),
                                           (sinks, "sink", "ready", "fromHost_hValid")):
        for name in sorted(group):
            dest = "sim_channelPorts_FireSim_ep_3_" + name + "_" + direction + "_" + ("ready" if direction == "source" else "valid")
            match = re.search(r"assign " + dest + r"\s*=\s*(.*?);", source, re.S)
            if not match:
                raise ValueError("missing SFC channel gate " + dest)
            expression = re.sub(r"//[^\n]*", "", match.group(1))
            factors = set(re.findall(r"[A-Za-z_][A-Za-z_0-9]*", expression))
            expected = {"sim_channelPorts_FireSim_ep_3_" + other + "_" + direction + "_" + signal
                        for other in group if other != name}
            expected.add("SimpleNICBridgeModule_0_hPort_" + host)
            if factors != expected or re.search(r"[|!~^]", expression):
                raise ValueError("SFC gate predicate mismatch " + dest)
            gate_checks[dest] = sorted(factors)
    (out / "sfc-gate-contracts.json").write_text(json.dumps(gate_checks, indent=2) + "\n")
    (out / "sfc.sv").write_text("\n\n".join(modules) + "\n")
    (out / "check.sv").write_text(r"""
module check;
  reg clock=0, reset=1, enq_valid=0, deq_ready=0;
  reg [65:0] enq_bits=0;
  wire nready,nvalid,r17,v17,r18,v18,keepReady;
  wire [65:0] nbits,b17,b18;
  wire [7:0] keep;
  GGNICTokenQueue10 nativeq(.clock(clock),.reset(reset),.enq_ready(nready),.enq_valid(enq_valid),
    .enq_bits(enq_bits),.deq_ready(deq_ready),.deq_valid(nvalid),.deq_bits(nbits));
  Queue_17 sfc17(.clock(clock),.reset(reset),.io_enq_ready(r17),.io_enq_valid(enq_valid),
    .io_enq_bits_data_in_data(enq_bits[63:0]),.io_enq_bits_data_in_last(enq_bits[64]),.io_enq_bits_data_in_valid(enq_bits[65]),
    .io_deq_ready(deq_ready),.io_deq_valid(v17),.io_deq_bits_data_in_data(b17[63:0]),
    .io_deq_bits_data_in_last(b17[64]),.io_deq_bits_data_in_valid(b17[65]),.io_deq_bits_data_in_keep(keep));
  Queue_18 sfc18(.clock(clock),.reset(reset),.io_enq_ready(r18),.io_enq_valid(enq_valid),
    .io_enq_bits_data_out_data(enq_bits[63:0]),.io_enq_bits_data_out_last(enq_bits[64]),.io_enq_bits_data_out_valid(enq_bits[65]),
    .io_deq_ready(deq_ready),.io_deq_valid(v18),.io_deq_bits_data_out_data(b18[63:0]),
    .io_deq_bits_data_out_last(b18[64]),.io_deq_bits_data_out_valid(b18[65]),.io_deq_bits_data_in_ready(keepReady));
  // Independent occupancy scoreboard; no native or SFC pointer expressions.
  reg [65:0] memory[0:9];
  integer head=0,tail=0,count=0,fullCycles=0,emptyCycles=0,duals=0,resets=0,pushes=0,pops=0;
  reg push,pop;
  initial begin
    #2; clock=1; #2; clock=0; reset=0;
    for (integer cycle=0;cycle<40000;cycle=cycle+1) begin
      enq_bits={2'($urandom),$urandom,$urandom};
      // Directed filling/draining, then independent stalls and repeated reset.
      enq_valid=cycle<30 ? 1 : cycle<60 ? 0 : ($urandom % 4)!=0;
      deq_ready=cycle<30 ? 0 : cycle<60 ? 1 : ($urandom % 4)!=0;
      reset=cycle>=60 && (cycle%113==0);
      #2;
      if(nready !== r17 || nready !== r18 || nready !== (count<10)) $fatal(1,"ready cycle %d",cycle);
      if(nvalid !== v17 || nvalid !== v18 || nvalid !== (count>0)) $fatal(1,"valid cycle %d",cycle);
      if(nvalid && (nbits !== b17 || nbits !== b18 || nbits !== memory[head] || keep !== 8'hff || keepReady !== 1))
        $fatal(1,"payload cycle %d",cycle);
      fullCycles += count==10; emptyCycles += count==0;
      push=nready&&enq_valid; pop=nvalid&&deq_ready;
      duals += push&&pop; resets += reset; pushes += push; pops += pop;
      if(push) begin memory[tail]=enq_bits; tail=(tail+1)%10; end
      if(pop) head=(head+1)%10;
      count=count+integer'(push)-integer'(pop);
      if(reset) begin head=0;tail=0;count=0;end
      clock=1; #2; clock=0;
    end
    if(fullCycles<20 || emptyCycles<20 || duals<1000 || resets<300 || pushes<10000 || pops<10000) $fatal(1,"coverage");
    $display("PASS NIC SFC Queue_17/Queue_18: 40000 cycles, full=%0d empty=%0d dual=%0d reset=%0d push=%0d pop=%0d",
      fullCycles,emptyCycles,duals,resets,pushes,pops);
    $finish;
  end
endmodule
""")
    run(["verilator", "--binary", "--timing", "--top-module", "check", "-Wno-fatal", "--Mdir", out / "obj",
         "-o", "check", "-j", "2", out / "native.sv", out / "sfc.sv", out / "check.sv"], "verilator-build")
    run([out / "obj/check"], "behavior")
    result = {"golden": str(args.golden.resolve()), "modules": ["Queue_17", "Queue_18"],
              "cycles": 40000, "channel_gate_contracts": len(gate_checks), "result": "matched", "behavior": (out / "behavior.log").read_text()}
    (out / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["behavior"], end="")


if __name__ == "__main__":
    main()
