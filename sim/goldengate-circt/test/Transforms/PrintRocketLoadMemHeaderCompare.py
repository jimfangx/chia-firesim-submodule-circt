#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare direct-engine LoadMem driver collateral with native/SFC boundaries."""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, module
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring, engine_nets, sfc_contract

candidate, header_path, native_ir, native_header, golden, report_path = map(Path, sys.argv[1:])
text, baseline = candidate.read_text(), native_ir.read_text()
header = header_path.read_text()
engine = native_module(text, 'GGPrintLoadMemEngine')
assert wiring(engine) == engine_nets(), 'direct engine wiring differs from Scala MMIO/AXI contract'
helpers = ['GGLoadMemWriter', 'GGLoadMemWriteDataFIFO', 'GGLoadMemReadRequests', 'GGLoadMemReadDataFIFO']
for helper in helpers:
    imported = native_module(text, 'CatalogRocket_' + helper).replace('@CatalogRocket_', '@')
    assert imported == native_module(baseline, helper), 'live imported helper changed: ' + helper
assert native_module(text, 'GGLoadMemWriteMMIOBank') == native_module(baseline, 'GGLoadMemWriteMMIOBank')
construct = r'(#ifdef GET_CORE_CONSTRUCTOR\nregistry.add_widget\(new loadmem_t\(.*?#endif // GET_CORE_CONSTRUCTOR)'
body = re.findall(construct, header, re.S)
assert len(body) == 1 and body == re.findall(construct, native_header.read_text(), re.S)
fields = ['W_ADDRESS_H', 'W_ADDRESS_L', 'W_LENGTH', 'ZERO_OUT_DRAM', 'W_DATA',
          'ZERO_FINISHED', 'R_ADDRESS_H', 'R_ADDRESS_L', 'R_DATA']
addresses = re.findall(r'\.(\w+) = (\d+),', body[0])
assert addresses == [(name, str(384 + 4*i)) for i, name in enumerate(fields)]
assert 'conf_target.mem,\n  2U' in body[0]
for i, name in enumerate(fields):
    assert f'offsetof(LOADMEMWIDGET_struct, {name}) == {i} * sizeof(uint64_t)' in header
regions = re.search(r'firrtl.module @GGControlAddressDecode\(.*?goldengate.controlRegions = \[(.*?)\]', text, re.S)[1]
assert 'name = "LoadMemWidget_0", size = 64 : i64, slave = 4 : i32, start = 384 : i64' in regions
reference = clean(golden.read_text())
router = module(reference, 'NastiRouter')[1]
for channel in ['aw', 'ar']:
    assert f"25'h180 <= io_master_{channel}_bits_addr & io_master_{channel}_bits_addr < 25'h1c0" in router
assert '#include "print-bridge-decoders.h"' not in header and '#pragma once' not in header
assert 'GOLDENGATE_PRINT_BRIDGE_DECODERS' in header
interface = (header_path.parent / 'FireSim-generated.const.vh').read_text()
assert interface == (native_header.parent / 'FireSim-generated.const.vh').read_text()
bus_widths = {}
for prefix in ['CTRL', 'CPU_MANAGED_AXI4', 'MEM']:
    values = [int(re.search(r'`define ' + prefix + '_' + kind + r'_BITS (\d+)', interface)[1])
              for kind in ['ID', 'ADDR', 'DATA']]
    assert 'AXI4Config{' + ', '.join(map(str, values)) + '}' in header
    bus_widths[prefix] = dict(zip(['id', 'address', 'data'], values))
assert all(header.count('new '+cls+'(') == 1 for cls in ['master_t','clockmodule_t','reset_pulse_t',
    'loadmem_t','peek_poke_t','uart_t','tsibridge_t','blockdev_t','tracerv_t','CPUManagedStreamWidget','FASEDMemoryTimingModel'])
assert all(header.count('registry.add_widget(goldengate_print::make_print_bridge_'+str(i)+'(') == 1 for i in range(2))
all_regions = {name: (int(start), int(size)) for name, size, slave, start in re.findall(
    r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', regions)}
structs = {'SIMULATIONMASTER': 'SimulationMaster_0', 'CLOCKBRIDGEMODULE': 'ClockBridgeModule_0',
    'RESETPULSEBRIDGEMODULE': 'ResetPulseBridgeModule_0', 'LOADMEMWIDGET': 'LoadMemWidget_0',
    'PEEKPOKEBRIDGEMODULE': 'PeekPokeBridgeModule_0', 'UARTBRIDGEMODULE': 'UARTBridgeModule_0',
    'TSIBRIDGEMODULE': 'TSIBridgeModule_0', 'BLOCKDEVBRIDGEMODULE': 'BlockDevBridgeModule_0',
    'TRACERVBRIDGEMODULE': 'TracerVBridgeModule_0'}
registers = {}
for struct, bank in structs.items():
    bodies = re.findall(struct + r'_struct\{(.*?)\}', header, re.S)
    assert len(bodies) == 1
    registers[bank] = {name: int(value) for name, value in re.findall(r'\.(\w+) = (\d+),', bodies[0])}
    start, size = all_regions[bank]
    assert registers[bank] and len(set(registers[bank].values())) == len(registers[bank])
    assert all(start <= address < start+size and address % 4 == 0 for address in registers[bank].values())
sfc = sfc_contract(golden)
report = dict(golden=str(golden.resolve()), candidate=str(header_path.resolve()),
              sfc_loadmem=sfc, mmio_addresses=dict(addresses), memory_data_chunks=2,
              engine_connections=len(engine_nets()), unchanged_live_helpers=helpers + ['GGLoadMemWriteMMIOBank'],
              native_loadmem_constructor_identical=True, complete_expanded_driver=True,
              native_struct_registers=registers, native_interface_bus_widths=bus_widths)
report_path.write_text(json.dumps(report, indent=2) + '\n')
print('PASS LoadMem direct-engine wiring, five retained helper bodies, nine SFC MMIO words and 64/32-bit chunk ABI')
