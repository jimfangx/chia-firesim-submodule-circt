#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute production NIC request-limit MMIO against the recorded SFC cone.

Clone the actual native bank and five wrapper bindings (or only the unattached
bank with --materialized-bank, without claiming admission composition). Extract SFC register
declarations, updates, readback and low-bit assignments verbatim. Decoded MCR
inputs replace the control decoder; transport/address allocation and admission
counter execution remain separate boundaries. SFC optimizes constant MCR
handshakes away; native valid/ready are checked against MCRIO.bindReg.
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

HELPER = 'GGFASEDRequestLimits'
OLD, NEW = 'GGFASEDReadAdmissionWrapper', 'GGFASEDRequestLimitsWrapper'


def composition(directory, baseline):
    before_text = (directory/'post-fame-fased-read-admission.mlir').read_text()
    text = (directory/'post-fame-fased-request-limits.mlir').read_text()
    before, after, reference = modules(before_text), modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items()), 'prior module changed'
    consumed = {'fased_write_max_reqs', 'fased_read_max_reqs'}
    copied = [p for p in native_ports(before[OLD]) if p[1] not in consumed]
    ports = native_ports(after[NEW])
    assert ports[:-1] == copied
    assert ports[-1] == native_ports(reference[NEW])[-1]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied}
    expected.update({'requestLimits.clock': 'top.hostClock', 'requestLimits.reset': 'top.hostReset',
                     'sim.fased_write_max_reqs': 'requestLimits.limits.write',
                     'sim.fased_read_max_reqs': 'requestLimits.limits.read',
                     'top.fased_request_limits_mcr': 'requestLimits.mcr'})
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-read-admission-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-request-limits-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', after[HELPER]) == [
        ('writeMaxReqs', '8'), ('readMaxReqs', '12')]
    return dict(unchanged_modules=len(before), wrapper_connections_compared=len(expected),
                copied_ports=len(copied), consumed_admission_maxima=2, annotation_records_retained=len(new),
                register_offsets_and_permissions_matched=True)


def oracle(path):
    host = module(clean(path.read_text()), 'FASEDMemoryTimingModel')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(host).items()}
    widths = re.findall(r'wire \[(\d+):0\] model_tNasti_io_mmReg_(?:write|read)MaxReqs;', host)
    assert len(widths) == 2 and len(set(widths)) == 1
    width = int(widths[0]) + 1
    assert 1 <= width <= 5
    shell = '''module SFCRequestLimits(input clock,reset,
input crFile_io_mcr_write_2_valid,crFile_io_mcr_write_3_valid,
input [31:0] crFile_io_mcr_write_2_bits,crFile_io_mcr_write_3_bits,
output [31:0] crFile_io_mcr_read_2_bits,crFile_io_mcr_read_3_bits,
output [LIMIT_HI:0] model_tNasti_io_mmReg_writeMaxReqs,model_tNasti_io_mmReg_readMaxReqs);
'''.replace('LIMIT_HI', str(width-1))
    for slot, name in ((2, 'writeMaxReqs'), (3, 'readMaxReqs')):
        declaration = re.findall(r'\breg \[31:0\] '+name+r';', host)
        assert len(declaration) == 1; shell += declaration[0]+'\n'
        for dest, source in ((f'crFile_io_mcr_read_{slot}_bits', name),
                             ('model_tNasti_io_mmReg_'+name, name+f'[{width-1}:0]')):
            assert eq[dest] == source
            assignment = re.findall(r'\bassign '+dest+r' = .*?;', host, re.S)
            assert len(assignment) == 1; shell += assignment[0]+'\n'
        update = re.findall(r'if \(reset\) begin\s*'+name+r' <= 32\x27h[0-9a-f]+;\s*end else if \(crFile_io_mcr_write_'+str(slot)+
                            r'_valid\) begin\s*'+name+r' <= crFile_io_mcr_write_'+str(slot)+r'_bits;\s*end', host)
        assert len(update) == 1
        initial = re.search(r'<= 32\x27h([0-9a-f]+);', update[0])
        assert initial and 1 <= int(initial[1], 16) <= 16
        assert width == int(initial[1], 16).bit_length()
        # Preserve the host edge that surrounds the original register update.
        begin = host.rfind('always @(posedge clock)', 0, host.index(update[0]))
        assert begin >= 0 and 'always @' not in host[begin+len('always @(posedge clock)'):host.index(update[0])]
        shell += 'always @(posedge clock) begin\n'+update[0]+'\nend\n'
    return shell+'endmodule\n'


