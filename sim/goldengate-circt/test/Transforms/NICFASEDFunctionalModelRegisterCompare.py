#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the production NIC functional-model MMIO bank against the SFC cone.

Clone the actual bank and four production wrapper bindings, and execute SFC
register updates, full-word readback and bit-zero ingress assignment verbatim.
Decoded MCR inputs replace the decoder; response transport and ingress queue
execution remain separate boundaries. Constant handshakes are checked against
MCRIO.bindReg because SFC optimizes them away.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

HELPER = 'GGFASEDFunctionalModelRegister'
OLD, NEW = 'GGFASEDLatencyRegistersWrapper', 'GGFASEDFunctionalModelRegisterWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-latency-registers.mlir').read_text())
    text = (directory/'post-fame-fased-functional-model-register.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items()), 'prior module changed'
    copied = [p for p in native_ports(before[OLD]) if p[1] != 'fased_ingress_relaxed']
    ports = native_ports(after[NEW])
    assert ports[:-1] == copied
    assert ports[-1] == native_ports(reference[NEW])[-1]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied}
    expected.update({'functionalModelRegister.clock': 'top.hostClock',
                     'functionalModelRegister.reset': 'top.hostReset',
                     'sim.fased_ingress_relaxed': 'functionalModelRegister.relaxed',
                     'top.fased_functional_model_mcr': 'functionalModelRegister.mcr'})
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-latency-registers-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-functional-model-register-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', after[HELPER]) == [
        ('relaxFunctionalModel', '72')]
    return dict(unchanged_modules=len(before), wrapper_connections_compared=len(expected),
                copied_ports=len(copied), consumed_ingress_relaxation_inputs=1,
                annotation_records_retained=len(new), register_offsets_and_permissions_matched=True)


def oracle(path):
    host = module(clean(path.read_text()), 'FASEDMemoryTimingModel')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(host).items()}
    shell = '''module SFCFunctionalModelRegister(input clock,reset,
input crFile_io_mcr_write_18_valid,input [31:0] crFile_io_mcr_write_18_bits,
output [31:0] crFile_io_mcr_read_18_bits,output ingress_io_relaxed);
'''
    declaration = re.findall(r'\breg \[31:0\] relaxFunctionalModel;', host)
    assert len(declaration) == 1
    shell += declaration[0]+'\n'
    for dest, source in (('crFile_io_mcr_read_18_bits', 'relaxFunctionalModel'),
                         ('ingress_io_relaxed', 'relaxFunctionalModel[0]')):
        assert eq[dest] == source
        assignment = re.findall(r'\bassign '+dest+r' = .*?;', host, re.S)
        assert len(assignment) == 1
        shell += assignment[0]+'\n'
    update = re.findall(r'if \(reset\) begin\s*relaxFunctionalModel <= 32\x27h0;\s*end else if '
                        r'\(crFile_io_mcr_write_18_valid\) begin\s*relaxFunctionalModel <= '
                        r'crFile_io_mcr_write_18_bits;\s*end', host)
    assert len(update) == 1
    begin = host.rfind('always @(posedge clock)', 0, host.index(update[0]))
    assert begin >= 0 and 'always @' not in host[begin+len('always @(posedge clock)'):host.index(update[0])]
    return shell+'always @(posedge clock) begin\n'+update[0]+'\nend\nendmodule\n'


def allocation(candidate, golden):
    region = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}', candidate.read_text())
    assert region and len(set(region)) == 1, 'FASED allocation copies disagree'
    size, slave, base = map(int, region[0])
    header = golden.with_suffix('.const.h')
    addresses = re.findall(r'R"ESC\(relaxFunctionalModel\)ESC", (\d+)', header.read_text())
    assert len(addresses) == 2 and all(int(a) == base+72 for a in addresses)
    assert 72+4 <= size
    return dict(golden=str(header.resolve()), golden_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                fased_base=base, fased_region_size=size, fased_slave=slave,
                register=dict(name='relaxFunctionalModel', offset=72, absolute_address=base+72,
                              readable=True, writeable=True), consistent_region_table_copies=len(region),
                scope='allocated region/registry compared; decoded fragment remains external')


