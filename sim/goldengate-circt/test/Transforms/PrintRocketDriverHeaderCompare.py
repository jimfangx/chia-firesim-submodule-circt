#!/usr/bin/env python3
# See LICENSE for license details.
"""Check composed native Rocket headers against allocated IR and immutable SFC RTL.

The oracle disables Print. This complete native replay has the same widget
catalog; expanded TracerV/Print fragments are compared separately.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, module

ir_path, directory, golden, report_path = map(Path, sys.argv[1:])
header_path = directory / 'FireSim-generated.const.h'
report = {'golden': str(golden.resolve()), 'candidate': str(header_path.resolve())}
header = header_path.read_text()
verilog = (directory / 'FireSim-generated.const.vh').read_text()
reference = clean(golden.read_text())
interface = ports(module(reference, 'F1Shim')[0])
macros = {name: int(value) for name, value in re.findall(r'`define (\w+) (\d+)', verilog)}
configs = {'CTRL': 'io_master', 'CPU_MANAGED_AXI4': 'io_pcis', 'MEM': 'io_slave_0'}
widths = {}
for prefix, bus in configs.items():
    widths[prefix] = {}
    for kind, field in [('ID', 'id'), ('ADDR', 'addr'), ('DATA', 'data')]:
        channel = 'w' if kind == 'DATA' else 'ar'
        width = interface[f'{bus}_{channel}_bits_{field}'][1]
        assert macros[f'{prefix}_{kind}_BITS'] == width
        widths[prefix][kind] = width
    values = [widths[prefix][kind] for kind in ['ID', 'ADDR', 'DATA']]
    assert f'AXI4Config{{{values[0]}, {values[1]}, {values[2]}}}' in header
assert macros['CPU_MANAGED_AXI4_PRESENT'] == 1 and macros['MEM_HAS_CHANNEL0'] == 1
assert '.mem_num_channels = 1' in header
constructors = ['master_t', 'clockmodule_t', 'reset_pulse_t', 'loadmem_t', 'peek_poke_t',
                'uart_t', 'tsibridge_t', 'blockdev_t', 'tracerv_t', 'CPUManagedStreamWidget',
                'FASEDMemoryTimingModel']
assert all(header.count('new ' + cls + '(') == 1 for cls in constructors)
ir = ir_path.read_text()
active = re.search(r'firrtl.module @GGControlAddressDecode\(.*?attributes \{goldengate.controlRegions = \[(.*?)\]\}', ir, re.S).group(1)
regions = {name: (int(start), int(size)) for name, size, slave, start in
           re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', active)}
router = module(reference, 'NastiRouter')[1]
matching_regions = []
for bank, (start, size) in regions.items():
    low = r"25'h" + format(start, 'x') + r" <= io_master_ar_bits_addr & " if start else ''
    matched = bool(re.search(low + r"io_master_ar_bits_addr < 25'h" + format(start + size, 'x'), router))
    if matched: matching_regions.append(bank)
assert len(matching_regions) == len(regions), (regions, matching_regions)
report.update(sfc_matching_regions=matching_regions,
              sfc_region_differences={n: regions[n] for n in regions if n not in matching_regions})
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
    assert registers[bank]
    start, size = regions[bank]
    assert all(start <= addr < start + size and addr % 4 == 0 for addr in registers[bank].values())
    assert len(set(registers[bank].values())) == len(registers[bank])
fased = re.search(r'new FASEDMemoryTimingModel\((.*?)\n\)\);', header, re.S).group(1)
fased_registers = [(name, int(value)) for name, value in re.findall(r'\{ std::string\("([^"]+)"\), (\d+)U \}', fased)]
start, size = regions['FASEDMemoryTimingModel_0']
registry = re.search(r'firrtl.module @GGFASEDMMIOWrapper\(.*?goldengate.mmioRegisters = \[(.*?)\]', ir, re.S).group(1)
words = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = (true|false), writeable = (true|false)\}', registry)
assert words
expected = [(name, start + int(offset)) for permission in [2, 3] for row in words
            for name, offset in [row[:2]] if row[permission] == 'true']
assert fased_registers == expected
assert all(start <= addr < start + size and addr % 4 == 0 for _, addr in fased_registers)
report.update(interface_widths_match_sfc=widths, native_constructors=constructors,
              native_struct_registers=registers, fased_read_write_entries=len(fased_registers),
              complete_native_driver_header=True)
report_path.write_text(json.dumps(report, indent=2) + '\n')
print('PASS complete native Rocket constructors, allocated registers and SFC AXI widths')
