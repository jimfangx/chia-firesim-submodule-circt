#!/usr/bin/env python3
# See LICENSE for license details.
"""Matched complete SFC/native recursive FAME5 grouping with independent ledgers.

The two-level hierarchy has two sibling Branch instances, each with distinct
state and a Leaf instance owning independent state and masked depth-three RAM.
All module-local unreset selectors start aligned; target RAM is never initialized.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

LEDGER = r'''`timescale 1ns/1ps
module ledger;
localparam integer N=NUM;
localparam integer RESET_AT=8192-(8192%N);
reg hostClock=0,hostReset=1;
reg [N-1:0] in_valid=0,out_ready=0;
wire [N-1:0] in_ready,out_valid;
reg in_clear[0:N-1],in_enable[0:N-1],in_write[0:N-1],in_mask[0:N-1];
reg [7:0] in_add[0:N-1];
reg [1:0] in_addr[0:N-1];
wire [7:0] out_left[0:N-1],out_right[0:N-1],out_memLeft[0:N-1],out_memRight[0:N-1],out_parent[0:N-1];
Top dut(PORTS);
reg [7:0] rootState[0:N-1],branchState[0:N-1][0:1],leafState[0:N-1][0:1];
reg [7:0] memory[0:N-1][0:1][0:2];
reg written[0:N-1][0:1][0:2];
reg handshake[0:N-1];
integer seq[0:N-1];
integer i,lane,addr,cycle,slot,sel,tx=0,stalls=0,observations=0,writes=0,masked=0,clears=0;
reg [511:0] control;
reg [7:0] actual,expected,data;
reg fire;
reg [63:0] checksum=0;
task tick;
 begin #5;hostClock=1;#5;hostClock=0;#1;end
endtask
task resetHost;
 begin
 hostReset=1;in_valid=0;out_ready=0;
 for(i=0;i<N;i=i+1)begin in_clear[i]=0;in_enable[i]=0;in_write[i]=0;in_mask[i]=0;in_add[i]=0;in_addr[i]=0;end
 repeat(N)tick();hostReset=0;
 for(i=0;i<N;i=i+1)begin
  rootState[i]=0;handshake[i]=1;
  for(lane=0;lane<2;lane=lane+1)begin branchState[i][lane]=0;leafState[i][lane]=0;end
 end
 end
endtask
initial begin
 dut.Worker_threaded.threadIdx=0;
 dut.Worker_threaded.left.threadIdx=0;
 dut.Worker_threaded.right.threadIdx=0;
 dut.Worker_threaded.left.leaf.threadIdx=0;
 dut.Worker_threaded.right.leaf.threadIdx=0;
 control=0;if($value$plusargs("control=%s",control))begin end
 BASE_COUNTER_STARTUP
 if(control=="alternate-child-phase")dut.Worker_threaded.right.leaf.threadIdx=1;
 for(i=0;i<N;i=i+1)begin seq[i]=0;for(lane=0;lane<2;lane=lane+1)for(addr=0;addr<3;addr=addr+1)written[i][lane][addr]=0;end
 resetHost();
 for(cycle=0;cycle<16384;cycle=cycle+1)begin
  if(cycle==RESET_AT)resetHost();
  for(i=0;i<N;i=i+1)begin
   if(handshake[i])begin
    seq[i]=seq[i]+1;in_add[i]=(seq[i]*29+i*41)%256;
    in_clear[i]=(seq[i]+i)%67==0;in_enable[i]=(seq[i]+i)%5!=0;
    in_addr[i]=(seq[i]+i)%3;in_write[i]=(seq[i]+i)%7!=0;in_mask[i]=(seq[i]+i)%4!=0;
    in_valid[i]=(cycle*3+i*7)%11!=0;
   end else if(!in_valid[i] && (cycle*3+i*7)%11!=0)in_valid[i]=1;
   out_ready[i]=(cycle/(i+1)+i)%7!=0;
  end
  #1;slot=cycle%N;sel=(slot==0)?1:(slot==1)?0:(N==4?5-slot:slot);
  fire=in_valid[sel] && out_ready[sel];
  for(i=0;i<N;i=i+1)begin
   if((in_ready[i] ^ (control=="corrupt-ready" && cycle==100 && i==sel)) !== ((i==sel)&&out_ready[sel]) ||
      (out_valid[i] ^ (control=="corrupt-valid" && cycle==100 && i==sel)) !== ((i==sel)&&in_valid[sel]))begin
    $display("CONTROL_FAIL cycle=%0d thread=%0d",cycle,i);$fatal;
   end
   checksum=checksum*131+in_ready[i];checksum=checksum*131+out_valid[i];
   if(out_valid[i])begin
    actual=out_parent[i];if(control=="corrupt-written-payload" && cycle>=100)actual=actual^1;
    if(actual !== rootState[i] || out_left[i] !== (branchState[i][0]^leafState[i][0]) ||
       out_right[i] !== (branchState[i][1]^leafState[i][1]))begin
     $display("STATE_FAIL cycle=%0d thread=%0d parent=%0d expected=%0d left=%0d right=%0d",cycle,i,actual,rootState[i],out_left[i],out_right[i]);$fatal;
    end
    checksum=checksum*131+actual;checksum=checksum*131+out_left[i];checksum=checksum*131+out_right[i];
   end
   for(lane=0;lane<2;lane=lane+1)begin
    actual=(lane==0)?out_memLeft[i]:out_memRight[i];addr=in_addr[i];
    if(control=="different-invalid-payload" && (!out_valid[i] || !written[i][lane][addr]))actual=actual^8'ha5;
    if(out_valid[i] && written[i][lane][addr])begin
     if(actual !== memory[i][lane][addr])begin
      $display("MEMORY_FAIL cycle=%0d thread=%0d lane=%0d address=%0d actual=%0d expected=%0d",cycle,i,lane,addr,actual,memory[i][lane][addr]);$fatal;
     end
     observations=observations+1;checksum=checksum*131+actual;
    end
   end
   handshake[i]=in_valid[i] && in_ready[i];
  end
  if(in_clear[sel])begin
   rootState[sel]=0;for(lane=0;lane<2;lane=lane+1)begin branchState[sel][lane]=0;leafState[sel][lane]=0;end
   clears=clears+1;
  end else if(fire && in_enable[sel])begin
   rootState[sel]=(rootState[sel]+3)&255;
   for(lane=0;lane<2;lane=lane+1)begin
    data=(lane==0)?in_add[sel]:(in_add[sel]^8'h5a);
    branchState[sel][lane]=(branchState[sel][lane]+data)&255;
    leafState[sel][lane]=(leafState[sel][lane]+1)&255;
   end
  end
  if(fire && in_write[sel])begin
   for(lane=0;lane<2;lane=lane+1)begin
    addr=in_addr[sel];data=(lane==0)?in_add[sel]:(in_add[sel]^8'h5a);
    if(in_mask[sel])begin memory[sel][lane][addr]=data;written[sel][lane][addr]=1;writes=writes+1;end
    else masked=masked+1;
   end
  end
  if(fire)tx=tx+1;else stalls=stalls+1;
  tick();
 end
 for(i=0;i<N;i=i+1)for(lane=0;lane<2;lane=lane+1)for(addr=0;addr<3;addr=addr+1)
  if(!written[i][lane][addr])begin $display("UNEXERCISED_WORD thread=%0d lane=%0d addr=%0d",i,lane,addr);$fatal;end
 if(tx<1000 || observations<1000 || writes==0 || masked==0 || stalls==0 || clears==0)$fatal;
 $display("HIERARCHY_LEDGER N=%0d tx=%0d responses=%0d writes=%0d masked=%0d stalls=%0d clears=%0d checksum=%0d",N,tx,observations,writes,masked,stalls,clears,checksum);
 $finish;
end
endmodule
'''

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--synchronous',action='store_true')
    parser.add_argument('--threads',type=int,nargs='+',choices=(2,3,4),default=(2,3,4))
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--sfc-root',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--xilinx-root',type=Path,default=Path('/opt/Xilinx/2025.2/Vivado'))
    args=parser.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
    receipt={'scope':'bounded recursive FAME5 two-level hierarchy with separate sibling register/memory state; complete unchanged SFC grouping','synchronous_descendants':args.synchronous,'compiler_sha256':digest(args.compiler),'script_sha256':digest(__file__),'oracle_sha256':digest(Path(__file__).with_name('FAME5HierarchyOracle.scala')),'commands':[],'cases':[],'assumptions':['all module-local unreset thread selectors start at zero aligned with top scheduler','hostReset asserted for exactly a complete thread rotation at startup and midtraffic','input valid/payload held until accepted','hostClock is shared by parent, child target state, added host controls and selectors','three-thread per-accessor unreset base counters start at zero aligned with local selector','synchronous descendant edge-count and host-tracker helpers exercise all four concrete Boolean startup pairs; target and response RAM/tag pipes stay uninitialized','no target RAM initialization: only locations previously written in the exact thread/sibling/address are compared'],'full_fame5':False}
    def run(command,directory,name):
        log=directory/(name+'.log');receipt['commands'].append({'argv':list(map(str,command)),'cwd':str(directory),'log':str(log)})
        with log.open('w')as stream:result=subprocess.run(list(map(str,command)),cwd=directory,stdout=stream,stderr=subprocess.STDOUT)
        return result.returncode,log.read_text()
    def simulate(directory,rtl,n,controls):
        directory.mkdir(exist_ok=True)
        ports=['.hostClock(hostClock)','.hostReset(hostReset)']
        for i in range(n):
            ports += [f'.{a}{i}_{b}({a}_{b}[{i}])' for a in ('in','out')for b in ('valid','ready')]
            ports += [f'.in{i}_bits_{field}(in_{field}[{i}])'for field in ('clear','add','enable','addr','write','mask')]
            ports += [f'.out{i}_bits_{field}(out_{field}[{i}])'for field in ('left','right','memLeft','memRight','parent')]
        startup=[]
        if n==3:
            for branch in ('left','right'):
                if rtl.name != 'threaded.sfc.v':
                    names=('store_base_addr_counter','store_base_addr_counter_1')
                    startup += [f' dut.Worker_threaded.{branch}.leaf.{name}=0;'for name in names]
                else:
                    startup += [f' dut.Worker_threaded.{branch}.leaf.store.base_addr_counter{suffix}=0;'for suffix in ('','_0')]
        ledger=LEDGER
        if args.synchronous:
            ledger=ledger.replace('reg written[0:N-1][0:1][0:2];','reg written[0:N-1][0:1][0:2];\nreg [7:0] lastRead[0:N-1][0:1];reg definedRead[0:N-1][0:1];')
            ledger=ledger.replace('rootState[i]=0;handshake[i]=1;', 'rootState[i]=0;handshake[i]=1;for(lane=0;lane<2;lane=lane+1)definedRead[i][lane]=0;')
            ledger=ledger.replace('written[i][lane][addr]', 'definedRead[i][lane]')
            # Restore target written-set initialization/final coverage; only
            # response qualification uses the prior read, sampled pre-write.
            ledger=ledger.replace('definedRead[i][lane]=0;end', 'written[i][lane][addr]=0;end')
            ledger=ledger.replace('if(!definedRead[i][lane])begin', 'if(!written[i][lane][addr])begin')
            ledger=ledger.replace('actual !== memory[i][lane][addr]', 'actual !== lastRead[i][lane]').replace('actual,memory[i][lane][addr]', 'actual,lastRead[i][lane]')
            ledger=ledger.replace('if(fire && in_write[sel])begin', 'for(lane=0;lane<2;lane=lane+1)begin addr=in_addr[sel];definedRead[sel][lane]=written[sel][lane][addr];if(definedRead[sel][lane])lastRead[sel][lane]=memory[sel][lane][addr];end\n  if(fire && in_write[sel])begin')
            for branch in ('left','right'):
                prefix=f' dut.Worker_threaded.{branch}.leaf.'
                if rtl.name=='threaded.sfc.v':
                    startup += [prefix+'store.edgeCount=(control=="helper-startup-10" || control=="helper-startup-11");',prefix+'store.edgeCountTracker=(control=="helper-startup-01" || control=="helper-startup-11");']
                else:
                    startup += [prefix+'store_edgeCount=(control=="helper-startup-10" || control=="helper-startup-11");',prefix+'store_edgeCountTracker=(control=="helper-startup-01" || control=="helper-startup-11");']
        (directory/'ledger.sv').write_text(ledger.replace('NUM',str(n)).replace('PORTS',',\n'.join(ports)).replace('BASE_COUNTER_STARTUP','\n'.join(startup)))
        code,_=run([args.xilinx_root/'bin/xvlog','-sv',rtl,directory/'ledger.sv'],directory,'syntax');assert code==0,str(directory/'syntax.log')
        code,_=run([args.xilinx_root/'bin/xelab','--timescale','1ns/1ps','--mt','off','--debug','typical','-s','ledger_snapshot','work.ledger'],directory,'elaborate');assert code==0,str(directory/'elaborate.log')
        results={}
        for control in controls:
            command=[args.xilinx_root/'bin/xsim','ledger_snapshot','-runall']
            if control!='none':command+=['-testplusarg','control='+control]
            _,text=run(command,directory,control);summary=re.search(r'HIERARCHY_LEDGER.*',text)
            if control in ('none','different-invalid-payload','alternate-child-phase') or control.startswith('helper-startup-'):
                assert summary,str(directory/(control+'.log'));results[control]=summary.group()
            else:
                assert not summary and re.search(r'(CONTROL|STATE|MEMORY)_FAIL',text),str(directory/(control+'.log'));results[control]='rejected'
        if 'different-invalid-payload'in results:assert results['none']==results['different-invalid-payload']
        if 'alternate-child-phase'in results:assert results['none']==results['alternate-child-phase']
        for name,value in results.items():
            if name.startswith('helper-startup-'):assert value==results['none']
        return results
    for n in args.threads:
        oracle=args.sfc_root/f'sfc{n}';case=args.output_dir/f'threads{n}';case.mkdir(exist_ok=True)
        native=case/'native';native.mkdir(exist_ok=True)
        command=[args.compiler,oracle/'input.fir','--annotation-file',oracle/'input.anno.json','--output-dir',native,'--multi-thread-fame5-models']
        code,_=run(command,case,'compiler');assert code==0,str(case/'compiler.log')
        annotations=json.loads((oracle/'threaded.sfc.json').read_text())
        inline={'class':'firrtl.passes.InlineAnnotation','target':'Top.ThreadedMem'}
        assert annotations.count(inline)==1;annotations.remove(inline)
        assert 'module ThreadedMem :'in(oracle/'threaded.sfc.fir').read_text()
        assert 'ThreadedMem'not in(native/'post-fame5-models.mlir').read_text()
        assert annotations==json.loads((native/'post-fame5-models-all.json').read_text())
        results=[]
        for side,rtl in [('sfc',oracle/'threaded.sfc.v'),('circt',native/'post-fame5-models.sv')]:
            controls=('none','different-invalid-payload','corrupt-ready','corrupt-valid','corrupt-written-payload','alternate-child-phase')
            if args.synchronous:controls+=('helper-startup-01','helper-startup-10','helper-startup-11')
            outcomes=simulate(case/side,rtl,n,controls)
            results.append({'side':side,'rtl_sha256':digest(rtl),'results':outcomes})
        assert results[0]['results']==results[1]['results'],'SFC/native ledger mismatch'
        faults=[]
        for fault,original,replacement in [('wrong-mask','store.w.mask <= mask','store.w.mask <= UInt<1>(1)'),('sibling-data-crosswire','right.data <= xor(in.bits.add, UInt<8>(90))','right.data <= in.bits.add')]:
            directory=case/fault;directory.mkdir(exist_ok=True)
            source=(oracle/'input.fir').read_text();assert source.count(original)==1
            (directory/'input.fir').write_text(source.replace(original,replacement))
            code,_=run([args.compiler,directory/'input.fir','--annotation-file',oracle/'input.anno.json','--output-dir',directory,'--multi-thread-fame5-models'],directory,'compiler');assert code==0
            # The positive-control name is replaced by a negative sentinel so
            # actual compiled faults must fail the same independent ledger.
            outcomes=simulate(directory,directory/'post-fame5-models.sv',n,('actual-'+fault,))
            faults.append({'fault':fault,'status':outcomes['actual-'+fault],'input_sha256':digest(directory/'input.fir'),'rtl_sha256':digest(directory/'post-fame5-models.sv')})
        if n==3:
            directory=case/'frozen-write-base';directory.mkdir(exist_ok=True)
            source=(native/'post-fame5-models.sv').read_text()
            corrupted,count=re.subn(r'(store_base_addr_counter_1 <=\s*)[^;]+;',r"\g<1>3'h0;",source)
            assert count==1,'missing uniquely identified write-accessor base counter'
            rtl=directory/'post-fame5-models.sv';rtl.write_text(corrupted)
            outcomes=simulate(directory,rtl,n,('actual-frozen-write-base',))
            faults.append({'fault':'actual emitted write-accessor base counter frozen at0','status':outcomes['actual-frozen-write-base'],'rtl_sha256':digest(rtl)})
        receipt['cases'].append({'threads':n,'same_input_sha256':digest(oracle/'input.fir'),'same_annotations_sha256':digest(oracle/'input.anno.json'),'annotation_relation':'exact retained annotations after discharging SFC generated ThreadedMem InlineAnnotation by direct native banking','syntax':True,'results':results,'actual_compiled_faults':faults})
    receipt['passed']=True;receipt['gaps']=['generalized target clocks and arbitrary-X selector startup proof','recursive production Linux representative','separate Jasper LEC AND SEC and actual F2 feature receipts']
    (args.output_dir/'comparison.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({'passed':True,'cases':len(receipt['cases']),'receipt':str(args.output_dir/'comparison.json')}))
if __name__=='__main__':main()
