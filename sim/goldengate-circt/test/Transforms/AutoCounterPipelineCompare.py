#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual source -> FAME -> bound CSV RTL against unchanged SFC.

External inputs are real decoupled target data/clock channels. CSV payloads are
produced exclusively by the transformed target, not supplied by this driver.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

DRIVER = r'''
#include "VBound.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define C(name) ((bus==0)?d->@CTRL0@##name:d->@CTRL1@##name)
#define CLOCK d->@CLOCK@
#define RESET d->@RESET@
VBound*d;unsigned bus=0;uint64_t cycle=0;unsigned reads=0,samples[2]={0,0};
unsigned accepted[8]={0},observed[2]={0,0},nonzero[2]={0,0};
bool pending[8]={};uint64_t payload[8]={};bool started=false;
struct Result{bool aw,w,b,ar,r;uint32_t data;};
Result step(){
 @EXTRAINPUTS@
 for(unsigned i=0;i<8;++i){
  if(RESET){pending[i]=false;accepted[i]=0;}
  else if(!pending[i]&&cycle%(3+i)!=0){
   pending[i]=true;
   if(i==0)payload[i]=1|((uint64_t(accepted[i]%3==0))<<1);
   else if(i==1||i==2)payload[i]=cycle<400||accepted[i]%137>=130;
   else if(i==3||i==4)payload[i]=(accepted[i]*31+7+i)&255;
   else if(i==5||i==6)payload[i]=(uint64_t(accepted[i])<<35)^(UINT64_MAX-accepted[i]*17-i);
   else payload[i]=accepted[i]&15;
  }
 }
 @DRIVEINPUTS@
 d->@OUT0@ready=cycle%7!=0;d->@OUT1@ready=cycle%11!=0;
 CLOCK=0;d->eval();
 Result r{bool(C(aw_ready)&&C(aw_valid)),bool(C(w_ready)&&C(w_valid)),bool(C(b_valid)&&C(b_ready)),bool(C(ar_ready)&&C(ar_valid)),bool(C(r_valid)&&C(r_ready)),uint32_t(C(r_bits_data))};
 bool fire[8]={@FIRES@};
 std::printf("%llu",(unsigned long long)cycle);
 @TRACEINPUTS@
 @TRACEOUTPUTS@
 int saved=bus;for(bus=0;bus<2;++bus){std::printf(" %u %u %u %u %u",unsigned(C(aw_ready)),unsigned(C(w_ready)),unsigned(C(b_valid)),unsigned(C(ar_ready)),unsigned(C(r_valid)));if(C(b_valid))std::printf(" b=%u:%u",unsigned(C(b_bits_id)),unsigned(C(b_bits_resp)));if(C(r_valid))std::printf(" r=%u:%u:%u:%u",unsigned(C(r_bits_id)),unsigned(C(r_bits_data)),unsigned(C(r_bits_resp)),unsigned(C(r_bits_last)));}bus=saved;
 std::puts("");CLOCK=1;d->eval();
 for(unsigned i=0;i<8;++i)if(fire[i]){++accepted[i];pending[i]=false;}
 ++cycle;return r;
}
void busWrite(unsigned addr,uint32_t data,unsigned seed){C(aw_bits_addr)=addr;C(aw_bits_id)=seed&4095;C(w_bits_data)=data;bool aw=false,w=false,b=false;for(unsigned n=0;!b&&n<100;++n){C(aw_valid)=!aw&&n>=seed%3;C(w_valid)=!w&&n>=(seed+1)%4;C(b_ready)=n%4!=0;auto r=step();aw|=r.aw;w|=r.w;b=r.b;}if(!aw||!w||!b)throw std::runtime_error("write timeout");C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;}
uint32_t busRead(unsigned addr,unsigned seed){C(ar_bits_addr)=addr;C(ar_bits_id)=seed&4095;bool ar=false;for(unsigned n=0;n<100;++n){C(ar_valid)=!ar;C(r_ready)=n>2&&n%3!=0;auto r=step();ar|=r.ar;if(r.r){C(ar_valid)=0;C(r_ready)=0;++reads;return r.data;}}throw std::runtime_error("read timeout");}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);Verilated::randReset(2);d=new VBound;
 RESET=1;CLOCK=0;@INITINPUTS@
 for(bus=0;bus<2;++bus){C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;C(ar_valid)=0;C(r_ready)=0;C(aw_bits_addr)=0;C(aw_bits_len)=0;C(aw_bits_size)=0;C(aw_bits_burst)=0;C(aw_bits_lock)=0;C(aw_bits_cache)=0;C(aw_bits_prot)=0;C(aw_bits_qos)=0;C(aw_bits_region)=0;C(aw_bits_id)=0;C(aw_bits_user)=0;C(ar_bits_addr)=0;C(ar_bits_len)=0;C(ar_bits_size)=0;C(ar_bits_burst)=0;C(ar_bits_lock)=0;C(ar_bits_cache)=0;C(ar_bits_prot)=0;C(ar_bits_qos)=0;C(ar_bits_region)=0;C(ar_bits_id)=0;C(ar_bits_user)=0;C(w_bits_data)=0;C(w_bits_id)=0;C(w_bits_user)=0;C(w_bits_strb)=15;C(w_bits_last)=1;}bus=0;
 d->eval();CLOCK=1;d->eval();for(unsigned n=0;n<4;++n)step();RESET=0;
 for(unsigned round=0;round<3;++round){
  for(bus=0;bus<2;++bus){busWrite(bus==0?48:40,round==0?0:round==1?3+bus:17+bus,round+bus+1);busWrite(bus==0?52:44,0,round+5);busWrite(bus==0?56:48,1,round+bus+9);}
  bus=0;for(unsigned n=0;n<701;++n)step();
  for(unsigned n=0;n<13;++n)for(bus=0;bus<2;++bus){if(busRead(bus==0?60:52,n+30)){++samples[bus];unsigned words=bus==0?12:10;for(unsigned word=0;word<words;++word){auto value=busRead(word*4,n*12+word);if(word>=2&&word<words-2&&value)++nonzero[bus];}busWrite(bus==0?64:56,1,n+50);}}
 }
 if(samples[0]<10||samples[1]<10||nonzero[0]<10||nonzero[1]<10||observed[0]<30||observed[1]<30||accepted[0]<100)throw std::runtime_error("insufficient bound target/counter exercise");
 std::fprintf(stderr,"PASS cycles=%llu reads=%u samples=%u/%u observed=%u/%u nonzero=%u/%u clocks=%u\n",(unsigned long long)cycle,reads,samples[0],samples[1],observed[0],observed[1],nonzero[0],nonzero[1],accepted[0]);d->final();delete d;return 0;}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("native", type=Path)
    p.add_argument("sfc", type=Path)
    p.add_argument("evidence", type=Path)
    p.add_argument("--firtool", type=Path, required=True)
    p.add_argument("--verilator", type=Path, required=True)
    p.add_argument("--global-control", action="store_true")
    a = p.parse_args()
    stem="post-autocounter-csv-control" if a.global_control else "post-autocounter-csv-bindings"
    native, sfc, out = a.native.resolve(), a.sfc.resolve(), a.evidence.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands, files = [], []

    def run(cmd, name, trace=None):
        cmd = list(map(str, cmd)); commands.append(cmd)
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with (out / (name + ".log")).open("w") as log:
            if trace:
                with trace.open("w") as data:
                    subprocess.run(cmd, stdout=data, stderr=log, check=True)
            else:
                subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)

    na = json.loads((native / (stem + "-all.json")).read_text())
    sa = json.loads((sfc / "sfc-fame.json").read_text())
    classes = {x["class"] for x in na}
    supplemental = {"firrtl.options.TargetDirAnnotation", "firrtl.transforms.CombinationalPath",
                    "midas.InternalXDCAnnotation", "firrtl.transforms.BlackBoxInlineAnno",
                    "midas.passes.AutoCounterPipelineOracle$ConstructorHints"}
    assert {x["class"] for x in sa} - classes == supplemental

    def norm(x):
        if isinstance(x, str):
            return x.replace("~GGControlMasterWrapper|GGControlMasterWrapper>","~FAMETop|FAMETop>").replace("~GGControlMasterWrapper","~GGAutoCounterBridgeHostWrapper").replace("~GGAutoCounterBridgeHostWrapper|GGAutoCounterBridgeHostWrapper>", "~FAMETop|FAMETop>").replace("~GGAutoCounterBridgeHostWrapper", "~FAMETop")
        if isinstance(x, list):
            return [norm(v) for v in x]
        if isinstance(x, dict):
            y = {k: norm(v) for k, v in x.items()}
            if y.get("class") == "firesim.lib.bridgeutils.BridgeIOAnnotation":
                y["target"] = "BOUNDHOST"
            if y.get("class") == "midas.passes.fame.FAMEChannelConnectionAnnotation" and y["globalName"].startswith("autocounter_"):
                y.pop("sinks", None)
            return y
        return x

    assert sorted(json.dumps(norm(x), sort_keys=True) for x in na) == sorted(json.dumps(norm(x), sort_keys=True) for x in sa if x["class"] in classes), "retained FAME/channel/constructor contract mismatch"
    bridges = [x for x in na if x["class"] == "firesim.lib.bridgeutils.BridgeIOAnnotation"]
    assert [b["clockInfo"] for b in bridges] == [dict(name="base", multiplier=1, divisor=1), dict(name="slow", multiplier=1, divisor=3)]
    sinks = {g: b["target"] + "." + local for b in bridges for local, g in b["channelMapping"].items()}
    for x in na:
        if x["class"].endswith("FAMEChannelConnectionAnnotation") and x["globalName"] in sinks:
            assert x["sinks"] == [sinks[x["globalName"]]]
            assert len(x["sources"]) == 1 and "|FAMETop>" in x["sources"][0] and x["sources"][0].endswith(".bits")
    mlir = (native / (stem + ".mlir")).read_text()
    banks = re.findall(r"goldengate\.mmioRegisters = \[(.*?)\]", mlir)
    assert len(banks) == 2
    for i, bank in enumerate(banks):
        rows = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i64, readable = (true|false), writeable = (true|false)\}', bank)
        assert (sfc / f"abi-{i}.txt").read_text() == "".join(f"{name}={offset}\n" for name, offset, _, _ in rows)
    gate_info = re.findall(r'goldengate\.fameClockGate = "([^"]+)", goldengate\.generatedClockConstraint = \{mfmr = (\d+) : i64, name = "([^"]+)"\}', mlir)
    assert sorted(gate_info) == [("clock0", "1", "base"), ("clock1", "3", "slow")]
    rtl = out / "sfc.sv"
    run([a.firtool.resolve(), sfc / "bound.fir", "--disable-all-randomization", "--disable-annotation-unknown", "-o", rtl], "sfc-syntax")
    traces = []
    channels = ["clocks", "reset0", "reset1", "event0", "event1", "value0", "value1", "topEvent"]
    for kind in ["native", "sfc"]:
        prefix = "" if kind == "native" else "channels_"
        def channel(n): return prefix + "Top_" + n + "_sink_"
        def output(i): return prefix + f"Top_observed{i}_source_"
        inputs, initial, checks, fires = [], [], [], []
        for i, n in enumerate(channels):
            name = channel(n)
            bits = [name + "bits_0", name + "bits_1"] if i == 0 else [name + "bits"]
            if kind == "sfc" and i == 0:
                bits = [name + "bits___05F0", name + "bits___05F1"]
            inputs.append(f"d->{name}valid=pending[{i}];")
            initial.append(f"d->{name}valid=0;")
            for j, bit in enumerate(bits):
                inputs.append(f"d->{bit}=(payload[{i}]>>{j})&1;" if i == 0 else f"d->{bit}=payload[{i}];")
                initial.append(f"d->{bit}=0;")
            fires.append(f"bool(d->{name}valid&&d->{name}ready)")
            checks.append(f'std::printf(" %u",unsigned(d->{name}ready));')
        output_checks = []
        for i in range(2):
            name = output(i)
            initial.append(f"d->{name}ready=0;")
            output_checks.append(f'std::printf(" %u",unsigned(d->{name}valid));if(cycle>=600&&d->{name}valid){{std::printf(" o{i}=%llu",(unsigned long long)d->{name}bits);if(d->{name}ready)++observed[{i}];}}')
        mapping = dict(CTRL0="autocounter_0_ctrl_" if kind == "native" else "ctrl0_", CTRL1="autocounter_1_ctrl_" if kind == "native" else "ctrl1_",
                       CLOCK="hostClock" if kind == "native" else "clock", RESET="hostReset" if kind == "native" else "reset",
                       EXTRAINPUTS="" if kind == "native" else "d->channels_hostClock=CLOCK;d->channels_hostReset=RESET;",
                       DRIVEINPUTS="\n".join(inputs), INITINPUTS="\n".join(initial), FIRES=",".join(fires), TRACEINPUTS="\n".join(checks), TRACEOUTPUTS="\n".join(output_checks), OUT0=output(0), OUT1=output(1))
        source = DRIVER
        if a.global_control:
            source=source.replace('#define C(name) ((bus==0)?d->@CTRL0@##name:d->@CTRL1@##name)', '#define C(name) d->ctrl_##name')
            source=source.replace('C(aw_bits_addr)=addr;', 'C(aw_bits_addr)=addr+(bus?128:0);').replace('C(ar_bits_addr)=addr;', 'C(ar_bits_addr)=addr+(bus?128:0);')
            old='int saved=bus;for(bus=0;bus<2;++bus){std::printf'
            source=source.replace(old,'std::printf').replace('unsigned(C(r_bits_last)));}bus=saved;','unsigned(C(r_bits_last)));')
        for key, value in mapping.items(): source = source.replace(f"@{key}@", value)
        assert not re.search(r"@[A-Z0-9_]+@", source)
        cpp, obj = out / f"{kind}.cpp", out / f"{kind}-obj"
        cpp.write_text(source)
        actual = native / (stem + ".sv") if kind == "native" else rtl
        run([a.verilator.resolve(), "--cc", "--exe", "--build", "-j", "1", "-Wno-fatal", "--assert", "--top-module", ("GGControlMasterWrapper" if a.global_control else "GGAutoCounterBridgeHostWrapper") if kind == "native" else "LazyModule", "--prefix", "VBound", "--Mdir", obj, actual, sfc / "AbstractClockGate.v", cpp], kind + "-build")
        trace = out / f"{kind}.trace"
        run([obj / "VBound", f"+verilator+seed+{101 if kind == 'native' else 202}"], kind + "-run", trace)
        traces.append(trace); files += [actual, cpp, trace]
    assert traces[0].read_bytes() == traces[1].read_bytes(), "bound target/MMIO/sample RTL differential mismatch"
    files += [native / (stem + ".mlir"), native / (stem + "-all.json"), sfc / "bound.fir", sfc / "sfc-fame.json", sfc / "AbstractClockGate.v", out / "commands.json", Path(__file__).resolve()]
    result = dict(passed=True, full_feature_acceptance=False,
        scope="same source -> CSV -> reset/trigger/clock wiring -> real FAME -> bound actual CSV hosts" + (" -> complete global AXI control" if a.global_control else ""),
        global_control=a.global_control,
        cycles=len(traces[0].read_text().splitlines()), exact_retained_annotations=len(na), exact_mmio_registers=[17, 15],
        checks=["complete channel/ordered constructor metadata", "base1/1 and slow1/3 clock ownership/gate constraints", "eight independently stalled stable input tokens", "target state and output stalls", "real target-derived CSV data and global-reset payloads", "period0/3/17 samples, queue drops, AXI AW/W skew and response stalls"],
        assumptions=["caller supplies rational clock tokens; generator phase is outside this boundary", "no trigger sources in this matched fixture selects unchanged SFC default-enabled trigger semantics", "independent randomized initial states then a host-reset edge; target registers explicitly reset through target input tokens before observing state payloads", "sample slots remain unreset and observed only after countersready; no memory clearing", "SFC verification linker maps ordered clock pins _0/_1 to numeric0/1 after LowerTypes and uses unchanged RemoveValidIf emission preparation"],
        remaining=["dynamic trigger-source representative", ("mixed-widget F2 platform assembly" if a.global_control else "global control/driver/header and F2 platform assembly"), "complete Print", "required representative LEC/SEC/F2 Linux evidence"],
        sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files})
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS", result["cycles"], "cycles; source/FAME/bound CSV differential")


if __name__ == "__main__":
    main()
