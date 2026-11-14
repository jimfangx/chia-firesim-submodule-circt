#!/usr/bin/env python3
"""Exercise AXI beat conversion, lane strobes, reused IDs and host stalls."""
import argparse
from pathlib import Path
import re
import subprocess


def testbench(rtl, width):
    header = re.search(r"(?ms)^module GGFASEDHostWidthAdapter\(.*?^\);", rtl).group()
    header = re.sub(r"//[^\n]*", "", header).split("(", 1)[1].rsplit(")", 1)[0]
    ports = []
    direction, bits = None, 1
    for part in header.split(","):
        match = re.fullmatch(r"\s*(?:(input|output)\s*)?(?:\[(\d+):0\]\s*)?(\w+)\s*", part)
        if not match:
            raise ValueError(part)
        if match[1]:
            direction, bits = match[1], int(match[2]) + 1 if match[2] else 1
        ports.append((direction, bits, match[3]))
    sv = [f"module tb; localparam WIDTH={width}, RATIO=(WIDTH>=64)?WIDTH/64:1, BYTES=WIDTH/8, LOG_BYTES=$clog2(BYTES), MAX_BEATS=64/BYTES;"]
    sv += [f"{'logic' if d == 'input' else 'wire'} [{w-1}:0] {n}" +
           ("=0;" if d == "input" else ";") for d, w, n in ports]
    sv += ["GGFASEDHostWidthAdapter dut(" + ",".join(f".{n}({n})" for _, _, n in ports) + ");"]
    sv += [r'''
integer cycles=0,writes=0,reads=0,arSeen=0,awSeen=0,wSeen=0,readBeats=0;
integer expRId[0:4095],expRLast[0:4095],expRResp[0:4095],re=0,rd=0;
logic [WIDTH-1:0] expRData[0:4095];
bit awFire,wFire,arFire,rFire,targetRFire;
task step;begin
  #2;
  awFire=in_aw_valid && in_aw_ready;wFire=out_w_valid && out_w_ready;
  arFire=in_ar_valid && in_ar_ready;rFire=out_r_valid && out_r_ready;
  targetRFire=in_r_valid && in_r_ready;
  if(!reset)begin
    if(awFire)begin
      if(!out_aw_valid || out_aw_bits_addr!=in_aw_bits_addr || out_aw_bits_id!=in_aw_bits_id ||
         out_aw_bits_size!=((in_aw_bits_size>3)?3:in_aw_bits_size) ||
         out_aw_bits_len!=((in_aw_bits_len+1)*((in_aw_bits_size>3)?(1<<(in_aw_bits_size-3)):1)-1))
        $fatal(1,"AW geometry conversion differs");
      awSeen++;
    end
    if(arFire)begin
      if(!out_ar_valid || out_ar_bits_addr!=in_ar_bits_addr || out_ar_bits_id!=in_ar_bits_id ||
         out_ar_bits_size!=((in_ar_bits_size>3)?3:in_ar_bits_size) ||
         out_ar_bits_len!=((in_ar_bits_len+1)*((in_ar_bits_size>3)?(1<<(in_ar_bits_size-3)):1)-1))
        $fatal(1,"AR geometry conversion differs");
      arSeen++;
    end
    if(targetRFire)begin
      if(rd==re || in_r_bits_data!==expRData[rd] || in_r_bits_id!=expRId[rd] ||
         in_r_bits_last!=expRLast[rd] || in_r_bits_resp!=expRResp[rd])
        $fatal(1,"read assembly differs idx=%0d id=%0d/%0d data=%h/%h",rd,in_r_bits_id,expRId[rd],in_r_bits_data,expRData[rd]);
      rd++;readBeats++;
    end
  end
end endtask
task edge_clock;begin clock=1;#2;clock=0;#2;cycles++;if(cycles>60000)$fatal(1,"timeout");end endtask
function automatic [63:0] dataWord(input integer transaction,beat,lane);
  return (64'h9876543200000000 | (64'(transaction)<<20) | (64'(beat)<<12) | 64'(lane));
endfunction
integer sizes[0:47],addresses[0:47],lengths[0:47];
integer batch,t,i,j,k,beat,segment,splits,lane,physical,expectedLast;
logic [WIDTH-1:0] sourceData;
logic [BYTES-1:0] sourceStrobe;
integer rSize[0:1][0:14],rAddr[0:1][0:14],rLen[0:1][0:14];
integer rTransaction[0:1],rPhysical[0:1],choose,tid,seq,offset,remaining;
logic [WIDTH-1:0] assembled[0:1];
integer response[0:1];
initial begin
  reset=1;step();edge_clock();reset=0;
  in_aw_bits_burst=1;in_ar_bits_burst=1;in_b_ready=1;in_r_ready=1;
  // Queue multiple AW descriptors before W, then stress both sides with stalls.
  for(t=0;t<48;t++)begin
    sizes[t]=t%(LOG_BYTES+1);
    addresses[t]=4096+t*128+((t*8)&(BYTES-1));
    addresses[t]=(addresses[t]>>sizes[t])<<sizes[t];
    lengths[t]=t%MAX_BEATS;
  end
  for(batch=0;batch<8;batch++)begin
    for(t=batch*6;t<batch*6+6;t++)begin
      in_aw_valid=1;in_aw_bits_addr=35'(addresses[t]);in_aw_bits_size=3'(sizes[t]);
      in_aw_bits_len=8'(lengths[t]);in_aw_bits_id=4'(t%16);
      do begin out_aw_ready=$urandom_range(0,1);step();edge_clock();end while(!awFire);
      in_aw_valid=0;
    end
    for(t=batch*6;t<batch*6+6;t++)begin
      splits=(sizes[t]>3)?(1<<(sizes[t]-3)):1;
      for(beat=0;beat<=lengths[t];beat++)begin
        sourceData=0;sourceStrobe=0;
        if(WIDTH>=64)for(j=0;j<RATIO;j++)begin
          sourceData[j*64+:64]=dataWord(t,beat,j);
          sourceStrobe[j*8+:8]=8'(8'hf7^t^beat^j);
        end
        if(WIDTH<64)begin sourceData=WIDTH'(dataWord(t,beat,0));sourceStrobe=BYTES'(8'hf7^t^beat);end
        in_w_bits_data=sourceData;in_w_bits_strb=sourceStrobe;
        in_w_bits_last=(beat==lengths[t]);in_w_valid=1;segment=0;
        while(segment<splits)begin
          out_w_ready=$urandom_range(0,1);step();
          if(wFire)begin
            lane=((addresses[t]+(beat<<sizes[t]))/8+segment)%RATIO;
            expectedLast=(beat==lengths[t] && segment==splits-1);
            if(WIDTH>=64)begin
              if(out_w_bits_data!==sourceData[lane*64+:64] || out_w_bits_strb!==sourceStrobe[lane*8+:8])
                $fatal(1,"write split/lane differs");
            end else begin
              lane=((addresses[t]+(beat<<sizes[t]))%8)/BYTES;
              if(out_w_bits_data!==(64'(sourceData)<<(lane*WIDTH)) || out_w_bits_strb!==(8'(sourceStrobe)<<(lane*BYTES)))
                $fatal(1,"write widening lane differs");
            end
            if(out_w_bits_last!=expectedLast || in_w_ready!=(segment==splits-1))$fatal(1,"write split handshake differs");
            segment++;wSeen++;
          end
          edge_clock();
        end
        in_w_valid=0;
      end
      writes++;
    end
  end
  // Fifteen same-ID reads are queued for each of two IDs. Return physical
  // beats interleaved between IDs, preserving order within each ID.
  for(i=0;i<2;i++)begin
    rTransaction[i]=0;rPhysical[i]=0;assembled[i]=0;response[i]=0;
    for(t=0;t<15;t++)begin
      rSize[i][t]=(t+i)%(LOG_BYTES+1);
      rAddr[i][t]=16384+t*128+((t*8)&(BYTES-1));
      rAddr[i][t]=(rAddr[i][t]>>rSize[i][t])<<rSize[i][t];
      rLen[i][t]=t%MAX_BEATS;
      in_ar_bits_id=4'(i==0?3:5);in_ar_bits_addr=35'(rAddr[i][t]);
      in_ar_bits_size=3'(rSize[i][t]);in_ar_bits_len=8'(rLen[i][t]);in_ar_valid=1;
      do begin out_ar_ready=$urandom_range(0,1);step();edge_clock();end while(!arFire);
      in_ar_valid=0;
    end
  end
  remaining=30;
  while(remaining!=0)begin
    choose=$urandom_range(0,1);if(rTransaction[choose]==15)choose=1-choose;
    seq=rTransaction[choose];tid=choose==0?3:5;
    splits=(rSize[choose][seq]>3)?(1<<(rSize[choose][seq]-3)):1;
    beat=rPhysical[choose]/splits;segment=rPhysical[choose]%splits;
    offset=(rAddr[choose][seq]+(beat<<rSize[choose][seq]))%BYTES;
    lane=(offset/8+segment)%RATIO;
    out_r_valid=1;out_r_bits_id=4'(tid);
    out_r_bits_data=dataWord(100+choose*15+seq,beat,lane);
    out_r_bits_last=(rPhysical[choose]==(rLen[choose][seq]+1)*splits-1);
    out_r_bits_resp=2'((segment+seq)%4);
    do begin in_r_ready=$urandom_range(0,1);step();
      if(rFire)begin
        if(segment==0)begin assembled[choose]=0;response[choose]=0;end
        if(WIDTH>=64)assembled[choose][lane*64+:64]=out_r_bits_data;
        else assembled[choose]=WIDTH'(out_r_bits_data>>(((rAddr[choose][seq]+(beat<<rSize[choose][seq]))%8)/BYTES*WIDTH));
        if(out_r_bits_resp>response[choose])response[choose]=out_r_bits_resp;
        if(segment==splits-1)begin
          expRData[re]=assembled[choose];expRId[re]=tid;expRLast[re]=int'(out_r_bits_last);expRResp[re]=response[choose];re++;
        end
        rPhysical[choose]++;
        if(out_r_bits_last)begin rTransaction[choose]++;rPhysical[choose]=0;remaining--;reads++;end
      end
      edge_clock();
    end while(!rFire);
    out_r_valid=0;
  end
  in_r_ready=1;
  while(rd!=re)begin step();edge_clock();end
  // B responses preserve identity and error response, under backpressure.
  for(t=0;t<16;t++)begin
    in_b_ready=(t%2==0);out_b_valid=1;out_b_bits_id=4'(t);out_b_bits_resp=2'(t%4);#1;
    if(in_b_valid!==out_b_valid || out_b_ready!==in_b_ready || in_b_bits_id!==out_b_bits_id || in_b_bits_resp!==out_b_bits_resp)
      $fatal(1,"B response changed");
    step();edge_clock();
  end
  if(writes!=48 || reads!=30 || awSeen!=48 || arSeen!=30 || readBeats==0)$fatal(1,"coverage failed");
  $display("PASS %0d-to-64 AXI conversion: writes=%0d reads=%0d target read beats=%0d physical write beats=%0d cycles=%0d",WIDTH,writes,reads,readBeats,wSeen,cycles);
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
    base = args.output_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    for width in (8, 32, 128, 256, 512):
        out = base / str(width)
        out.mkdir(exist_ok=True)

        def run(command, name):
            with (out / (name + ".log")).open("w") as log:
                subprocess.run(list(map(str, command)), check=True, stdout=log,
                               stderr=subprocess.STDOUT, timeout=240)

        run([args.fixture.resolve(), str(width), out / "native.mlir"], "fixture")
        run([args.firtool.resolve(), out / "native.mlir", "--format=mlir", "--verilog", "-o", out / "native.sv"], "lower")
        (out / "tb.sv").write_text(testbench((out / "native.sv").read_text(), width))
        run([args.verilator.resolve(), "--binary", "--timing", "--assert", "-Wno-fatal", "-j", "4",
             "--top-module", "tb", "--Mdir", out / "obj", out / "native.sv", out / "tb.sv"], "build")
        run([out / "obj/Vtb"], "run")
        print((out / "run.log").read_text())


if __name__ == "__main__":
    main()
