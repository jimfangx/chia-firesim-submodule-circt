#!/usr/bin/env python3
# See LICENSE for license details.
"""Exercise the enabled source compiler handoff and fail-closed publication.
No platform, formal or Linux acceptance is inferred from these compiler checks.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('compiler',type=Path);p.add_argument('work',type=Path)
    a=p.parse_args();a.work.mkdir(parents=True,exist_ok=True)
    fixture=Path(__file__).with_name('EnabledInstrumentationPipeline.fir')
    annotations=Path(__file__).with_name('EnabledInstrumentationPipeline.anno.json')
    original=json.loads(annotations.read_text());commands=[];cases=[]
    for name in ['enabled','direct-input','no-csv','no-print','missing-print','foreign-constructor','late-module-collision']:
        records=json.loads(json.dumps(original));directory=a.work/name;directory.mkdir(exist_ok=True)
        if name=='no-csv':records=[x for x in records if x['class']!='midas.InternalAutoCounterFirrtlAnnotation']
        if name=='no-print':records=[x for x in records if x['class']!='midas.targetutils.SynthPrintfAnnotation']
        if name=='missing-print':
            next(x for x in records if x['class']=='midas.targetutils.SynthPrintfAnnotation')['target']='~Top|Top>missing'
        if name=='foreign-constructor':records.append(dict(
            **{'class':'firesim.lib.bridgeutils.BridgeIOAnnotation'},target='~Top|Top>foreign',
            widgetClass='test.UnimplementedWidget',widgetConstructorKey={},channelMapping={}))
        source=directory/'input.anno.json';source.write_text(json.dumps(records,indent=2)+'\n')
        input_fir=fixture
        if name=='direct-input':
            input_fir=directory/'input.fir'
            text=fixture.read_text();assert text.count('\"top=%x\\n\", printValue)')==1
            input_fir.write_text(text.replace('\"top=%x\\n\", printValue)','\"top=%x\\n\", topEvent)'))
        if name=='late-module-collision':
            input_fir=directory/'input.fir'
            input_fir.write_text(fixture.read_text()+'  module GGCPUStreamPortAdapter :\n    input clock : Clock\n')
        output=directory/'output'
        command=[str(a.compiler.resolve()),str(input_fir.resolve()),'--annotation-file',str(source.resolve()),'--output-dir',str(output.resolve()),'--synthesize-instrumentation-source-control']
        commands.append(command);r=subprocess.run(command,text=True,capture_output=True,timeout=120)
        (directory/'stdout.log').write_text(r.stdout);(directory/'stderr.log').write_text(r.stderr)
        products=list(output.glob('post-instrumentation-control*'))
        if name in ['enabled','direct-input']:
            assert r.returncode==0,r.stdout+r.stderr
            ir=(output/'post-instrumentation-control.mlir').read_text()
            assert 'goldengate.instrumentationGlobalControl'in ir and 'totalCredits'in ir and 'totalDebits'in ir
            retained=json.loads((output/'post-instrumentation-control-all.json').read_text())
            bridges=[x for x in retained if x['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
            assert [x['widgetClass'].rsplit('.',1)[-1]for x in bridges]==['AutoCounterBridgeModule','AutoCounterBridgeModule','PrintBridgeModule']
            assert not any(x['class']in ('midas.InternalAutoCounterFirrtlAnnotation','midas.targetutils.SynthPrintfAnnotation','midas.InternalTriggerSourceAnnotation')for x in retained)
            assert (output/'post-instrumentation-control.sv').is_file()and(output/'print-bridge-decoders.h').is_file()
        else:
            assert r.returncode==1 and r.stderr and not products,(name,r.stdout,r.stderr,products)
        cases.append(dict(case=name,passed=True,returncode=r.returncode,products=len(products)))
    (a.work/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    report=dict(passed=True,full_feature_acceptance=False,scope='enabled source compiler stage and fail-closed late/early publication',cases=cases,
        sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in [a.compiler,fixture,annotations,Path(__file__)]})
    (a.work/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS enabled source-to-FAME/shared-control/CPU compiler including direct-input Print and five early/late rejection cases')


if __name__=='__main__':main()
