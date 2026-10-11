#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print LoadMem engine assembly with immutable SFC RTL."""
import argparse
import json
import re
from pathlib import Path
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module

OLD, TOP, ENGINE = 'GGFASEDAddressTranslationWrapper', 'GGPrintLoadMemWrapper', 'GGPrintLoadMemEngine'
HELPERS = ['GGLoadMemWriter', 'GGLoadMemRequestQueue2', 'GGLoadMemRequestArbiter',
           'GGLoadMemWriteMMIOBank', 'GGLoadMemWriteDataFIFO', 'GGLoadMemReadRequests', 'GGLoadMemReadDataFIFO']
INSTANCES = ['writer', 'write_requests', 'request_arbiter', 'registers', 'write_data', 'read_requests', 'read_data']
MEMORY = [('in', 'aw_ready', 1), ('out', 'aw_valid', 1), ('out', 'aw_bits_addr', 34),
          ('out', 'aw_bits_len', 8), ('in', 'w_ready', 1), ('out', 'w_valid', 1),
          ('out', 'w_bits_data', 64), ('out', 'w_bits_last', 1), ('out', 'b_ready', 1),
          ('in', 'b_valid', 1), ('in', 'ar_ready', 1), ('out', 'ar_valid', 1),
          ('out', 'ar_bits_addr', 34), ('out', 'r_ready', 1), ('in', 'r_valid', 1), ('in', 'r_bits_data', 64)]


def wiring(body):
    lines = body.splitlines()
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', lines[0])}
    nets = {}
    for line in lines[1:]:
        if ' = firrtl.instance ' in line:
            left, right = line.split(' = firrtl.instance ', 1)
            inst = right.split()[0]
            values.update(zip(re.findall(r'%\w+', left),
                              (inst + '.' + p for p in re.findall(r'\b(?:in|out) (\w+):', right))))
        if m := re.search(r'(%\w+) = firrtl.constant (\d+) :', line):
            values[m[1]] = m[2]
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            values[m[1]] = values[m[2]] + '.' + m[3]
        if m := re.search(r'(%\w+) = firrtl.subindex (%\w+)\[(\d+)\]', line):
            values[m[1]] = values[m[2]] + '[' + m[3] + ']'
        if m := re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)', line):
            assert values[m[1]] not in nets, 'duplicate driver'
            nets[values[m[1]]] = values[m[2]]
    return nets


def permission_checks(body):
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', body.splitlines()[0])}
    checks = []
    for line in body.splitlines()[1:]:
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            if m[2] in values: values[m[1]] = values[m[2]] + '.' + m[3]
        if m := re.search(r'(%\w+) = firrtl.subindex (%\w+)\[(\d+)\]', line):
            if m[2] in values: values[m[1]] = values[m[2]] + '[' + m[3] + ']'
        if m := re.search(r'(%\w+) = firrtl.not (%\w+)', line):
            values[m[1]] = 'not(' + values[m[2]] + ')'
        if m := re.search(r'firrtl.assert (%\w+), (%\w+), (%\w+), "([^"]+)"', line):
            checks.append(tuple(values[x] for x in m.groups()[:3]) + (m[4],))
    return checks