def allocation(candidate, golden):
    text = candidate.read_text()
    region = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}', text)
    # Decoder, response routing and allocation each retain this region table.
    assert region and len(set(region)) == 1, 'FASED allocation copies disagree'
    size, slave, base = map(int, region[0])
    header = golden.with_suffix('.const.h'); reference = header.read_text()
    records = []
    for name, offset in (('writeMaxReqs', 8), ('readMaxReqs', 12)):
        addresses = re.findall(r'R"ESC\('+name+r'\)ESC", (\d+)', reference)
        assert len(addresses) == 2 and all(int(a) == base+offset for a in addresses)
        assert offset+4 <= size
        records.append(dict(name=name, offset=offset, absolute_address=base+offset, readable=True, writeable=True))
    return dict(golden=str(header.resolve()), golden_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                fased_base=base, fased_region_size=size, fased_slave=slave, registers=records,
                consistent_region_table_copies=len(region),
                scope='allocated region/registry compared; decoded fragments remain external')


def differential(candidate, cone, export_tool, out, materialized=False):
    width = int(re.search(r'output \[(\d+):0\] model_tNasti', cone)[1]) + 1
    defaults = re.findall(r'(?:write|read)MaxReqs <= 32\x27h([0-9a-f]+);', cone)
    assert len(defaults) == 2 and len(set(defaults)) == 1
    initial = int(defaults[0], 16)
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'request-limits.mlir', out/'request-limits.sv'
    run([export_tool] + (['--materialized-bank'] if materialized else []) + [candidate, exported], 'export')
    text = exported.read_text().replace('firrtl.module public @', 'firrtl.module @')
    isolated = modules(text)
    assert set(isolated) == {HELPER, 'GGFASEDRequestLimitsProbe'}
    assert isolated[HELPER] == modules(candidate.read_text())[HELPER], 'export rebuilt production bank'
    expected = {'requestLimits.clock': 'top.clock', 'requestLimits.reset': 'top.reset',
                'top.limits.write': 'requestLimits.limits.write', 'top.limits.read': 'requestLimits.limits.read',
                'top.mcr': 'requestLimits.mcr'}
    assert wiring(text, 'GGFASEDRequestLimitsProbe', aggregate=True) == expected
    # Each mutation has valid FIRRTL types. The exporter verifies the whole IR
    # before rejecting these incorrect production connections.
    production = candidate.read_text(); body = modules(production).get(NEW, '')
    mutations = [
        ('model-reset', body.replace('firrtl.strictconnect %requestLimits_reset, %hostReset',
                                    'firrtl.strictconnect %requestLimits_reset, %sim_fased_model_reset'),
         'request limits must use host clock/reset'),
        ('crossed-maxima', body.replace('firrtl.subfield %requestLimits_limits[write]',
                                       'firrtl.subfield %requestLimits_limits[read]'),
         'wrong admission maximum source'),
        ('missing-mcr', re.sub(r'^      firrtl.connect %fased_request_limits_mcr,.*\n', '', body, flags=re.M),
         'missing production driver')]
    if materialized:
        mutations = []  # Unattached bank: no production admission wiring is claimed.
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
reg mcr_write_0_valid=0,mcr_write_1_valid=0,mcr_read_0_ready=0,mcr_read_1_ready=0;
reg [31:0] mcr_write_0_bits=0,mcr_write_1_bits=0;
reg [3:0] mcr_wstrb=0;
wire [31:0] mcr_read_0_bits,mcr_read_1_bits,sWrite,sRead;
wire mcr_read_0_valid,mcr_read_1_valid,mcr_write_0_ready,mcr_write_1_ready;
wire [LIMIT_HI:0] limits_write,limits_read,sWriteLimit,sReadLimit;
GGFASEDRequestLimitsProbe candidate(.*);
SFCRequestLimits oracle(.clock(clock),.reset(reset),
 .crFile_io_mcr_write_2_valid(mcr_write_0_valid),.crFile_io_mcr_write_3_valid(mcr_write_1_valid),
 .crFile_io_mcr_write_2_bits(mcr_write_0_bits),.crFile_io_mcr_write_3_bits(mcr_write_1_bits),
 .crFile_io_mcr_read_2_bits(sWrite),.crFile_io_mcr_read_3_bits(sRead),
 .model_tNasti_io_mmReg_writeMaxReqs(sWriteLimit),.model_tNasti_io_mmReg_readMaxReqs(sReadLimit));
