#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production NIC response-error bank with verbatim SFC RTL.

Decoded MCR and observed response inputs isolate the status register boundary;
full control transport, response queues and NIC workloads remain separate.
SFC optimizes R/B ready away; RTL comparison uses its always-ready boundary.
The native CTest separately enumerates stalled handshakes and assertion enables.
Illegal-write predicates/messages match, but SFC prints while CIRCT stops;
this comparison does not establish identical assertion termination policy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

HELPER = 'GGFASEDResponseErrors'
OLD, NEW = 'GGFASEDFunctionalModelRegisterWrapper', 'GGFASEDResponseErrorsWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-functional-model-register.mlir').read_text())
    text = (directory/'post-fame-fased-response-errors.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items()), 'prior module changed'
    copied = native_ports(before[OLD]); ports = native_ports(after[NEW])
    assert ports[:-1] == copied and ports[-1] == native_ports(reference[NEW])[-1]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied}
    expected.update({'responseErrors.clock': 'top.hostClock', 'responseErrors.reset': 'top.hostReset',
                     'top.fased_response_errors_mcr': 'responseErrors.mcr'})
    for prefix, response in (('r', 'read'), ('b', 'write')):
        expected['responseErrors.'+prefix+'Valid'] = 'top.fased_host_'+response+'_response.valid'
        expected['responseErrors.'+prefix+'Ready'] = 'sim.fased_host_'+response+'_response.ready'
        expected['responseErrors.'+prefix+'Resp'] = 'top.fased_host_'+response+'_response.bits.resp'
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-functional-model-register-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-response-errors-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', after[HELPER]) == [
        ('rrespError', '76'), ('brespError', '80')]
    return dict(unchanged_modules=len(before), wrapper_connections_compared=len(expected),
                copied_ports=len(copied), annotation_records_retained=len(new),
                register_offsets_and_read_only_permissions_matched=True)


def oracle(path):
    host = module(clean(path.read_text()), 'FASEDMemoryTimingModel')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(host).items()}
    shell = '''module SFCResponseErrors(input clock,reset,
input auto_to_host_dram_out_r_valid,auto_to_host_dram_out_b_valid,
input [1:0] auto_to_host_dram_out_r_bits_resp,auto_to_host_dram_out_b_bits_resp,
input crFile_io_mcr_write_19_valid,crFile_io_mcr_write_20_valid,
output [31:0] crFile_io_mcr_read_19_bits,crFile_io_mcr_read_20_bits);
'''
    updates, assertions = [], []
    for index, name, response in ((19, 'rrespError', 'r'), (20, 'brespError', 'b')):
        declarations = re.findall(r'\breg \[1:0\] '+name+r';', host); assert len(declarations) == 1
        shell += declarations[0]+'\n'
        dest = f'crFile_io_mcr_read_{index}_bits'
        assert eq[dest] == "{{30'd0}, "+name+'}'
        assignments = re.findall(r'\bassign '+dest+r' = .*?;', host, re.S); assert len(assignments) == 1
        shell += assignments[0]+'\n'
        update = re.findall(r'if \(reset\) begin\s*'+name+r" <= 2'h0;\s*end else if \("
                            r'auto_to_host_dram_out_'+response+r"_bits_resp != 2'h0 & auto_to_host_dram_out_"
                            +response+r'_valid\) begin\s*'+name+r' <= auto_to_host_dram_out_r_bits_resp;\s*end', host)
        assert len(update) == 1
        begin = host.rfind('always @(posedge clock)', 0, host.index(update[0]))
        assert begin >= 0 and 'always @' not in host[begin+len('always @(posedge clock)'):host.index(update[0])]
        updates += update
        assertion = re.findall(r'if \(~reset & ~\(~crFile_io_mcr_write_'+str(index)+r'_valid\)\) begin\s*'
                               r'\$fwrite\(.*?Register '+name+r'.*?\);\s*end', host, re.S)
        assert len(assertion) == 1
        assertions += assertion
    return shell+'always @(posedge clock) begin\n'+'\n'.join(updates+assertions)+'\nend\nendmodule\n'


def allocation(candidate, golden):
    regions = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}', candidate.read_text())
    assert regions and len(set(regions)) == 1, 'FASED allocation copies disagree'
    size, slave, base = map(int, regions[0]); header = golden.with_suffix('.const.h')
    widget, = re.findall(r'registry.add_widget\(new FASEDMemoryTimingModel\((.*?)\n  0,', header.read_text(), re.S)
    tables = re.findall(r'std::vector<std::pair<std::string, uint32_t>>\{(.*?)\n    }', widget, re.S)
    assert len(tables) == 2, 'missing SFC readable/writeable address vectors'
    registers = []
    for name, offset in (('rrespError', 76), ('brespError', 80)):
        addresses = re.findall(r'R"ESC\('+name+r'\)ESC", (\d+)', header.read_text())
        assert len(addresses) == 1 and int(addresses[0]) == base+offset and offset+4 <= size
        assert f'R"ESC({name})ESC"' in tables[0] and f'R"ESC({name})ESC"' not in tables[1]
        registers.append(dict(name=name, offset=offset, absolute_address=base+offset,
                              readable=True, writeable=False))
    return dict(golden=str(header.resolve()), golden_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                fased_base=base, fased_region_size=size, fased_slave=slave, registers=registers,
                consistent_region_table_copies=len(regions), scope='allocated read-only ABI; decoder external')