def engine_nets():
    n = {}
    for inst in [x for x in INSTANCES if x != 'request_arbiter']:
        for p in ('clock', 'reset'): n[f'{inst}.{p}'] = f'top.{p}'
    n.update({'registers.write_ready': 'write_requests.enq.ready',
              'write_requests.enq.valid': 'registers.write_valid', 'write_requests.enq.bits.zero': '0',
              'write_requests.enq.bits.addr': 'registers.write_bits_addr',
              'write_requests.enq.bits.len': 'registers.write_bits_len',
              'write_requests.deq.ready': 'request_arbiter.in_0_ready',
              'request_arbiter.in_0_valid': 'write_requests.deq.valid',
              'registers.zero_ready': 'request_arbiter.in_1_ready',
              'request_arbiter.in_1_valid': 'registers.zero_valid',
              'request_arbiter.out_ready': 'writer.req_ready', 'writer.req_valid': 'request_arbiter.out_valid',
              'registers.zero_finished': 'writer.req_ready', 'write_data.out_ready': 'writer.data_ready',
              'writer.data_valid': 'write_data.out_valid', 'writer.data_bits': 'write_data.out_bits'})
    for f in ('zero', 'addr', 'len'):
        n['request_arbiter.in_0_bits_' + f] = 'write_requests.deq.bits.' + f
        n['writer.req_bits_' + f] = 'request_arbiter.out_bits_' + f
    for d, p, _ in MEMORY:
        peer = ('writer.mem_' + p if p.startswith(('aw_', 'w_', 'b_')) else
                'read_requests.mem_' + p if p.startswith('ar_') else
                {'r_ready': 'read_data.in_ready', 'r_valid': 'read_data.in_valid', 'r_bits_data': 'read_data.in_bits'}[p])
        n[peer if d == 'in' else 'top.loadmem_mem_' + p] = 'top.loadmem_mem_' + p if d == 'in' else peer
    for w in (0, 1, 2, 3, 5):
        n[f'top.mcr.read[{w}]'] = f'registers.mcr.read_{w}'
        n[f'registers.mcr.write_{w}'] = f'top.mcr.write[{w}]'
    n['registers.mcr.wstrb'] = 'top.mcr.wstrb'
    for w in (6, 7):
        n[f'top.mcr.write[{w}].ready'] = f'read_requests.write_{w}_ready'
        n[f'read_requests.write_{w}_valid'] = f'top.mcr.write[{w}].valid'
        n[f'read_requests.write_{w}_bits'] = f'top.mcr.write[{w}].bits'
        n[f'top.mcr.read[{w}].valid'] = f'read_requests.read_{w}_valid'
        n[f'top.mcr.read[{w}].bits'] = f'read_requests.read_{w}_bits'
    n.update({'read_requests.read_7_ready': 'top.mcr.read[7].ready',
              'top.mcr.write[4].ready': 'write_data.in_ready', 'write_data.in_valid': 'top.mcr.write[4].valid',
              'write_data.in_bits': 'top.mcr.write[4].bits', 'top.mcr.read[4].valid': '0', 'top.mcr.read[4].bits': '0',
              'read_data.out_ready': 'top.mcr.read[8].ready', 'top.mcr.read[8].valid': 'read_data.out_valid',
              'top.mcr.read[8].bits': 'read_data.out_bits', 'top.mcr.write[8].ready': '0'})
    return n


