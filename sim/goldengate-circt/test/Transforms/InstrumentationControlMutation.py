#!/usr/bin/env python3
# See LICENSE for license details.
"""Require a type-correct actual emitted Print payload corruption to fail replay."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['native','baseline-comparison','evidence','verilator']:
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    text=a.native.read_text()
    needle="     hBits_record0_argument,"
    assert text.count(needle)==1,'expected one actual Print record packing lane'
    changed=text.replace(needle,"     (hBits_record0_argument ^ 5'h1),",1)
    rtl=out/'crosswire.sv';rtl.write_text(changed)
    driver=out/'driver.cpp';shutil.copy2(a.baseline_comparison/'native.cpp',driver)
    obj=out/'obj';cmd=[str(a.verilator.resolve()),'--cc','--exe','--build','-j','1','-Wno-fatal','--assert','--top-module','GGSelectedPrintCPUStreamPlatformWrapper','--prefix','VWidget','--Mdir',str(obj),str(rtl),str(driver)]
    commands=[cmd]
    with (out/'build.log').open('w')as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
    cmd=[str(obj/'VWidget'),'+verilator+seed+101'];commands.append(cmd)
    with (out/'trace').open('w')as trace,(out/'run.log').open('w')as log:
        run=subprocess.run(cmd,stdout=trace,stderr=log)
    def cycles(path):return [line for line in path.read_text().splitlines()if line and line[0].isdigit()]
    mismatch=cycles(out/'trace')!=cycles(a.baseline_comparison/'sfc.trace')
    assert run.returncode!=0 or mismatch,'actual Print argument corruption escaped complete-network comparison'
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    files=[a.native,rtl,driver,out/'trace',out/'commands.json',Path(__file__).resolve()]
    result=dict(passed=True,full_feature_acceptance=False,control='type-correct actual emitted Print signed argument bit corruption',syntax_compiled=True,returncode=run.returncode,cycle_mismatch=mismatch,sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in files})
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual Print argument corruption rejected',run.returncode,mismatch)

if __name__=='__main__':main()
