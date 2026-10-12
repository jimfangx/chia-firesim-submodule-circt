#!/usr/bin/env python3
# See LICENSE for license details.
"""Check linked native banks and the immutable SFC Master/LoadMem boundary."""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import sfc_contract, MEMORY
from PrintRocketControlMasterCompare import native_ports
from PrintControlMasterCompare import ports as rtl_ports

candidate, native, golden, report = map(Path, sys.argv[1:])
text, source = candidate.read_text(), native.read_text()
roots = ['GGSimulationMasterBank', 'GGPeekPokeMCRFile', 'GGPeekPokeMMIOBank',
         'GGResetPulseBridgeMCRFile', 'GGResetPulseBridge', 'GGBlockDevMMIOBank',
         'GGUARTMCRFile', 'GGUARTMMIOBank', 'GGFASEDLatencyRegisters', 'GGFASEDRequestLimits',
         'GGFASEDHistograms', 'GGFASEDStatistics', 'GGFASEDFunctionalModelRegister',
         'GGFASEDResponseErrors', 'GGTracerVMCRFile', 'GGTracerVTriggerConfig', 'GGTSIMMIOBank',
         'GGClockBridgeMCRFile', 'GGSingleClockBridge', 'GGLoadMemMCRFile',
         'GGLoadMemWriteMMIOBank']
linked = roots + re.findall(r'^    firrtl.module @(CatalogRocket_\w+)\(', text, re.M)
registries = dict(zip(['GGLoadMemWriteDataFIFO', 'GGLoadMemReadRequests', 'GGLoadMemReadDataFIFO'],
                     ['GGLoadMemWriteDataWrapper', 'GGLoadMemReadRequestWrapper', 'GGLoadMemReadDataWrapper']))
register_pattern = r'goldengate.mmioRegisters = \[.*?\](?:, )?'
for helper in ['GGLoadMemWriter', 'GGLoadMemRequestQueue2', 'GGLoadMemRequestArbiter',
               'GGLoadMemWriteDataFIFO', 'GGLoadMemReadRequests', 'GGLoadMemReadDataFIFO']:
    assert 'CatalogRocket_' + helper in linked, helper
for name in linked:
    original = name.removeprefix('CatalogRocket_')
    actual = native_module(text, name).replace('@CatalogRocket_', '@')
    if original in registries:
        rows = re.search(register_pattern, actual)[0].removesuffix(', ')
        expected_rows = re.search(register_pattern, native_module(source, registries[original]))[0].removesuffix(', ')
        assert rows == expected_rows, original
        actual = re.sub(register_pattern, '', actual)
    assert actual == native_module(source, original), name
assert 'firrtl.module @F1Shim(' not in text, 'catalog captured platform top'
assert 'firrtl.module @GGCPUStreamRead(' not in text, 'catalog captured allocated CPU transport'
assert not re.search(r'firrtl.module @CatalogRocket_(?:Rocket|FAME.*Wrapper|GGLoadMem.*Wrapper)\(', text), 'catalog captured target/transport wrapper'

reference = clean(golden.read_text())
bank = native_module(text, 'GGSimulationMasterBank')
oracle = module(reference, 'SimulationMaster')[1]
widths = dict((n, int(w)+1) for w, n in re.findall(r'\breg\s+\[(\d+):0\]\s+(\w+)', oracle)
              if not n.startswith('_RAND_'))
actual = {}
for line in bank.splitlines():
    match = re.search(r'%(\w+) = firrtl.reg(?:reset)? ', line)
    if match:
        actual[match[1]] = int(re.findall(r'!firrtl.uint<(\d+)>', line)[-1])
assert actual == widths
constants = dict((v, (int(w), int(n))) for v, n, w in re.findall(
    r'(%\w+) = firrtl.constant (\d+) : !firrtl.uint<(\d+)>', bank))
resets = dict((n, constants[v]) for n, v in re.findall(
    r'%(\w+) = firrtl.regreset %clock, %reset, (%\w+)', bank))
expected = dict((n, (int(w), int(v, 16))) for n, w, v in re.findall(
    r'if \(reset\) begin\s*(\w+) <= (\d+)\x27h([0-9a-f]+);', oracle))
assert resets == expected
slots = ['INIT_DONE', 'PRESENCE_READ', 'PRESENCE_WRITE']
nets = equations(oracle)
assert all(nets[f'crFile_io_mcr_read_{i}_bits'] == name for i, name in enumerate(slots))
assert re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = true, writeable = true\}', bank) == list(zip(slots, ['0', '4', '8']))
oracle_ports = rtl_ports(module(reference, 'LoadMemWidget')[0])
memory_widths = {}
for direction, pin, width in MEMORY:
    assert oracle_ports['auto_to_host_memory_out_' + pin][1] == width
    if pin.startswith(('aw_', 'w_', 'b_', 'ar_')):
        helper = 'GGLoadMemReadRequests' if pin.startswith('ar_') else 'GGLoadMemWriter'
        assert (direction, 'mem_' + pin, f'!firrtl.uint<{width}>') in native_ports(
            native_module(text, 'CatalogRocket_' + helper))
    elif pin == 'r_bits_data':
        assert ('in', 'in_bits', f'!firrtl.uint<{width}>') in native_ports(
            native_module(text, 'CatalogRocket_GGLoadMemReadDataFIFO'))
    memory_widths[pin] = width
result = dict(candidate=str(candidate.resolve()), golden=str(golden.resolve()),
              imported_roots=len(roots), unchanged_hardware_definitions=len(linked), transferred_loadmem_registries=len(registries),
              master_register_widths=actual, master_resets=resets, master_slots=slots,
              loadmem_memory_widths=memory_widths, loadmem_sfc_contract=sfc_contract(golden),
              limitation='Definition ingestion; remaining Rocket token/stream and full-platform binding are separate boundaries.')
report.write_text(json.dumps(result, indent=2) + '\n')
print(f'PASS control catalog: {len(roots)} roots, {len(linked)} unchanged hardware definitions, three retained LoadMem registries; SFC Master and LoadMem contracts')
