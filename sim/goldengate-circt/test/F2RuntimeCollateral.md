# F2 runtime collateral

The exact four F2 SFC archives emit two SimulationMapping outputs that the
native compiler previously omitted: `.partition.const.h` and
`.peer2peer.const.yaml`. All four selected recipes are unpartitioned, so the
partition header contains the two standard comment lines and no constants.
Their stream maps contain one TracerV source at base zero, capacity 6144 tokens,
and width 64 bytes. Missing collateral is a compiler output mismatch; no runtime
failure is inferred from that absence for these no-NIC recipes.

The oracle is `SimulationMapping.generateHeaderAnnos`,
`PlatformShim.genPartitioningHeader/genPeerToPeerAddrMap`, and
`CPUManagedStreamEngine.genPeerToPeerAddrMap`. The manager reads the YAML in
`firesim_topology_with_passes.py:get_bridge_offset`, and partition switch setup
uses the partition header. Unsupported partition configurations still reject.

`prepareMetasimInterfaceHeader` now emits the empty partition header only after
validating the supported physical boundary. `prepareCPUManagedStreamHeader`
emits YAML from the same ordered source-then-sink allocations that it validates
against live queues, DMA selects, count MMIO and the driver constructor. JSON
quoted YAML keys preserve unusual names; names and geometry are never inferred
from the target configuration. Existing sidecar suffixes reject before mutation.
The complete driver composer publishes all files transactionally. The Print
composer forwards the validated map and AutoILA retains it across the temporary
pre-lowering driver-header capture. Make copies and grouped outputs, and CHIA's
candidate required-output inventory, require both files so stale SFC outputs
cannot satisfy a native compilation.

Focused checks use the existing CPU stream and metasim header tests. CPU coverage
includes quoted/backslash/newline/tab names, independent outgoing DMA bases,
shared queue definitions, invalid hardware/allocation rejection, and an existing
map conflict. Print binding tests cover optional map return, ordered allocations,
caller-string preservation on failure and complete driver publication. The small
CPU fixture can emit the exact selected stream identity through the real output
emitter without compiling a selected target:

```
goldengate-cpu-managed-stream-header-test --peer-map OUTPUT_DIRECTORY
goldengate-metasim-interface-header-test --f2-collateral OUTPUT_DIRECTORY
```

Compare the parsed emitted map and partition header with each immutable golden,
retaining ordered allocation evidence in the C++ test. Metasim tests also assert
the exact partition header body. Full selected-target regeneration, AutoILA path validation and the
complete paired semantic node remain CHIA-owned. These component checks do not
establish full behavioral parity, Jasper proof, F2 runtime success or speed.