reg [31:0] writeState=INITIAL,readState=INITIAL;
reg [63:0] rng=64'h207abcd;
reg [15:0] strobes=0;
reg [COVER_HI:0] writeLimits=0,readLimits=0;
reg [3:0] masks=0;
integer cycles=0,writes=0,zeroStrobe=0,resetWrites=0,upperBits=0,stallWrites=0,readBackpressure=0;
task compare;
begin
 if(mcr_read_0_bits!==sWrite || mcr_read_1_bits!==sRead ||
    mcr_read_0_bits!==writeState || mcr_read_1_bits!==readState ||
    limits_write!==sWriteLimit || limits_read!==sReadLimit ||
    limits_write!==writeState[LIMIT_HI:0] || limits_read!==readState[LIMIT_HI:0])
  $fatal(1,"request-limit readback/admission mismatch cycle=%0d",cycles);
 if(!mcr_read_0_valid||!mcr_read_1_valid||!mcr_write_0_ready||!mcr_write_1_ready)
  $fatal(1,"MCRIO.bindReg valid/ready mismatch");
 if(candidate.requestLimits.writeMaxReqs!==oracle.writeMaxReqs ||
    candidate.requestLimits.readMaxReqs!==oracle.readMaxReqs)
  $fatal(1,"request-limit register mismatch cycle=%0d",cycles);
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 strobes[mcr_wstrb]=1;masks[{mcr_write_1_valid,mcr_write_0_valid}]=1;
 writes+=mcr_write_0_valid+mcr_write_1_valid;
 zeroStrobe+=(mcr_write_0_valid||mcr_write_1_valid)&&mcr_wstrb==0;
 resetWrites+=(mcr_write_0_valid||mcr_write_1_valid)&&reset;
 stallWrites+=(mcr_write_0_valid||mcr_write_1_valid)&&!targetFire;
 upperBits+=mcr_write_0_valid&&mcr_write_0_bits>LIMIT_MASK;
 readBackpressure+=!mcr_read_0_ready||!mcr_read_1_ready;
 if(reset)begin writeState=INITIAL;readState=INITIAL;end
 else begin
  if(mcr_write_0_valid)writeState=mcr_write_0_bits;
  if(mcr_write_1_valid)readState=mcr_write_1_bits;
 end
 clock=1;#2;compare();writeLimits[limits_write]=1;readLimits[limits_read]=1;
 clock=0;#2;cycles++;
end endtask
initial begin
 reset=1;step();reset=0;
 for(integer strobe=0;strobe<16;strobe++)for(integer mask=0;mask<4;mask++)begin
  mcr_wstrb=4'(strobe);mcr_write_0_valid=mask[0];mcr_write_1_valid=mask[1];
  mcr_write_0_bits=32'hffffffff;mcr_write_1_bits=32'h80000000;step();
 end
 mcr_write_0_valid=1;mcr_write_1_valid=1;
 for(integer limit=0;limit<LIMIT_COUNT;limit++)begin
  mcr_write_0_bits=32'(limit);mcr_write_1_bits=32'(LIMIT_MASK-limit);step();
 end
 for(integer i=0;i<32768;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  targetFire=rng[0];reset=i%31==0;mcr_write_0_valid=rng[1];mcr_write_1_valid=rng[2];
  mcr_read_0_ready=rng[3];mcr_read_1_ready=rng[4];mcr_wstrb=rng[11:8];
  mcr_write_0_bits=rng[31:0];mcr_write_1_bits=rng[63:32];step();
 end
 if(writes<1000||zeroStrobe<100||resetWrites<100||upperBits<1000||stallWrites<1000||
    readBackpressure<1000||strobes!=16'hffff||masks!=4'hf||writeLimits!='1||readLimits!='1)
  $fatal(1,"missing request-limit coverage");
 $display("PASS cycles=%0d writes=%0d zero_strobe_writes=%0d reset_writes=%0d upper_bits=%0d target_stall_writes=%0d read_backpressure=%0d strobes=%0d masks=%0d write_limits=%0d read_limits=%0d",
 cycles,writes,zeroStrobe,resetWrites,upperBits,stallWrites,readBackpressure,strobes,masks,writeLimits,readLimits);
 $finish;
end endmodule
'''
    for key, value in {'LIMIT_HI': width-1, 'INITIAL': initial, 'COVER_HI': (1 << width)-1,
                       'LIMIT_MASK': (1 << width)-1, 'LIMIT_COUNT': 1 << width}.items():
        tb = tb.replace(key, str(value))
    (out/'check.sv').write_text(tb)
    run(['verilator', '--binary', '--assert', '-j', '4', '--timing', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_bank_cloned_verbatim=True, production_five_wrapper_bindings_executed=not materialized,
                admission_width=width, reset_default=initial, unattached_bank_only=materialized,
                original_sfc_register_updates_readback_and_low_bits_executed=True,
                valid_IR_production_binding_mutations_rejected=len(mutations),
                outputs_and_registers_compared_before_and_after_every_edge=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'golden', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    p.add_argument('--baseline', type=Path)
    p.add_argument('--immutable', type=Path)
    p.add_argument('--materialized-bank', action='store_true')
    a = p.parse_args(); cone = oracle(a.golden)
    if a.immutable:
        assert ' '.join(cone.split()) == ' '.join(oracle(a.immutable).split())
    candidate = a.candidate_dir/('post-fame-fased-request-limits-bank.mlir' if a.materialized_bank
                               else 'post-fame-fased-request-limits.mlir')
    result = {} if a.materialized_bank else composition(a.candidate_dir, a.baseline)
    allocated = None if a.materialized_bank else allocation(candidate, a.golden)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()) if a.immutable else None,
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['FASEDMemoryTimingModel'],
                  allocated_register_ABI=allocated,
                  rtl_differential=differential(candidate, cone, a.export_tool, a.output_dir, a.materialized_bank), limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS SFC/native request-limit MMIO bank' + ('; admission attachment remains unsupported'
          if a.materialized_bank else ', admission binding and boundary composition'))


if __name__ == '__main__': main()
