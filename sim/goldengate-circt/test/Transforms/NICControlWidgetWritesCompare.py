#!/usr/bin/env python3
"""Compare production NIC widget write bindings and the cloned binding slice.

Dispatch AW/W, NIC done and AR/B/R are explicit boundaries. Global trackers,
response arbitration, host memory/platform integration and NIC runtime remain
pending. SFC register and MCRFile operations are retained verbatim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlCompare import compare_rtl
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('dispatch', 'candidate', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    run([a.export_tool, a.dispatch, out/'attached.mlir', out/'binding.mlir'], 'production-export')
    before, after = [modules(x.read_text()) for x in (a.dispatch, a.candidate)]
    assert after == modules((out/'attached.mlir').read_text()), 'exported composition differs from compiler'
    old, new = 'GGControlWriteDispatchWrapper', 'GGControlWidgetWriteWrapper'
    assert set(after)-set(before) == {new}
    assert all(after[n] == body for n, body in before.items()), 'prior module body changed'
    top = after[new]
    rows = re.findall(r'\{name = "(\w+)", port = "(\w+)", slave = (\d+) : i32\}',
                      re.search(r'goldengate.controlWriteBindings = (\[.*?\])', top)[1])
    bindings = [(name, port, int(slave)) for name, port, slave in rows]
    assert bindings == [('TracerVBridgeModule_0', 'tracerv_ctrl', 2),
                        ('LoadMemWidget_0', 'loadmem_ctrl', 4),
                        ('PeekPokeBridgeModule_0', 'peekPokeBridge_ctrl', 5),
                        ('UARTBridgeModule_0', 'uartBridge_ctrl', 6),
                        ('ClockBridgeModule_0', 'clockBridge_ctrl', 8),
                        ('ResetPulseBridgeModule_0', 'resetBridge_ctrl', 11),
                        ('CPUManagedStreamEngine_0', 'cpuStream_ctrl', 10),
                        ('SimpleNICBridgeModule_0', 'nicBridge_ctrl', 7)]
    control_names = {port for _, port, _ in bindings}
    slaves = {slave for _, _, slave in bindings}
    consumed = {'ctrl_write_dispatch_slave_'+str(slave)+'_'+field
                for slave in slaves for field in ('aw_ready','w_ready','aw_valid','aw_bits_addr',
                    'aw_bits_len','aw_bits_id','w_valid','w_bits_data','w_bits_last')}
    prior = native_ports(before[old]); current = native_ports(top)
    copied = [(d,n,t) for d,n,t in prior if n not in consumed]
    remain = dict((n,t) for d,n,t in current if n in control_names)
    for port in control_names:
        assert remain[port].startswith('!firrtl.bundle<b flip:') and 'aw:' not in remain[port] and ', w:' not in remain[port]
    expected_ports = [(d,n,remain[n] if n in control_names else t) for d,n,t in copied]
    metadata = [('aw', f,w) for f,w in [('size',3),('burst',2),('lock',1),('cache',4),('prot',3),('qos',4),('region',4),('user',1)]]
    metadata += [('w',f,w) for f,w in [('id',12),('strb',4),('user',1)]]
    expected_ports += [('in','ctrl_write_dispatch_master_'+ch+'_bits_'+f,f'!firrtl.uint<{w}>') for ch,f,w in metadata]
    assert current == expected_ports, 'partial bundle or remaining dispatch port differs'
    expected = {}
    for d,n,t in copied:
        if n in control_names:
            for ch in ('ar','b','r'):
                expected[('sim.' if ch=='ar' else 'top.')+n+'.'+ch] = ('top.' if ch=='ar' else 'sim.')+n+'.'+ch
        else:
            expected[('sim.' if d=='in' else 'top.')+n] = ('top.' if d=='in' else 'sim.')+n
    for _,port,slave in bindings:
        for ch, fields, dispatched in (
                ('aw',('addr','len','size','burst','lock','cache','prot','qos','region','id','user'),{'addr','len','id'}),
                ('w',('data','last','id','strb','user'),{'data','last'})):
            prefix = f'sim.ctrl_write_dispatch_slave_{slave}_{ch}'
            bank = f'sim.{port}.{ch}'
            expected[prefix+'_ready'] = bank+'.ready'
            expected[bank+'.valid'] = prefix+'_valid'
            for field in fields:
                expected[bank+'.bits.'+field] = prefix+'_bits_'+field if field in dispatched else f'top.ctrl_write_dispatch_master_{ch}_bits_{field}'
    assert wiring(top) == expected, 'production requests or ready connect to wrong bank'
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    def transfer(v):
        if isinstance(v,str):
            if v == '~'+old: return '~'+new
            if not v.startswith('~'+old+'|'): return v
            suffix = v[len(old)+1:]; prefix = '|'+old+'>'
            if suffix.startswith(prefix):
                ref = suffix[len(prefix):]; port = re.split(r'[.\[]',ref)[0]
                moved = port in {n for _,n,_ in copied}
                if port in control_names:
                    sub = ref[len(port):]
                    moved = any(sub=='.'+ch or sub.startswith('.'+ch+'.') for ch in ('ar','b','r'))
                if moved: suffix = '|'+new+'>'+ref
            return '~'+new+suffix
        if isinstance(v,list): return [transfer(x) for x in v]
        if isinstance(v,dict): return {k:transfer(x) for k,x in v.items()}
        return v
    assert annos(a.candidate) == transfer(annos(a.dispatch)), 'recursive annotation transfer differs'
    stand = modules((out/'binding.mlir').read_text())
    for name in ('GGNICMMIOBank','GGNICMCRFile'):
        assert stand[name] == after[name], 'production helper rebuilt'
    def isolated(value):
        if value.startswith('sim.nicBridge_ctrl'):
            return 'crFile.nasti'+value[len('sim.nicBridge_ctrl'):]
        if value.startswith('sim.ctrl_write_dispatch_slave_7_'):
            return 'top.'+value[len('sim.'):]
        return value
    slice_expected = {isolated(d):isolated(s) for d,s in expected.items()
                      if d.startswith(('sim.nicBridge_ctrl','sim.ctrl_write_dispatch_slave_7_','top.nicBridge_ctrl'))
                      or s.startswith('sim.nicBridge_ctrl')}
    assert len(slice_expected) == 23
    for d,n,t in native_ports(after['GGNICMMIOBank'])[:10]:
        external = 'top.'+{'clock':'hostClock','reset':'hostReset'}.get(n,n)
        slice_expected['nicRegisters.'+n if d=='in' else external] = external if d=='in' else 'nicRegisters.'+n
    slice_expected.update({'crFile.clock':'top.hostClock','crFile.reset':'top.hostReset','crFile.mcr':'nicRegisters.mcr'})
    assert wiring(stand['GGNICControlWriteBindingTest']) == slice_expected, 'exported binding slice differs from production connections'
    # SFC Widget.scala connects sorted widgets to these exact allocated lanes.
    _, sfc_top = module(clean(a.golden.read_text()), 'FPGATop'); eq = equations(sfc_top)
    for name,port,slave in bindings:
        for field in ('aw_valid','aw_bits_addr','aw_bits_len','aw_bits_id','w_valid','w_bits_data'):
            assert eq[name+'_io_ctrl_'+field] == f'ctrlInterconnect_io_slaves_{slave}_{field}'
        for field in ('aw_ready','w_ready'):
            assert eq[f'ctrlInterconnect_io_slaves_{slave}_{field}'] == name+'_io_ctrl_'+field
    run(['firtool',out/'binding.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'], 'firtool')
    def canonical(name):
        for prefix in ('ctrl_write_dispatch_slave_7_', 'ctrl_write_dispatch_master_', 'nicBridge_ctrl_'):
            if name.startswith(prefix): return name[len(prefix):]
        return {'hostClock':'clock','hostReset':'reset'}.get(name,name)
    constants, simulation = compare_rtl(out/'native.sv',a.golden,out,run,'GGNICControlWriteBindingTest',canonical)
    def artifact(path): return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(golden=artifact(a.golden),candidate=artifact(a.candidate),result='matched',
                  preserved_modules=len(before),bindings=bindings,copied_ports=len(copied),connections=len(expected),
                  annotations=len(annos(a.candidate)),sfc_allocation_connections=64,
                  cloned_binding_scalar_connections=20,cloned_binding_bundle_connections=3,
                  response_constants=constants,simulation=simulation,limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    print(simulation)


if __name__ == '__main__': main()
