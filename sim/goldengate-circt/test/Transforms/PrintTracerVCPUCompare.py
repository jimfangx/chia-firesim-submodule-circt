#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare live TracerV/Print CPU allocation with immutable SFC transport.

SFC has Print disabled. Its stream-0 handshake/data/count semantics serve as
an oracle for both identical-width windows; this is not Print runtime parity.
"""
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, instance, module, without_targets
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintControlMasterCompare import ports
from PrintTracerVTriggerCompare import RTL, parsed_expression


class ParsedNets(dict):
    def __getitem__(self, key):
        value = super().__getitem__(key)
        if isinstance(value, str):
            value = parsed_expression(value)
            self[key] = value
        return value


class CombinationalRTL(RTL):
    def __init__(self, header, body):
        body = re.sub(r"\d+'h[0-9a-fA-F]+", lambda m: m[0].lower(), body)
        self.widths = {n: p[1] for n, p in ports(header).items()}
        for _, hi, lo, name in re.findall(r'\b(reg|wire|logic)\s*(?:\[(\d+):(\d+)\])?\s+(\w+)', body):
            self.widths[name] = int(hi) - int(lo) + 1 if hi else 1
        self.nets = ParsedNets(equations(body))

    def signal(self, name):
        return self.eval(('leaf', name))[1]


def main():
    candidate, golden, report = map(Path, sys.argv[1:])
    before = (candidate / 'post-print-host-binding.mlir').read_text()
    after = (candidate / 'post-print-cpu-streams.mlir').read_text()
    names = re.findall(r'^    firrtl.module @(\w+)\(', before, re.M)
    for name in names:
        assert native_module(before, name) == native_module(after, name), name
    added = set(re.findall(r'^    firrtl.module @(\w+)\(', after, re.M)) - set(names)
    assert added == {'GGCPUStreamRead', 'GGCPUStreamReadWrapper', 'GGCPUStreamCountBank', 'GGCPUStreamCountWrapper'}
    read_ir = native_module(after, 'GGCPUStreamRead')
    rows = re.findall(r'\{bufferBaseAddress = (\d+) : i64, depth = (\d+) : i64, index = (\d+) : i64, name = "([^"]+)", port = "([^"]+)", widthBytes = (\d+) : i64\}', read_ir)
    expected = [('0', '6144', '0', 'TRACERVBRIDGEMODULE_0_to_cpu_stream', 'tracerv_stream', '64'),
                ('524288', '6144', '1', 'PRINTBRIDGEMODULE_0_to_cpu_stream', 'print_0_stream', '64')]
    assert rows == expected and 'goldengate.streamAddressSpaceBits = 19 : i64' in read_ir
    count_ir = native_module(after, 'GGCPUStreamCountBank')
    for i, row in enumerate(expected):
        assert f'name = "{row[3]}_count", offset = {4*i} : i32, readable = true, writeable = false' in count_ir
    nets = wiring(native_module(after, 'GGCPUStreamReadWrapper'), 'GGCPUStreamReadWrapper', aggregate=True)
    for i, row in enumerate(expected):
        assert nets[f'cpuStreamRead.stream_{i}'] == f'sim.{row[4]}'
    nets = wiring(native_module(after, 'GGCPUStreamCountWrapper'), 'GGCPUStreamCountWrapper', aggregate=True)
    for i, name in enumerate(('tracerv_stream_count', 'print_0_stream_count')):
        assert nets[f'streamCount.count_{i}'] == f'sim.{name}'
    annos = json.loads((candidate / 'post-print-cpu-streams-all.json').read_text())
    prior = json.loads((candidate / 'post-print-host-binding-all.json').read_text())
    assert without_targets(annos) == without_targets(prior)
    def transfer(value, old, new, copied):
        if isinstance(value, str):
            if value == '~'+old: return '~'+new
            if not value.startswith('~'+old+'|'): return value
            suffix = value[len(old)+1:]
            prefix = '|'+old+'>'
            if suffix.startswith(prefix):
                root = re.split(r'[.\[]', suffix[len(prefix):])[0]
                if root in copied: suffix = '|'+new+'>'+suffix[len(prefix):]
            return '~'+new+suffix
        if isinstance(value, list): return [transfer(v, old, new, copied) for v in value]
        if isinstance(value, dict): return {k: transfer(v, old, new, copied) for k, v in value.items()}
        return value
    stages = [('GGPrintTracerVStreamQueueWrapper', 'GGCPUStreamReadWrapper', {'tracerv_stream','print_0_stream'}),
              ('GGCPUStreamReadWrapper', 'GGCPUStreamCountWrapper', {'tracerv_stream_count','print_0_stream_count'})]
    transferred = prior
    for old, new, consumed in stages:
        copied = set(re.findall(r'\b(?:in|out) %(\w+):', native_module(after, old).splitlines()[0])) - consumed
        transferred = transfer(transferred, old, new, copied)
    assert annos == transferred, 'copied/consumed target identities differ'
    clocks = lambda a: [r for r in a if r['class'] == 'midas.passes.ChannelClockInfoAnnotation']
    assert clocks(annos) == clocks(prior)
    rtl = clean((candidate / 'post-print-cpu-streams.sv').read_text())
    reference = clean(golden.read_text())
    actual = CombinationalRTL(*module(rtl, 'GGCPUStreamRead'))
    oracle = CombinationalRTL(*module(reference, 'CPUManagedStreamEngine'))
    prefix = 'TRACERVBRIDGEMODULE_0_to_cpu_stream_'
    # SFC moves the constant response into the downstream buffer's RAM writer.
    # Recover only that proven producer constant; do not assume a missing port.
    instance(module(reference, 'FPGATop')[1], 'AXI4Buffer_2', 'axi4buf_2')
    instance(module(reference, 'AXI4Buffer_2')[1], 'Queue_55', 'nodeIn_r_deq_q')
    response = equations(module(reference, 'Queue_55')[1])['ram_resp_MPORT_data'].strip()
    assert response == "2'h0"
    oracle.nets['auto_cpu_managed_axi4_in_r_bits_resp'] = response
    # Check the source/count pair at the emitted instance boundary, including
    # the flipped queue-ready direction, independently of MLIR SSA checks.
    wrapper = module(rtl, 'GGCPUStreamReadWrapper')[1]
    sim = instance(wrapper, 'GGPrintTracerVStreamQueueWrapper', 'sim')
    transport = instance(wrapper, 'GGCPUStreamRead', 'cpuStreamRead')
    for i, row in enumerate(expected):
        for leaf in ('ready', 'valid', 'bits'):
            assert sim[f'{row[4]}_{leaf}'] == transport[f'stream_{i}_{leaf}']
    wrapper = module(rtl, 'GGCPUStreamCountWrapper')[1]
    sim = instance(wrapper, 'GGCPUStreamReadWrapper', 'sim')
    bank = instance(wrapper, 'GGCPUStreamCountBank', 'streamCount')
    for i, name in enumerate(('tracerv_stream_count', 'print_0_stream_count')):
        assert sim[name] == bank[f'count_{i}']
    counts = CombinationalRTL(*module(rtl, 'GGCPUStreamCountBank'))
    assert equations(module(reference, 'CPUManagedStreamEngine')[1])['crFile_io_mcr_read_0_bits'].strip() == "{{19'd0}, " + prefix + 'outgoingQueueIO_q_io_count}'
    rng = random.Random(130); comparisons = 0
    for sample in range(4096):
        index = sample % 2
        ar, ready, v0, v1 = [(sample >> b) & 1 for b in range(1, 5)]
        length = rng.getrandbits(8)
        beats = [length if sample & (32 << i) else rng.getrandbits(9) for i in range(2)]
        bits = [rng.getrandbits(512) for _ in range(2)]
        ident, offset = rng.getrandbits(16), rng.randrange(524288)
        actual.inputs = dict(ar_valid=ar, r_ready=ready, ar_bits_addr=index*524288+offset,
            ar_bits_len=length, ar_bits_id=ident, stream_0_valid=v0, stream_1_valid=v1,
            stream_0_bits=bits[0], stream_1_bits=bits[1], readBeatCounter_0=beats[0], readBeatCounter_1=beats[1])
        oracle.inputs = dict(auto_cpu_managed_axi4_in_ar_valid=ar, auto_cpu_managed_axi4_in_r_ready=ready,
            auto_cpu_managed_axi4_in_ar_bits_addr=offset, auto_cpu_managed_axi4_in_ar_bits_len=length,
            auto_cpu_managed_axi4_in_ar_bits_id=ident,
            **{prefix+'ser_des_io_narrow_out_valid': (v0, v1)[index],
               prefix+'ser_des_io_narrow_out_bits': bits[index], prefix+'readBeatCounter': beats[index]})
        for out in ('ar_ready', 'r_valid', 'r_bits_id', 'r_bits_data', 'r_bits_last', 'r_bits_resp'):
            assert actual.signal(out) == oracle.signal('auto_cpu_managed_axi4_in_'+out), (sample, out)
            comparisons += 1
        assert actual.signal(f'stream_{index}_ready') == oracle.signal(prefix+'ser_des_io_narrow_out_ready')
        assert actual.signal(f'stream_{1-index}_ready') == 0
        comparisons += 2
        occupancy = [rng.randrange(6145) for _ in range(2)]
        counts.inputs = {f'count_{i}': occupancy[i] for i in range(2)}
        for i in range(2):
            assert counts.signal(f'mcr_read_{i}_bits') == occupancy[i]
            assert counts.signal(f'mcr_read_{i}_valid') == 1
            comparisons += 2
    report.write_text(json.dumps(dict(golden=str(golden.resolve()), golden_module='CPUManagedStreamEngine',
        matching_signal_samples=comparisons, unchanged_modules=len(names), annotation_records=len(annos),
        ordered_streams=[r[3] for r in expected], stream_base_addresses=[0,524288], count_offsets=[0,4],
        clock_map_preserved=True, live_queue_count_connections=True, recursive_target_transfer=True,
        limitation='SFC has Print disabled; full joint control/platform assembly and enabled-Print runtime remain pending'), indent=2)+'\n')
    print(f'PASS {comparisons} SFC CPU handshake/data/count samples; {len(names)} bodies and {len(annos)} annotations preserved')


if __name__ == '__main__': main()