def differential(candidate, cone, export_tool, out):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'functional-model-register.mlir', out/'functional-model-register.sv'
    run([export_tool, candidate, exported], 'export')
    text = exported.read_text().replace('firrtl.module public @', 'firrtl.module @')
    isolated = modules(text)
    assert set(isolated) == {HELPER, 'GGFASEDFunctionalModelRegisterProbe'}
    assert isolated[HELPER] == modules(candidate.read_text())[HELPER], 'export rebuilt production bank'
    assert wiring(text, 'GGFASEDFunctionalModelRegisterProbe', aggregate=True) == {
        'functionalModelRegister.clock': 'top.clock', 'functionalModelRegister.reset': 'top.reset',
        'top.relaxed': 'functionalModelRegister.relaxed', 'top.mcr': 'functionalModelRegister.mcr'}
    production = candidate.read_text(); body = modules(production)[NEW]
    # Type-correct mutations are rejected after the exporter verifies the whole IR.
    mutations = [
        ('model-reset', body.replace('firrtl.strictconnect %functionalModelRegister_reset, %hostReset',
                                    'firrtl.strictconnect %functionalModelRegister_reset, %sim_fased_model_reset'),
         'functional model register must use host clock/reset'),
        ('wrong-relaxation', body.replace('firrtl.strictconnect %sim_fased_ingress_relaxed, %functionalModelRegister_relaxed',
                                         'firrtl.strictconnect %sim_fased_ingress_relaxed, %sim_fased_model_reset'),
         'wrong ingress relaxation source'),
        ('missing-mcr', re.sub(r'^      firrtl.connect %fased_functional_model_mcr,.*\n', '', body, flags=re.M),
         'missing production driver')]
    for name, changed, diagnostic in mutations:
        assert changed != body
        bad = out/(name+'.mlir'); bad.write_text(production.replace(body, changed))
        with (out/(name+'.log')).open('w') as log:
            rejected = subprocess.run([str(export_tool), str(bad), str(out/(name+'-probe.mlir'))],
                                      stdout=log, stderr=subprocess.STDOUT)
        assert rejected.returncode != 0 and diagnostic in (out/(name+'.log')).read_text()
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    (out/'oracle.sv').write_text(cone)
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0;
reg mcr_write_0_valid=0,mcr_read_0_ready=0;
reg [31:0] mcr_write_0_bits=0;
reg [3:0] mcr_wstrb=0;
wire [31:0] mcr_read_0_bits,sRead;
wire mcr_read_0_valid,mcr_write_0_ready,relaxed,sRelaxed;
GGFASEDFunctionalModelRegisterProbe candidate(.*);
SFCFunctionalModelRegister oracle(.clock(clock),.reset(reset),
 .crFile_io_mcr_write_18_valid(mcr_write_0_valid),.crFile_io_mcr_write_18_bits(mcr_write_0_bits),
 .crFile_io_mcr_read_18_bits(sRead),.ingress_io_relaxed(sRelaxed));
