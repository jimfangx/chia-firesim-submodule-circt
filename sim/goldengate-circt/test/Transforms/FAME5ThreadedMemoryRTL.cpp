// See LICENSE for license details.
// Identical public-interface scoreboard for unchanged SFC and native CIRCT RTL.
#include "VModel.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  unsigned threads = argc == 2 ? std::atoi(argv[1]) : 4;
  if (threads != 2 && threads != 4) return 2;
  VModel top;
  using Word = std::array<unsigned, 2>;
  std::vector<std::array<Word, 3>> expected(threads);
  auto edge = [&](bool targetEnable) {
    top.clock = top.hostClock = 0; top.eval();
    top.hostClock = 1; top.clock = targetEnable; top.eval();
    top.clock = top.hostClock = 0; top.eval();
  };
  top.clock = top.hostClock = 0; top.ren = 0; top.rwen = 0;
  top.raddr = top.rwaddr = top.waddr = top.wmode = 0;
  top.rwmask_lo = top.rwmask_hi = top.rwdata_lo = top.rwdata_hi = 0;
  // Raw target memories have unspecified initial contents. Initialize every
  // legal word through its selected thread's ordinary masked write port.
  top.wen = top.wmask_lo = top.wmask_hi = 1;
  for (unsigned thread = 0; thread < threads; ++thread)
    for (unsigned addr = 0; addr < 3; ++addr) {
      top.threadIdx = thread; top.waddr = addr;
      top.wdata_lo = 11 + thread * 19 + addr;
      top.wdata_hi = 23 + thread * 13 + addr;
      expected[thread][addr] = {top.wdata_lo, top.wdata_hi}; edge(true);
    }
  uint64_t checksum = 0;
  unsigned observations = 0;
  for (unsigned cycle = 0; cycle < 8192; ++cycle) {
    unsigned thread = (cycle * 7 + cycle / 11) % threads;
    unsigned wa = cycle % 3, rwa = (cycle + 1) % 3, ra = (cycle / 3) % 3;
    top.threadIdx = thread; top.raddr = ra; top.waddr = wa; top.rwaddr = rwa;
    top.ren = cycle % 5 != 0; top.wen = cycle % 4 != 0;
    top.rwen = cycle % 7 != 0; top.wmode = cycle % 3 != 0;
    top.wmask_lo = cycle & 1; top.wmask_hi = (cycle >> 1) & 1;
    top.rwmask_lo = (cycle >> 2) & 1; top.rwmask_hi = (cycle >> 3) & 1;
    top.wdata_lo = (cycle * 17 + thread) & 255;
    top.wdata_hi = (cycle * 29 + thread) & 255;
    top.rwdata_lo = (cycle * 31 + thread) & 255;
    top.rwdata_hi = (cycle * 43 + thread) & 255;
    top.eval();
    if ((top.ren && (top.result_lo != expected[thread][ra][0] || top.result_hi != expected[thread][ra][1])) ||
        (top.rwen && !top.wmode && (top.rwresult_lo != expected[thread][rwa][0] || top.rwresult_hi != expected[thread][rwa][1]))) {
      std::cerr << "pre-edge read mismatch cycle " << cycle << '\n'; return 1;
    }
    bool enabledClock = cycle % 11 != 0;
    if (enabledClock) {
      if (top.wen) {
        if (top.wmask_lo) expected[thread][wa][0] = top.wdata_lo;
        if (top.wmask_hi) expected[thread][wa][1] = top.wdata_hi;
      }
      if (top.rwen && top.wmode) {
        if (top.rwmask_lo) expected[thread][rwa][0] = top.rwdata_lo;
        if (top.rwmask_hi) expected[thread][rwa][1] = top.rwdata_hi;
      }
    }
    edge(enabledClock);
    // Observe all threads after every edge, including masked/disabled writes
    // and stalled target clocks. Host clock continues to advance independently.
    top.wen = top.rwen = 0; top.ren = 1;
    for (unsigned slot = 0; slot < threads; ++slot)
      for (unsigned addr = 0; addr < 3; ++addr) {
        top.threadIdx = slot; top.raddr = addr; top.eval();
        if (top.result_lo != expected[slot][addr][0] || top.result_hi != expected[slot][addr][1]) {
          std::cerr << "thread/mask isolation mismatch cycle " << cycle << " slot " << slot << " addr " << addr << '\n'; return 1;
        }
        checksum = checksum * 131 + top.result_lo;
        checksum = checksum * 131 + top.result_hi;
        ++observations;
      }
  }
  top.final();
  std::cout << "MEMORY " << threads << " " << observations << " " << checksum << '\n';
  return 0;
}
