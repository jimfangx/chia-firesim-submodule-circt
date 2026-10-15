#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the production NIC FASED MCRFile against archived SFC RTL.

Clone the actual adapter, retaining its state and ports. SFC has optimized
constant bank handshakes and zero histogram bins away; specialize those inputs
only in the probe. This checks local transport, not NIC platform binding.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

from NICMMIOBankCompare import modules
from NICFASEDMMIOBankCompare import oracle as register_abi
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, module
from PrintRocketControlMasterCompare import native_ports, control_type
from PrintRocketFASEDControlCompare import mcr_semantics, transfer
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintSelectedFASEDControlCompare import oracle
from PrintSelectedFASEDTimingCycleCompare import transfer as retarget

OLD, NEW, MCR = 'GGFASEDMMIOWrapper', 'GGFASEDBridgeControlWrapper', 'GGFASEDMCRFile'


def composition(directory):
    before = (directory/'post-fame-fased-mmio.mlir').read_text()
    text = (directory/'post-fame-fased-control.mlir').read_text()
    prior, after = modules(before), modules(text)
    assert set(after)-set(prior) == {NEW, MCR}
    assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
    copied = [p for p in native_ports(prior[OLD]) if p[1] != 'fasedBridge_mcr']
    assert native_ports(after[NEW]) == copied+[('in', 'fasedBridge_ctrl', control_type())]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied}
    expected.update({'crFile.clock': 'top.hostClock', 'crFile.reset': 'top.hostReset',
                     'crFile.nasti': 'top.fasedBridge_ctrl', 'crFile.mcr': 'sim.fasedBridge_mcr'})
    assert wiring(after[NEW]) == expected
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', after[NEW])
    assert text.splitlines()[1] == transfer(before.splitlines()[1], OLD, NEW, [(d, n) for d, n, _ in copied])
    a = json.loads((directory/'post-fame-fased-mmio-all.json').read_text())
    b = json.loads((directory/'post-fame-fased-control-all.json').read_text())
    assert retarget(a, OLD, NEW, {n for _, n, _ in copied}) == b
    mcr_semantics(text)
    return dict(unchanged_modules=len(prior), copied_typed_ports=len(copied), connections=len(expected),
                annotation_records=len(b), state_registers=9, reset_flags=4, index_bits='addr[6:2]')