def sfc_contract(path):
    text = clean(path.read_text()); header, body = module(text, 'LoadMemWidget')
    ports = rtl_ports(header)
    assert {k: v for k, v in ports.items() if k.startswith('auto_to_host_memory_out_')} == {
        'auto_to_host_memory_out_' + p: ['input' if d == 'in' else 'output', w] for d, p, w in MEMORY}
    e = {k: ' '.join(v.split()) for k, v in equations(body).items()}
    expected = {}
    for inst in ('wAddrQ', 'wDataQ', 'writer', 'rAddrQ_q', 'rDataQ'):
        for p in ('clock', 'reset'): expected[inst + '_' + p] = p
    expected.update({'wAddrQ_io_enq_valid': 'crFile_io_mcr_write_2_valid',
        'wAddrQ_io_enq_bits_addr': '{W_ADDRESS_H,W_ADDRESS_L}',
        'wAddrQ_io_enq_bits_len': "{{2'd0}, crFile_io_mcr_write_2_bits}",
        'wAddrQ_io_deq_ready': 'reqArb_io_in_0_ready', 'reqArb_io_in_0_valid': 'wAddrQ_io_deq_valid',
        'reqArb_io_in_1_valid': 'crFile_io_mcr_write_3_valid', 'reqArb_io_out_ready': 'writer_io_req_ready',
        'writer_io_req_valid': 'reqArb_io_out_valid', 'wDataQ_io_out_ready': 'writer_io_data_ready',
        'writer_io_data_valid': 'wDataQ_io_out_valid', 'writer_io_data_bits': 'wDataQ_io_out_bits',
        'wDataQ_io_in_valid': 'crFile_io_mcr_write_4_valid', 'wDataQ_io_in_bits': 'crFile_io_mcr_write_4_bits',
        'rAddrQ_q_io_enq_valid': 'crFile_io_mcr_write_7_valid',
        'rAddrQ_q_io_enq_bits': "{{2'd0}, crFile_io_mcr_write_7_bits}",
        'rAddrQ_q_io_deq_ready': 'memNasti_conv_io_nasti_ar_ready',
        'rDataQ_io_in_valid': 'memNasti_conv_io_nasti_r_valid', 'rDataQ_io_in_bits': 'memNasti_conv_io_nasti_r_bits_data',
        'rDataQ_io_out_ready': 'crFile_io_mcr_read_8_ready',
        'crFile_io_mcr_read_5_bits': "{{31'd0}, writer_io_req_ready}",
        'crFile_io_mcr_read_8_valid': 'rDataQ_io_out_valid', 'crFile_io_mcr_read_8_bits': 'rDataQ_io_out_bits',
        'crFile_io_mcr_write_2_ready': 'wAddrQ_io_enq_ready', 'crFile_io_mcr_write_3_ready': 'reqArb_io_in_1_ready',
        'crFile_io_mcr_write_4_ready': 'wDataQ_io_in_ready', 'crFile_io_mcr_write_7_ready': 'rAddrQ_q_io_enq_ready',
        'crFile_io_mcr_write_8_ready': "1'h0", 'crFile_io_mcr_read_4_valid': "1'h0", 'crFile_io_mcr_read_4_bits': "32'h0"})
    for f in ('zero', 'addr', 'len'):
        expected['reqArb_io_in_0_bits_' + f] = 'wAddrQ_io_deq_bits_' + f
        expected['writer_io_req_bits_' + f] = 'reqArb_io_out_bits_' + f
    for d, p, _ in MEMORY:
        outer, axi, nasti = 'auto_to_host_memory_out_' + p, 'memNasti_conv_io_axi4_' + p, 'memNasti_conv_io_nasti_' + p
        expected[outer if d == 'out' else axi] = axi if d == 'out' else outer
        if p.startswith(('aw_', 'w_', 'b_')):
            writer = 'writer_io_mem_' + p
            expected[nasti if d == 'out' else writer] = writer if d == 'out' else nasti
    expected['memNasti_conv_io_nasti_ar_valid'] = 'rAddrQ_q_io_deq_valid'
    expected['memNasti_conv_io_nasti_ar_bits_addr'] = '_conv_io_nasti_ar_bits_T[33:0]'
    expected['_conv_io_nasti_ar_bits_T'] = '{R_ADDRESS_H,rAddrQ_bits}'
    expected['memNasti_conv_io_nasti_r_ready'] = 'rDataQ_io_in_ready'
    assert all(e.get(k) == v for k, v in expected.items()), [(k, e.get(k), v) for k, v in expected.items() if e.get(k) != v]
    # All AXI pins traverse the lossless Nasti/AXI identity conversion.
    _, identity = module(text, 'Nasti2AXI4IdentityModule'); ie = equations(identity)
    for d, p, _ in MEMORY:
        assert ie['io_' + ('axi4_' if d == 'out' else 'nasti_') + p] == 'io_' + ('nasti_' if d == 'out' else 'axi4_') + p
    _, arbiter = module(text, 'Arbiter'); ae = equations(arbiter)
    arbiter_want = {'io_out_bits_zero': "io_in_0_valid ? io_in_0_bits_zero : 1'h1",
        'io_out_bits_addr': "io_in_0_valid ? io_in_0_bits_addr : 34'h0",
        'io_out_bits_len': "io_in_0_valid ? io_in_0_bits_len : 34'h80000000",
        'io_in_0_ready': 'io_out_ready', 'grant_1': '~io_in_0_valid', 'io_in_1_ready': 'grant_1 & io_out_ready'}
    assert all(ae.get(k) == v for k, v in arbiter_want.items())
    _, queue = module(text, 'Queue_27'); qe = equations(queue)
    assert 'ram_zero [0:1];' in queue and 'ram_addr [0:1];' in queue and 'ram_len [0:1];' in queue
    assert qe['ram_zero_MPORT_data'] == "1'h0" and qe['ram_zero_MPORT_en'] == 'io_enq_ready & io_enq_valid'
    assert qe['io_enq_ready'] == '~full' and qe['io_deq_valid'] == '~empty'
    return dict(memory_ports=16, address_bits=34, data_bits=64, widget_equations=len(expected),
                identity_conversion_routes=16, write_request_depth=2, zero_fill_priority='queued writes first')


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, after))
    assert len(prior) == 270 and set(result) - set(prior) == {TOP, ENGINE}
    assert all(result[n] == body for n, body in prior.items()), 'prior module body changed'
    old_ports = native_ports(native_module(before, OLD)); copied = [p for p in old_ports if p[1] != 'loadmem_mcr']
    assert len(copied) == 109
    wrapper = native_module(after, TOP)
    assert native_ports(wrapper) == copied + [(d, 'loadmem_mem_' + p, f'!firrtl.uint<{w}>') for d, p, w in MEMORY]
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'loadmem.clock': 'top.hostClock', 'loadmem.reset': 'top.hostReset', 'sim.loadmem_mcr': 'loadmem.mcr'})
    for d, p, _ in MEMORY:
        name = 'loadmem_mem_' + p
        nets[('loadmem.' if d == 'in' else 'top.') + name] = ('top.' if d == 'in' else 'loadmem.') + name
    assert wiring(wrapper) == nets
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', wrapper) == [('sim', OLD), ('loadmem', ENGINE)]
    engine = native_module(after, ENGINE); engine_ports = native_ports(engine)
    assert engine_ports[:2] == [('in', 'clock', '!firrtl.clock'), ('in', 'reset', '!firrtl.uint<1>')]
    assert engine_ports[2] == ('out', 'mcr', next(t for _, n, t in old_ports if n == 'loadmem_mcr'))
    assert engine_ports[3:] == native_ports(wrapper)[109:]
    want = engine_nets(); assert wiring(engine) == want, (wiring(engine).keys() - want.keys(), want.keys() - wiring(engine).keys())
    instances = re.findall(r' = firrtl.instance (\w+) @(\w+)\b', engine)
    assert [x for x, _ in instances] == INSTANCES
    for (_, name), helper in zip(instances, HELPERS):
        assert name == helper or name.endswith('_' + helper)
        actual = native_module(after, name).replace('@CatalogRocket_', '@')
        assert actual == native_module(baseline, helper), 'shared helper differs: ' + helper
    assert permission_checks(engine) == [
        ('top.clock', 'not(top.mcr.read[4].ready)', 'not(top.reset)', 'Can only write to this decoupled sink'),
        ('top.clock', 'not(top.mcr.write[8].valid)', 'not(top.reset)', 'Can only read from this decoupled source')]
    assert engine.count('firrtl.assert ') == 2 and not re.search(r'firrtl.(?:reg|regreset|mem)\b', engine)
    # Retarget nested archive identities by copied root; retain consumed MCR on inner module.
    archive = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"').replace('~' + OLD, '~' + TOP)
    for _, p, _ in copied:
        archive = re.sub(r'\|' + OLD + r'>' + re.escape(p) + r'(?=[.\["\\])', '|' + TOP + '>' + p, archive)
    assert archive == after.splitlines()[1], 'annotation archive target rewrite differs'
    assert len([n for n in prior if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=270,
                copied_ports=109, memory_ports=16, top_ports=125, wrapper_connections=len(nets),
                engine_connections=len(want), reused_helpers=7, preserved_Print_modules=21,
                nested_archive_targets_preserved=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['LoadMemWidget', 'Nasti2AXI4IdentityModule', 'Arbiter', 'Queue_27'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Print-enabled production integration and runtime parity remain pending; this attaches the native engine, before the shared AXI write arbiter.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC LoadMemWidget ports/handshakes and identity routes; 270 prior bodies, 109 copied ports, seven native helpers and nested targets preserved per order')


if __name__ == '__main__': main()
