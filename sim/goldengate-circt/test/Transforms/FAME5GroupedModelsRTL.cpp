// See LICENSE for license details.
// Identical public-interface scoreboard for real SFC and native grouped models.
#include "VTop.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <iostream>
#ifndef THREADS
#define THREADS 4
#endif
#define PORTS(field) &top.field##0, &top.field##1
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv); VTop top;
#if THREADS == 2
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix
#elif THREADS == 4
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix, &top.in2_##suffix, &top.in3_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix, &top.out2_##suffix, &top.out3_##suffix
#else
#error only two/four-thread RTL supporting cases
#endif
  std::array<CData*, THREADS> valid{INPUTS(valid)}, ready{INPUTS(ready)}, clear{INPUTS(bits_clear)}, add{INPUTS(bits_add)}, enable{INPUTS(bits_enable)};
  std::array<CData*, THREADS> outReady{OUTPUTS(ready)}, outValid{OUTPUTS(valid)}, sum{OUTPUTS(bits_sum)}, echo{OUTPUTS(bits_echo)}, flag{OUTPUTS(bits_flag)};
  std::array<unsigned, THREADS> expected{};
  auto edge = [&] { top.hostClock = 0; top.eval(); top.hostClock = 1; top.eval(); top.hostClock = 0; top.eval(); };
  auto resetAll = [&] {
    top.hostReset = 1;
    for (unsigned i = 0; i < THREADS; ++i) { *valid[i] = *clear[i] = *add[i] = *enable[i] = *outReady[i] = 0; }
    // SFC's model-local selector is unreset. Its initial phase is zero under
    // Verilator; hold reset for a whole rotation, starting at phase zero, to
    // reset every bank and align it with the resettable top selector.
    for (unsigned i = 0; i < THREADS; ++i) edge();
    expected.fill(0); top.hostReset = 0;
  };
  top.hostClock = 0; resetAll();
  uint64_t checksum = 0; unsigned observations = 0;
  for (unsigned cycle = 0; cycle < 8192; ++cycle) {
    if (cycle == 2048 || cycle == 4096) resetAll();
    unsigned slot = cycle % THREADS;
    unsigned selected = slot == 0 ? 1 : slot == 1 ? 0 : 5-slot;
    for (unsigned i = 0; i < THREADS; ++i) {
      *valid[i] = (cycle * 3 + i * 7) % 11 != 0;
      *outReady[i] = (cycle / (i + 1) + i) % 7 != 0;
      *enable[i] = (cycle + i * 5) % 5 != 0;
      *clear[i] = (cycle * 7 + i * 31) % 173 == 0;
      *add[i] = (cycle * 29 + i * 41 + cycle / 7) & 255;
    }
    top.eval();
    for (unsigned i = 0; i < THREADS; ++i) {
      bool active = i == selected;
      if (*ready[i] != (active && *outReady[selected]) ||
          *outValid[i] != (active && *valid[selected]) ||
          *sum[i] != expected[selected] || *echo[i] != *add[selected] || *flag[i] != *enable[selected]) {
        std::cerr << "grouped state/channel mismatch cycle " << cycle << " slot " << slot << " port " << i << '\n'; return 1;
      }
      checksum = checksum * 131 + *ready[i]; checksum = checksum * 131 + *outValid[i];
      checksum = checksum * 131 + *sum[i]; checksum = checksum * 131 + *echo[i]; checksum = checksum * 131 + *flag[i];
      ++observations;
    }
    if (*clear[selected]) expected[selected] = 0;
    else if (*valid[selected] && *enable[selected] && *outReady[selected]) expected[selected] = (expected[selected] + *add[selected]) & 255;
    edge();
  }
  top.final();
  std::cout << "GROUPED " << THREADS << ' ' << observations << ' ' << checksum << '\n';
  return 0;
}