def execute(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name, check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=check,
                                  preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    exported, rtl = out/'adapter.mlir', out/'adapter.sv'
    run([export_tool, candidate, exported], 'export')
    clone = exported.read_text().replace('firrtl.module public @', 'firrtl.module @')
    assert modules(clone)[MCR] == modules(candidate.read_text())[MCR], 'export rebuilt production adapter'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', rtl], 'firtool')
    h, body = module(clean(golden.read_text()), 'MCRFile_5')
    (out/'oracle.sv').write_text('module SFCMCRFile('+h+');'+body+'endmodule\n')
    nh, _ = module(clean(rtl.read_text()), MCR)
    native, sfc = rtl_ports(nh), rtl_ports(h)
    assert all(native[k.removeprefix('io_')] == spec for k, spec in sfc.items())
    declarations, native_pins, sfc_pins, checks = [], [], [], []
    for pin, (direction, width) in native.items():
        width_text = f'[{width-1}:0] ' if width > 1 else ''
        if direction == 'input':
            declarations.append(f'reg {width_text}{pin}=0;')
            source = pin
        else:
            declarations.append(f'wire {width_text}n_{pin};')
            source = 'n_'+pin
        native_pins.append(f'.{pin}({source})')
    for pin, (direction, width) in sfc.items():
        local = pin.removeprefix('io_')
        if direction == 'input': source = local
        else:
            source = 's_'+local
            declarations.append(f'wire [{width-1}:0] {source};')
            # Payload registers are intentionally unreset in Scala. Compare
            # their values only when the corresponding transaction is valid.
            condition = '1'
            if local.startswith('nasti_r_bits_'): condition = 's_nasti_r_valid'
            if local == 'nasti_b_bits_id': condition = 's_nasti_b_valid'
            if local.startswith('mcr_write_') and local.endswith('_bits'):
                condition = 's_'+local.removesuffix('_bits')+'_valid'
            checks.append(f'if ({condition} && n_{local} !== {source}) $fatal(1,"cycle %0d {local}",cycle);')
        sfc_pins.append(f'.{pin}({source})')
    tb = 'module check;\n'+'\n'.join(declarations)+'\n'
    tb += MCR+' native('+','.join(native_pins)+');\nSFCMCRFile oracle('+','.join(sfc_pins)+');\n'
    tb += '''integer cycle=0,reads=0,writes=0,awFirst=0,wFirst=0,readStalls=0,writeStalls=0;
integer word,phase;
reg [31:0] seed=32'h215fa5ed,seenRead=0,seenWrite=0;
reg [4:0] readIndex=0,writeIndex=0;
reg haveAW=0,haveW=0;
function automatic [31:0] randomWord();
  seed=seed^(seed<<13);seed=seed^(seed>>17);seed=seed^(seed<<5);return seed;
endfunction
task automatic compare;
'''+ '\n'.join(checks)+'''
if (n_nasti_r_bits_resp!==0 || n_nasti_b_bits_resp!==0 || n_nasti_r_bits_last!==1 ||
    n_nasti_r_bits_user!==0 || n_nasti_b_bits_user!==0 || n_mcr_wstrb!==0)
  $fatal(1,"constant response metadata/strobes");
endtask
task automatic tick;
  #1;if(cycle>=2)compare();
  if(!reset)begin
    if(s_nasti_aw_ready && nasti_aw_valid)begin
      awFirst+=!haveW && !(s_nasti_w_ready && nasti_w_valid);
      haveAW=1;writeIndex=nasti_aw_bits_addr[6:2];seenWrite[writeIndex]=1;
    end
    if(s_nasti_w_ready && nasti_w_valid)begin
      wFirst+=!haveAW;haveW=1;
    end
    if(s_nasti_ar_ready && nasti_ar_valid)begin readIndex=nasti_ar_bits_addr[6:2];seenRead[readIndex]=1;end
    reads+=s_nasti_r_valid && nasti_r_ready;
    writes+=s_nasti_b_valid && nasti_b_ready;
    readStalls+=s_nasti_r_valid && !nasti_r_ready;
    writeStalls+=s_nasti_b_valid && !nasti_b_ready;
    if(s_nasti_b_valid && nasti_b_ready)begin haveAW=0;haveW=0;end
  end else begin haveAW=0;haveW=0;end
  #1;clock=1;#1;clock=0;cycle++;
endtask
initial begin
'''
    for i in range(21):
        tb += f'mcr_read_{i}_valid=1;mcr_write_{i}_ready=1;\n'
    tb += '''reset=1;tick();tick();reset=0;
// Each local index, both AW/W orders, high address aliases and byte offsets.
// Reset interrupts every transaction after exercising held or stalled state.
for(word=0;word<32;word++)for(phase=0;phase<2;phase++)begin
  reset=1;tick();reset=0;
  nasti_aw_valid=0;nasti_w_valid=0;nasti_ar_valid=1;
  nasti_r_ready=0;nasti_b_ready=0;
  nasti_ar_bits_addr=25'h1fff000+(word<<2)+phase;
  nasti_aw_bits_addr=25'h80+(word<<2)+phase;
  nasti_aw_bits_id=randomWord();nasti_ar_bits_id=randomWord();nasti_w_bits_data=randomWord();
  if(phase==0)nasti_aw_valid=1;else nasti_w_valid=1;
  tick();nasti_ar_valid=0;nasti_aw_valid=0;nasti_w_valid=0;
  repeat(3)tick();
  if(phase==0)nasti_w_valid=1;else nasti_aw_valid=1;
  tick();nasti_aw_valid=0;nasti_w_valid=0;repeat(5)tick();
  nasti_r_ready=1;nasti_b_ready=1;repeat(3)tick();
end
// Independent request timing, concurrent reads/writes, long response stalls,
// reset cancellation and every valid/invalid local index.
repeat(24000)begin
  reset=cycle%137==0;
  nasti_aw_valid=randomWord()%3==0;nasti_w_valid=randomWord()%3==0;nasti_ar_valid=randomWord()%3==0;
  nasti_aw_bits_addr=randomWord();nasti_ar_bits_addr=randomWord();
  nasti_aw_bits_id=randomWord();nasti_ar_bits_id=randomWord();nasti_w_bits_data=randomWord();
  nasti_r_ready=cycle%211>=40 && (randomWord()&1);
  nasti_b_ready=cycle%173>=40 && (randomWord()&1);
  nasti_w_bits_strb=randomWord();nasti_w_bits_last=randomWord();nasti_w_bits_id=randomWord();
'''
    for i in range(21):
        value = '0' if i in (8, 13) else 'randomWord()&3' if i >= 19 else 'randomWord()'
        tb += f'mcr_read_{i}_bits={value};\n'
    tb += '''  tick();
end
if(seenRead!==32'hffffffff || seenWrite!==32'hffffffff || reads<500 || writes<100 ||
   awFirst<50 || wFirst<50 || readStalls<100 || writeStalls<100)$fatal(1,"insufficient coverage");
$display("PASS cycles=%0d reads=%0d writes=%0d AW-first=%0d W-first=%0d R-stalls=%0d B-stalls=%0d all-32-indices",cycle,reads,writes,awFirst,wFirst,readStalls,writeStalls);
$finish;
end
endmodule
'''
    (out/'check.sv').write_text(tb)
    def simulate(source, name, check=True):
        build = out/(name+'-obj')
        run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal',
             '-j', '2', '--Mdir', build, source, out/'oracle.sv', out/'check.sv'], name+'-build')
        return run([build/'Vcheck'], name+'-run', check)
    simulate(rtl, 'compare')
    # Same-width, valid-IR address mutation must be detected by executing RTL.
    text = exported.read_text()
    mutated = text.replace(' 6 to 2 ', ' 7 to 3 ')
    assert mutated != text
    bad, bad_rtl = out/'wrong-index.mlir', out/'wrong-index.sv'
    bad.write_text(mutated)
    run(['firtool', bad, '--format=mlir', '--disable-all-randomization', '-o', bad_rtl], 'mutation-firtool')
    rejected = simulate(bad_rtl, 'mutation', False)
    assert rejected.returncode != 0 and 'cycle ' in (out/'mutation-run.log').read_text(), 'address mutation escaped'
    return dict(golden=str(golden.resolve()), golden_sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
                oracle_module='MCRFile_5', production_adapter=str(candidate.resolve()),
                specialized_bank_inputs='21 ready/valid ones; zero read bins 8/13; two-bit errors 19/20',
                cycles=24898, log=(out/'compare-run.log').read_text(),
                valid_IR_wrong_address_decode_mutation_detected=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'export-tool', 'output-dir'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--header', type=Path, help='explicit separately archived driver header')
    args = parser.parse_args()
    directory, golden, tool, out = [p.resolve() for p in
        (args.candidate_dir, args.golden, args.export_tool, args.output_dir)]
    oracle(golden)
    structure = composition(directory)
    allocation = register_abi(golden, directory/'post-fame-fased-control.mlir', args.header)
    execution = execute(directory/'post-fame-fased-control.mlir', golden, tool, out)
    (out/'comparison.json').write_text(json.dumps(dict(composition=structure, allocation=allocation, execution=execution,
        limitation='Local FASED transport only; NIC slave/platform binding and execution remain pending'), indent=2)+'\n')
    print('PASS production FASED MCRFile composition and SFC RTL transaction differential')


if __name__ == '__main__':
    main()
