#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded empty CPU write attachment with immutable SFC semantics."""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintControlMasterCompare import ports as rtl_ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

OLD = 'GGHostMemoryPlatformWrapper'
TOP = 'GGPrintCPUStreamWriteWrapper'
HELPER = 'CatalogRocket_GGEmptyCPUStreamWrite'
FIELDS = [('out', 'aw_ready', 1), ('in', 'aw_valid', 1), ('in', 'aw_bits_id', 16),
          ('in', 'aw_bits_size', 3), ('out', 'w_ready', 1), ('in', 'w_valid', 1),
          ('in', 'w_bits_strb', 64), ('out', 'b_valid', 1), ('out', 'b_bits_id', 16),
          ('out', 'b_bits_resp', 2), ('in', 'b_ready', 1)]


def sfc_contract(path):
    s = clean(path.read_text())
    header, body = module(s, 'CPUManagedStreamEngine')
    nets = equations(body)
    assert nets['auto_cpu_managed_axi4_in_b_bits_id'] == 'auto_cpu_managed_axi4_in_aw_bits_id'
    compact = re.sub(r'\s+', '', body)
    assert "~reset&~(~auto_cpu_managed_axi4_in_aw_valid|auto_cpu_managed_axi4_in_aw_bits_size==3'h6)" in compact
    assert "_T_4&~(~auto_cpu_managed_axi4_in_w_valid|auto_cpu_managed_axi4_in_w_bits_strb==64'hffffffffffffffff)" in compact
    assert nets['_T_4'] == '~reset' and 'always@(posedgeclock)' in compact
    buffer = equations(module(s, 'AXI4Buffer_2')[1])
    assert buffer['nodeOut_aw_deq_q_io_deq_ready'] == "1'h0"
    assert 'io_deq_ready' not in rtl_ports(module(s, 'Queue_52')[0])
    ack = equations(module(s, 'Queue_53')[1])
    assert ack['ram_resp_MPORT_en'] == "1'h0" and ack['ram_resp_MPORT_data'] == "2'h0"
    return dict(module='CPUManagedStreamEngine', live_AW_ID_while_B_invalid=True,
                reset_gated_AW_size=6, reset_gated_W_strb='0xffffffffffffffff',
                AW_dequeue_ready=0, W_dequeue_ready_port_eliminated=True,
                B_enqueue_valid=0, B_response_code=0,
                boundary='engine facing; AXI queues remain separate porting steps')


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(s):
        entries = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', s, re.M | re.S))
        result = {x[1]: x[0] for x in entries}
        assert len(entries) == len(result)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 288 and len(new) == 289 and set(new) - set(old) == {TOP}
    for name, body in old.items():
        assert new[name] == body, 'changed prior module ' + name
    copied = native_ports(old[OLD])
    assert len(copied) == 109
    appended = [(d, 'cpu_stream_' + n, f'!firrtl.uint<{w}>') for d, n, w in FIELDS]
    assert native_ports(new[TOP]) == copied + appended
    expected = {('sim.' + n if d == 'in' else 'top.' + n):
                ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected.update({'cpuStreamWrite.clock': 'top.hostClock', 'cpuStreamWrite.reset': 'top.hostReset'})
    for d, n, _ in FIELDS:
        a, v = 'top.cpu_stream_' + n, 'cpuStreamWrite.' + n
        expected[v if d == 'in' else a] = a if d == 'in' else v
    assert wiring(new[TOP]) == expected and len(expected) == 122
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('cpuStreamWrite', HELPER)]
    native = native_module(baseline, 'GGEmptyCPUStreamWrite')
    assert new[HELPER].replace('@' + HELPER + '(', '@GGEmptyCPUStreamWrite(') == native
    assert wiring(new[HELPER]) == {'top.aw_ready': '0', 'top.w_ready': '0', 'top.b_valid': '0',
                                  'top.b_bits_id': 'top.aw_bits_id', 'top.b_bits_resp': '0'}
    assert len([n for n in old if n.startswith('GGPrintBridge')]) == 21
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.EmptyCPUStreamWriteTargets", nested = \{targets = \[([^]]+)\]\}\}', line)
    assert probe
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{TOP}>' + n for n in (
        'hostClock', 'hostReset', 'mem_0.aw.bits.id', 'cpu_stream_ar_bits_addr', 'cpu_stream_r_bits_data')]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied:
        original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=288,
                public_ports=120, copied_ports=109, added_write_ports=11,
                wrapper_connections=122, retained_Print_modules=21,
                native_empty_writer_identity=True, recursive_copied_target_transfer=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'):
        p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    baseline = a.baseline.read_text()
    report = dict(golden=str(a.sfc.resolve()), SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, baseline) for x, y in zip(a.before, a.after)],
                  limitation='Engine-facing empty CPU write; AW/W/AR/R/B buffers and production Print integration remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC empty CPU write defaults, live AW ID and reset-gated assertions; 288 bodies, 120 ports, 122 routes and recursive targets per order')


if __name__ == '__main__':
    main()