reg [31:0] state=0;
reg [63:0] rng=64'h209abcd;
reg [15:0] strobes=0;
reg [1:0] writeStates=0,relaxedStates=0;
integer cycles=0,writes=0,zeroStrobe=0,resetWrites=0,upperBits=0,stallWrites=0,readBackpressure=0,evenNonzero=0;
task compare;
begin
 if(mcr_read_0_bits!==sRead || mcr_read_0_bits!==state ||
    relaxed!==sRelaxed || relaxed!==state[0])
  $fatal(1,"functional-model register readback/ingress mismatch cycle=%0d",cycles);
 if(!mcr_read_0_valid||!mcr_write_0_ready)
  $fatal(1,"MCRIO.bindReg valid/ready mismatch");
 if(candidate.functionalModelRegister.relaxFunctionalModel!==oracle.relaxFunctionalModel)
  $fatal(1,"functional-model register state mismatch cycle=%0d",cycles);
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 strobes[mcr_wstrb]=1;writeStates[mcr_write_0_valid]=1;
 writes+=mcr_write_0_valid;
 zeroStrobe+=mcr_write_0_valid&&mcr_wstrb==0;
 resetWrites+=mcr_write_0_valid&&reset;
 stallWrites+=mcr_write_0_valid&&!targetFire;
 upperBits+=mcr_write_0_valid&&mcr_write_0_bits>1;
 readBackpressure+=!mcr_read_0_ready;
 if(reset)state=0;else if(mcr_write_0_valid)state=mcr_write_0_bits;
 clock=1;#2;compare();relaxedStates[relaxed]=1;
 evenNonzero+=state!=0&&!state[0];
 clock=0;#2;cycles++;
end endtask
initial begin
 reset=1;step();reset=0;
 for(integer strobe=0;strobe<16;strobe++)for(integer valid=0;valid<2;valid++)begin
  mcr_wstrb=4'(strobe);mcr_write_0_valid=valid[0];
  mcr_write_0_bits=32'hffffffff;step();
 end
 mcr_write_0_valid=1;
 for(integer value=0;value<8;value++)begin
  case(value)
   0:mcr_write_0_bits=0;1:mcr_write_0_bits=1;2:mcr_write_0_bits=2;3:mcr_write_0_bits=3;
   4:mcr_write_0_bits=32'h80000000;5:mcr_write_0_bits=32'h80000001;
   6:mcr_write_0_bits=32'hffffffff;7:mcr_write_0_bits=32'h10000;
  endcase
  step();
 end
 for(integer i=0;i<32768;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  targetFire=rng[0];reset=i%31==0;mcr_write_0_valid=rng[1];mcr_read_0_ready=rng[2];
  mcr_wstrb=rng[11:8];mcr_write_0_bits=rng[31:0];step();
 end
 if(writes<1000||zeroStrobe<100||resetWrites<100||upperBits<1000||stallWrites<1000||
    readBackpressure<1000||evenNonzero<1000||strobes!=16'hffff||writeStates!=2'b11||relaxedStates!=2'b11)
  $fatal(1,"missing functional-model register coverage");
 $display("PASS cycles=%0d writes=%0d zero_strobe_writes=%0d reset_writes=%0d upper_bits=%0d target_stall_writes=%0d read_backpressure=%0d even_nonzero_relaxation_disabled=%0d strobes=%0d write_states=%0d relaxed_states=%0d",
 cycles,writes,zeroStrobe,resetWrites,upperBits,stallWrites,readBackpressure,evenNonzero,strobes,writeStates,relaxedStates);
 $finish;
end endmodule
'''
    (out/'check.sv').write_text(tb)
    run(['verilator', '--binary', '--assert', '-j', '4', '--timing', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_bank_cloned_verbatim=True, production_four_wrapper_bindings_executed=True,
                original_sfc_register_update_readback_and_ingress_assignment_executed=True,
                valid_IR_production_binding_mutations_rejected=len(mutations),
                outputs_and_registers_compared_before_and_after_every_edge=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args(); cone = oracle(a.golden)
    assert ' '.join(cone.split()) == ' '.join(oracle(a.immutable).split())
    candidate = a.candidate_dir/'post-fame-fased-functional-model-register.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['FASEDMemoryTimingModel'], allocated_register_ABI=allocation(candidate, a.golden),
                  rtl_differential=differential(candidate, cone, a.export_tool, a.output_dir), limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC host-clock functional-model MMIO, full-word readback, bit-zero ingress and preserved composition')


if __name__ == '__main__': main()
