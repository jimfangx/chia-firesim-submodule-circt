# Indexed NIC control and driver bindings

`mapNICBridgeControl` preserves `nicBridge_ctrl` for the existing one-widget
ABI. Multiple decoded banks receive distinct `nicBridge_N_ctrl` AXI ports and
MCRFile state. Every bank/control identity is preflighted before wrapping.
Global allocation derives the actual NIC count and retains NIC/TracerV producer
constructor order; NIC sink counts remain independent. Write/read bindings and
header constructors retain each widget's actual MMIO region and both stream
indices. Unsupported loopback constructors still reject explicitly.

Supporting checks:

* `goldengate-nic-multi-mmio` checks indexed local control wiring and atomic
  rejection of missing, duplicated and colliding identities.
* `goldengate-control-address-decode` checks all six interleavings of two NIC
  and two TracerV widgets against their real register/count contracts.
* `goldengate-control-widget-writes` traces both NIC controls through the live
  wrapper hierarchy to distinct adapters and checks late failures are atomic.
* `goldengate-nic-header` checks both constructors, nontrivial widget indices
  and independent source/sink assignments, plus malformed late bindings.
* `goldengate-cpu-stream-port` composes the actual native read/write engines
  for two normal NICs, then for two NICs sharing outgoing transport with
  TracerV and Print. The full platform derives outgoing and incoming address
  windows independently (18/18 bits for NIC-only and 19/18 for the mixed case),
  retains every engine body and forwards all 512 payload bits. Corrupt stream
  identities, count/index/base/width metadata and incomplete directional
  allocations reject atomically. The prior singleton-sink and fixed TracerV
  window restrictions no longer block these normal production compositions.
* `NICMultiControlCompare.py --compose-tool <goldengate-nic-multi-mmio-test>
  --golden <immutable-SFC-NIC-RTL> --output-dir <new-directory>` lowers the
  actual two-widget wrapper. Each AXI path is compared to the unchanged archived
  SFC MCRFile/register cone while its peer holds outstanding B/R responses.
  Directed and randomized checks cover ordering, IDs, holes, strobes, reset,
  backpressure and configuration. Valid transaction outputs and initialized
  configuration are checked; uninitialized capture data before valid is outside
  this supporting simulation's scope.

The archived primitive oracle is historical U250 RTL. These tests supply no F2
feature receipt, separate LEC/SEC proof or Linux packet evidence. A complete
multi-widget platform composition, normal NIC traffic and both actual loopback
APIs still need enabled representative verification through CHIA.
