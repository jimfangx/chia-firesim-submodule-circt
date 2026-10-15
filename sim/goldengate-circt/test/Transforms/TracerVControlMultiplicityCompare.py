#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute four production TracerV MCRFiles against the quad-Rocket SFC RTL.

Checks local bus state and exact per-bank composition. Global control allocation,
stream queues and the quad-core simulator are separate, still pending stages.
"""
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import sys
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer


def composition(prior, text):
    before, after = modules(prior), modules(text)
    expected = set()
    old = 'GGTracerVTriggerWrapper'
    for bank in range(4):
        suffix = f'_{bank}' if bank else ''
        new = 'GGTracerVBridgeControlWrapper' + (f'_{bank}' if bank < 3 else '')
        adapter = 'GGTracerVMCRFile' + suffix
        expected.update((new, adapter))
        original = native_ports(before[old] if bank == 0 else after[old])
        current = native_ports(after[new])
        assert len(original) == len(current)
        connections = {}
        copied = set()
        for (direction, name, typ), (nd, nn, nt) in zip(original, current):
            if name == 'tracerv_mcr' + suffix:
                assert (nd, nn) == ('in', 'tracerv_ctrl' + suffix)
            else:
                assert (direction, name, typ) == (nd, nn, nt)
                copied.add(name)
                connections[('sim.' if direction == 'in' else 'top.') + name] = (
                    'top.' if direction == 'in' else 'sim.') + name
        connections.update({'crFile.clock': 'top.hostClock', 'crFile.reset': 'top.hostReset',
                            'crFile.nasti': 'top.tracerv_ctrl' + suffix,
                            'crFile.mcr': 'sim.tracerv_mcr' + suffix})
        assert wiring(after[new]) == connections, (bank, 'crossed or missing route')
        old = new
    assert set(after) - set(before) == expected
    assert all(after[n] == body for n, body in before.items())
    return len(before)


def main():
    candidate, golden, firtool, out = [Path(p).resolve() for p in sys.argv[1:]]
    out.mkdir(parents=True, exist_ok=False)
    prior = (candidate/'post-fame-tracerv-trigger.mlir').read_text()
    text = (candidate/'post-fame-tracerv-control.mlir').read_text()
    unchanged = composition(prior, text)
    bad = text.replace('firrtl.connect %crFile_mcr, %sim_tracerv_mcr_3',
                       'firrtl.connect %crFile_mcr, %sim_tracerv_ctrl')
    assert bad != text
    try: composition(prior, bad)
    except AssertionError: pass
    else: raise AssertionError('crossed fourth bank accepted')
    ann = json.loads((candidate/'post-fame-tracerv-trigger-all.json').read_text())
    old = 'GGTracerVTriggerWrapper'
    for bank in range(4):
        new = 'GGTracerVBridgeControlWrapper' + (f'_{bank}' if bank < 3 else '')
        copied = {n for _, n, _ in native_ports(modules(text)[new]) if n != 'tracerv_ctrl' + (f'_{bank}' if bank else '')}
        ann = transfer(ann, old, new, copied)
        old = new
    result_ann = json.loads((candidate/'post-fame-tracerv-control-all.json').read_text())
    assert ann == result_ann
    source = clean(golden.read_text())
    _, host = module(source, 'TracerVBridgeModule')
    kind, = re.findall(r'\b(MCRFile\w*) crFile', host)
    header, body = module(source, kind)
    (out/'oracle.sv').write_text('module SFCMCRFile('+header+');'+body+'endmodule\n')
    reference = ports(header)
    def run(cmd, name, check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=check,
                                  preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    native_files = []
    for bank in range(4):
        name = 'GGTracerVMCRFile' + (f'_{bank}' if bank else '')
        mlir, rtl = out/(name+'.mlir'), out/(name+'.sv')
        mlir.write_text('module { firrtl.circuit "'+name+'" {\n'+native_module(text, name)+'\n} }\n')
        run([firtool, mlir, '--format=mlir', '--disable-all-randomization', '-o', rtl], name)
        native_files.append(rtl)
    actual = ports(module(clean(native_files[0].read_text()), 'GGTracerVMCRFile')[0])
    for pin, spec in reference.items(): assert actual[pin.removeprefix('io_')] == spec
    tb = ['module check;', 'integer cycle=0, reads=0, writes=0;',
          "reg [31:0] seed=32'h239ace01;", 'reg [15:0] seenRead[4],seenWrite[4];',
          'function automatic [31:0] randomWord(); seed=seed^(seed<<13);seed=seed^(seed>>17);seed=seed^(seed<<5);return seed;endfunction']
    checks, drive, stats, tick = [], [], [], []
    for bank in range(4):
        pins, refs = [], []
        prefix = f'b{bank}_'
        for pin, (direction, width) in actual.items():
            local = prefix + pin
            tb.append(f'{"reg" if direction == "input" else "wire"} [{width-1}:0] {local}' + ('=0;' if direction == 'input' else ';'))
            pins.append(f'.{pin}({local})')
            if direction == 'input':
                val = 'randomWord()'
                if pin == 'clock': continue
                if pin == 'reset': val = f'(cycle<2 || (cycle+{bank}*19)%137==0)'
                elif pin.endswith('_len'): val = '0'
                elif pin.endswith('_valid') or pin.endswith('_ready'): val = 'randomWord()%3==0'
                if pin.startswith('mcr_'):
                    if pin.endswith('_valid') or pin.endswith('_ready'): val = '1'
                    elif pin.endswith('_bits') and pin not in ('mcr_read_0_bits','mcr_read_1_bits'): val = '0'
                drive.append(local+'='+val+';')
        for pin, (direction, width) in reference.items():
            name = pin.removeprefix('io_'); local = prefix+name
            if direction == 'output':
                ref = prefix+'s_'+name; tb.append(f'wire [{width-1}:0] {ref};')
                valid = '1'
                if name.startswith('nasti_r_bits_'): valid = prefix+'s_nasti_r_valid'
                if name == 'nasti_b_bits_id': valid = prefix+'s_nasti_b_valid'
                if name.startswith('mcr_write_') and name.endswith('_bits'): valid = prefix+'s_'+name.removesuffix('_bits')+'_valid'
                checks.append(f'if({valid} && {ref} !== {local}) $fatal(1,"bank {bank} cycle %0d {name}",cycle);')
                local = ref
            refs.append(f'.{pin}({local})')
        name = 'GGTracerVMCRFile' + (f'_{bank}' if bank else '')
        tb += [name+f' native{bank}('+','.join(pins)+');', 'SFCMCRFile'+f' reference{bank}('+','.join(refs)+');']
        stats += [f'if(!{prefix}reset)begin',
                  f'if({prefix}s_nasti_ar_ready && {prefix}nasti_ar_valid)seenRead[{bank}][{prefix}nasti_ar_bits_addr[5:2]]=1;',
                  f'if({prefix}s_nasti_aw_ready && {prefix}nasti_aw_valid)seenWrite[{bank}][{prefix}nasti_aw_bits_addr[5:2]]=1;',
                  f'reads+={prefix}s_nasti_r_valid && {prefix}nasti_r_ready;', f'writes+={prefix}s_nasti_b_valid && {prefix}nasti_b_ready;', 'end']
        tick += [prefix+'clock=1;']
    tb += ['initial begin', 'for(integer i=0;i<4;i++)begin seenRead[i]=0;seenWrite[i]=0;end',
           'repeat(24000)begin', *drive, '#1;if(cycle>=2)begin', *checks, 'end', *stats,
           '#1;', *tick, '#1;', *[f'b{i}_clock=0;' for i in range(4)], 'cycle++;end',
           'for(integer i=0;i<4;i++)if(seenRead[i]!==16\'hffff || seenWrite[i]!==16\'hffff)$fatal(1,"missing local index");',
           'if(reads<2000 || writes<400)$fatal(1,"insufficient transfers");',
           '$display("PASS four independent MCRFiles cycles=%0d reads=%0d writes=%0d all-16-indices",cycle,reads,writes);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    def simulate(files, name, check=True):
        build = out/(name+'-obj')
        run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal', '-j', '2', '--Mdir', build,
             *files, out/'oracle.sv', out/'check.sv'], name+'-build')
        return run([build/'Vcheck'], name, check)
    simulate(native_files, 'compare')
    # A legal same-width address shift must fail the executed RTL comparison.
    original = (out/'GGTracerVMCRFile.mlir').read_text()
    mutated = original.replace(' 5 to 2 ', ' 6 to 3 ')
    assert original != mutated
    bad_mlir, bad_rtl = out/'wrong-index.mlir', out/'wrong-index.sv'
    bad_mlir.write_text(mutated)
    run([firtool, bad_mlir, '--format=mlir', '--disable-all-randomization', '-o', bad_rtl], 'mutation-firtool')
    failed = simulate([bad_rtl, *native_files[1:]], 'mutation', False)
    assert failed.returncode != 0 and 'bank 0 cycle ' in (out/'mutation.log').read_text()
    report = dict(golden=str(golden), golden_sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
                  oracle_module=kind, unchanged_modules=unchanged,
                  retained_annotations=len(ann), independent_slaves=4, cycles_per_slave=24000,
                  crossed_fourth_bank_detected=True, valid_IR_wrong_address_decode_detected=True, all_16_local_indices=True,
                  specialized_inputs='SFC constant ready/valid=1; write-only read words=0',
                  result=(out/'compare.log').read_text(), limitation='local slaves only; stream storage and global ABI pending')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['result'])


if __name__ == '__main__': main()