def differential(candidate, cone, export_tool, out):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    def run(cmd, name, check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=check)
    exported, native = out/'response-errors.mlir', out/'response-errors.sv'
    run([export_tool, candidate, exported], 'export')
    text = exported.read_text().replace('firrtl.module public @', 'firrtl.module @'); isolated = modules(text)
    assert set(isolated) == {HELPER, 'GGFASEDResponseErrorsProbe'}
    assert isolated[HELPER] == modules(candidate.read_text())[HELPER], 'export rebuilt production bank'
    expected = {'responseErrors.'+n: 'top.'+n for n in ('clock','reset','rValid','rReady','bValid','bReady','rResp','bResp')}
    expected['top.mcr'] = 'responseErrors.mcr'
    assert wiring(text, 'GGFASEDResponseErrorsProbe', aggregate=True) == expected
    production = candidate.read_text(); body = modules(production)[NEW]
    # Mutations preserve FIRRTL types and are rejected after whole-IR verification.
    r_code, = re.findall(r'firrtl.strictconnect %responseErrors_rResp, (%\w+)', body)
    r_valid, = re.findall(r'firrtl.strictconnect %responseErrors_rValid, (%\w+)', body)
    mutations = [
        ('model-reset', body.replace('firrtl.strictconnect %responseErrors_reset, %hostReset',
                                    'firrtl.strictconnect %responseErrors_reset, %sim_fased_model_reset'),
         'response errors must use host clock/reset'),
        ('crossed-code', re.sub(r'(firrtl.strictconnect %responseErrors_bResp, )%\w+', lambda m: m[1]+r_code, body),
         'wrong production response code source'),
        ('wrong-ready', re.sub(r'(firrtl.strictconnect %responseErrors_rReady, )%\w+', lambda m: m[1]+r_valid, body),
         'wrong production response handshake source'),
        ('missing-mcr', re.sub(r'^      firrtl.connect %fased_response_errors_mcr,.*\n', '', body, flags=re.M),
         'missing production driver')]
    for name, changed, diagnostic in mutations:
        assert changed != body
        bad = out/(name+'.mlir'); bad.write_text(production.replace(body, changed))
        rejected = run([export_tool, bad, out/(name+'-probe.mlir')], name, check=False)
        assert rejected.returncode != 0 and diagnostic in (out/(name+'.log')).read_text()
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    (out/'oracle.sv').write_text(cone)
    tb = r'''module check;
reg clock=0,reset=0,rValid=0,rReady=1,bValid=0,bReady=1,targetFire=0;
reg [1:0] rResp=0,bResp=0;
reg mcr_write_0_valid=0,mcr_write_1_valid=0,mcr_read_0_ready=0,mcr_read_1_ready=0;
reg [31:0] mcr_write_0_bits=0,mcr_write_1_bits=0;
reg [3:0] mcr_wstrb=0;
wire [31:0] mcr_read_0_bits,mcr_read_1_bits,sR,sB;
wire mcr_read_0_valid,mcr_read_1_valid,mcr_write_0_ready,mcr_write_1_ready;
reg oracleOnly=0,nativeOnly=0;
wire candidateReset=reset|oracleOnly,oracleReset=reset|nativeOnly;
GGFASEDResponseErrorsProbe candidate(.reset(candidateReset),.*);
SFCResponseErrors oracle(.clock(clock),.reset(oracleReset),
 .auto_to_host_dram_out_r_valid(rValid),.auto_to_host_dram_out_b_valid(bValid),
 .auto_to_host_dram_out_r_bits_resp(rResp),.auto_to_host_dram_out_b_bits_resp(bResp),
 .crFile_io_mcr_write_19_valid(mcr_write_0_valid),.crFile_io_mcr_write_20_valid(mcr_write_1_valid),
 .crFile_io_mcr_read_19_bits(sR),.crFile_io_mcr_read_20_bits(sB));
reg [1:0] rState=0,bState=0;
reg [63:0] rng=64'h210abcd;
reg [15:0] codePairs=0;
reg [3:0] valids=0,resetWrites=0;
integer cycles=0,rCaptures=0,bCaptures=0,zeroBCaptures=0,crossedBCaptures=0,stalledTargetCaptures=0,readBackpressure=0;
integer badWrite=0;
task compare;
begin
 if(mcr_read_0_bits!==sR||mcr_read_1_bits!==sB||mcr_read_0_bits!=={30'd0,rState}||mcr_read_1_bits!=={30'd0,bState})
  $fatal(1,"response-error readback mismatch cycle=%0d",cycles);
 if(!mcr_read_0_valid||!mcr_read_1_valid||!mcr_write_0_ready||!mcr_write_1_ready)
  $fatal(1,"MCRIO.bindReg valid/ready mismatch");
 if(candidate.responseErrors.rrespError!==oracle.rrespError||candidate.responseErrors.brespError!==oracle.brespError)
  $fatal(1,"response-error state mismatch cycle=%0d",cycles);
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 codePairs[{rResp,bResp}]=1;valids[{rValid,bValid}]=1;
 readBackpressure+=!mcr_read_0_ready||!mcr_read_1_ready;
 if(reset)begin rState=0;bState=0;resetWrites[{mcr_write_0_valid,mcr_write_1_valid}]=1;end
 else begin
  rCaptures+=rValid&&rResp!=0;bCaptures+=bValid&&bResp!=0;
  zeroBCaptures+=bValid&&bResp!=0&&rResp==0;
  crossedBCaptures+=bValid&&bResp!=0&&rResp!=bResp;
  stalledTargetCaptures+=!targetFire&&((rValid&&rResp!=0)||(bValid&&bResp!=0));
  if(rValid&&rResp!=0)rState=rResp;if(bValid&&bResp!=0)bState=rResp;
 end
 clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 oracleOnly=$test$plusargs("oracle-only");nativeOnly=$test$plusargs("native-only");
 if($value$plusargs("bad-write=%d",badWrite))begin
  reset=1;mcr_write_0_valid=badWrite[0];mcr_write_1_valid=badWrite[1];
  #2;clock=1;#2;clock=0;reset=0;#2;clock=1;#2;$finish;
 end else begin
 reset=1;step();
 for(integer i=0;i<512;i++)begin
  reset=i[0];rValid=i[1];bValid=i[2];rResp=i[4:3];bResp=i[6:5];
  mcr_write_0_valid=reset&&i[7];mcr_write_1_valid=reset&&i[8];step();
 end
 for(integer i=0;i<32768;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  reset=i%31==0;targetFire=rng[0];rValid=rng[1];bValid=rng[2];rResp=rng[4:3];bResp=rng[6:5];
  mcr_read_0_ready=rng[7];mcr_read_1_ready=rng[8];mcr_wstrb=rng[12:9];
  mcr_write_0_valid=reset&&rng[13];mcr_write_1_valid=reset&&rng[14];
  mcr_write_0_bits=rng[31:0];mcr_write_1_bits=rng[63:32];step();
 end
 if(rCaptures<1000||bCaptures<1000||zeroBCaptures<1000||crossedBCaptures<1000||
    stalledTargetCaptures<1000||readBackpressure<1000||codePairs!=16'hffff||valids!=4'hf||resetWrites!=4'hf)
  $fatal(1,"missing response-error coverage");
 $display("PASS cycles=%0d r_captures=%0d b_captures=%0d b_captures_zero_R_code=%0d b_captures_distinct_R_code=%0d target_stall_captures=%0d read_backpressure=%0d code_pairs=%0d valid_pairs=%0d reset_write_pairs=%0d",
 cycles,rCaptures,bCaptures,zeroBCaptures,crossedBCaptures,stalledTargetCaptures,readBackpressure,codePairs,valids,resetWrites);
 $finish;
 end
end endmodule
'''
    (out/'check.sv').write_text(tb)
    run(['verilator', '--binary', '--assert', '-j', '4', '--timing', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    # Each illegal write mask is diagnosed outside reset. The SFC emits $fwrite;
    # CIRCT emits a fatal assertion, so compare the register diagnostics, not exits.
    for mask in (1,2,3):
        native_result = run([out/'obj/check', '+native-only', f'+bad-write={mask}'], f'native-write-{mask}', check=False)
        assert native_result.returncode != 0
        native_diag = (out/f'native-write-{mask}.log').read_text()
        assert 'is read only' in native_diag
        if mask != 3:
            assert f'Register {"rrespError" if mask == 1 else "brespError"} is read only' in native_diag
        run([out/'obj/check', '+oracle-only', f'+bad-write={mask}'], f'oracle-write-{mask}')
        diag = (out/f'oracle-write-{mask}.log').read_text()
        for index, name in enumerate(('rrespError','brespError')):
            assert (f'Register {name} is read only' in diag) == bool(mask & (1<<index))
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_bank_cloned_verbatim=True, production_nine_bindings_validated_and_cloned=True,
                original_sfc_register_updates_readback_and_assertions_executed=True,
                valid_IR_production_binding_mutations_rejected=len(mutations), read_only_write_masks_diagnosed=3,
                outputs_and_registers_compared_before_and_after_every_edge=True,
                illegal_write_termination_policy=dict(sfc='diagnostic then continues', circt='fatal assertion',
                                                      matched=False, scope='predicates/messages compared; termination parity pending'),
                scope='SFC always-ready RTL; native CTest additionally covers ready stalls')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args(); cone = oracle(a.golden)
    assert ' '.join(cone.split()) == ' '.join(oracle(a.immutable).split())
    candidate = a.candidate_dir/'post-fame-fased-response-errors.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['FASEDMemoryTimingModel'], allocated_register_ABI=allocation(candidate, a.golden),
                  rtl_differential=differential(candidate, cone, a.export_tool, a.output_dir), limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC legal response-error state, SFC B capture, read-only MMIO and composition; assertion termination parity pending')


if __name__ == '__main__': main()
