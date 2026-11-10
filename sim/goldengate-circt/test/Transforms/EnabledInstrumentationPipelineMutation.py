#!/usr/bin/env python3
# See LICENSE for license details.
"""Check actual source Print/trigger faults and invalid payload qualification."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['native','baseline-comparison','oracle','evidence','verilator']:
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--direct-print-input',action='store_true')
    a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    text=a.native.read_text();commands=[];results=[]
    value='topEvent_1_sink_bits'if a.direct_print_input else'printValue'
    argument=f'assign synthesizedPrintf_report_wire_args_0_source_bits = {value};'
    trigger='trigger_sync <= trigger_source;'
    assert text.count(argument)==1 and text.count(trigger)==1,'expected unique actual target argument/trigger update'
    controls=[('print-argument',text.replace(argument,argument.replace(f'= {value};',f'= {value} ^ 4\'h1;')),True),
              ('trigger-disable',text.replace(trigger,"trigger_sync <= 1'h0;"),True),
              ('invalid-print-argument',text.replace(argument,argument.replace(f'= {value};',f'= synthesizedPrintf_report_wire_args_0_source_valid ? {value} : ~{value};')),False)]
    if a.direct_print_input:
        ready='assign topEvent_1_sink_ready = synthesizedPrintf_report_wire_args_0_source_ready;'
        assert text.count(ready)==1
        controls += [('direct-argument-crosswire',text.replace(argument,argument.replace(value,'Top_topEvent_sink_bits')),True),('direct-ready',text.replace(ready,ready.replace('= synthesizedPrintf_report_wire_args_0_source_ready;',"= 1'b0;")),True)]
    baseline=[x for x in(a.baseline_comparison/'sfc.trace').read_text().splitlines()if x and x[0].isdigit()]
    for name,changed,mustFail in controls:
        directory=out/name;directory.mkdir(exist_ok=True);rtl=directory/'fault.sv';rtl.write_text(changed)
        driver=directory/'driver.cpp';shutil.copy2(a.baseline_comparison/'native.cpp',driver);obj=directory/'obj'
        cmd=[str(a.verilator.resolve()),'--cc','--exe','--build','-j','1','-Wno-fatal','--assert','--top-module','GGSelectedPrintCPUStreamPlatformWrapper','--prefix','VBound','--Mdir',str(obj),str(rtl),str(a.oracle.resolve()/'AbstractClockGate.v'),str(driver)]
        commands.append(cmd)
        with(directory/'build.log').open('w')as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
        cmd=[str(obj/'VBound'),'+verilator+seed+101'];commands.append(cmd)
        with(directory/'trace').open('w')as trace,(directory/'run.log').open('w')as log:r=subprocess.run(cmd,stdout=trace,stderr=log)
        lines=[x for x in(directory/'trace').read_text().splitlines()if x and x[0].isdigit()]
        mismatch=lines!=baseline;failed=r.returncode!=0 or mismatch
        assert failed==mustFail,(name,r.returncode,mismatch)
        results.append(dict(control=name,passed=True,syntax_compiled=True,expected_rejection=mustFail,returncode=r.returncode,cycle_mismatch=mismatch))
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    files=[a.native,Path(__file__),out/'commands.json']+[out/x['control']/'fault.sv'for x in results]+[out/x['control']/'trace'for x in results]
    result=dict(passed=True,full_feature_acceptance=False,scope='compiled emitted source Print argument, direct-loopback handshake and trigger corruption controls',direct_print_input=a.direct_print_input,controls=results,sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in files})
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS actual source Print argument and trigger corruption rejected; invalid argument payload accepted')


if __name__=='__main__':main()
