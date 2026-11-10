// See LICENSE for license details.
// Public-interface scoreboard shared by the unchanged SFC and native RTL.
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
  std::vector<std::array<Word, 3>> memory(threads);
  std::vector<Word> lastRead(threads), lastRWRead(threads);
  std::vector<bool> readValid(threads), rwValid(threads);
  auto edge = [&](bool targetEnable) {
    top.hostClock = top.clock = 0; top.eval();
    top.hostClock = 1; top.clock = targetEnable; top.eval();
    top.hostClock = top.clock = 0; top.eval();
  };
  top.hostClock = top.clock = 0;
  top.ren = top.rwen = 1; top.wmode = 0;
  top.raddr = top.rwaddr = 0;
  top.wen = top.wmask_lo = top.wmask_hi = 1;
  top.rwmask_lo = top.rwmask_hi = top.rwdata_lo = top.rwdata_hi = 0;
  // Neither the target RAM nor SFC's edge/tag registers have a reset. Populate
  // RAM by ordinary writes while keeping the required round-robin schedule.
  for (unsigned cycle = 0; cycle < 3 * threads; ++cycle) {
    unsigned thread = cycle % threads, address = cycle / threads;
    top.threadIdx = thread; top.waddr = address;
    top.wdata_lo = 11 + thread * 19 + address;
    top.wdata_hi = 23 + thread * 13 + address;
    memory[thread][address] = {top.wdata_lo, top.wdata_hi};
    edge(true);
  }
  top.wen = 0;
  for (unsigned cycle = 0; cycle < 8 * threads; ++cycle) {
    unsigned thread = cycle % threads;
    top.threadIdx = thread;
    lastRead[thread] = lastRWRead[thread] = memory[thread][0];
    readValid[thread] = rwValid[thread] = true;
    edge(true);
  }
  uint64_t checksum = 0;
  unsigned observations = 0, stalled = 0, masked = 0;
  for (unsigned cycle = 0; cycle < 16384; ++cycle) {
    unsigned thread = cycle % threads;
    unsigned ra = (cycle / threads) % 3;
    // Periodically collide the reader with the write port to check RUW=old;
    // the two writer ports always address distinct words.
    unsigned wa = cycle % 5 == 0 ? ra : (ra + 1) % 3, rwa = (ra + 2) % 3;
    top.threadIdx = thread;
    top.raddr = ra; top.waddr = wa; top.rwaddr = rwa;
    top.ren = cycle % 7 != 0; top.wen = cycle % 4 != 0;
    top.rwen = cycle % 11 != 0; top.wmode = cycle % 3 != 0;
    top.wmask_lo = cycle & 1; top.wmask_hi = (cycle >> 1) & 1;
    top.rwmask_lo = (cycle >> 2) & 1; top.rwmask_hi = (cycle >> 3) & 1;
    top.wdata_lo = (cycle * 17 + thread) & 255;
    top.wdata_hi = (cycle * 29 + thread) & 255;
    top.rwdata_lo = (cycle * 31 + thread) & 255;
    top.rwdata_hi = (cycle * 43 + thread) & 255;
    top.eval();
    // In the selected thread's next slot, a synchronous memory exposes the
    // response from its preceding target edge, even as other threads access
    // unrelated addresses. Disabled reads/write-mode RW responses are undefined
    // in FIRRTL and are deliberately excluded from this behavioral assertion.
    if (readValid[thread]) {
      if (top.result_lo != lastRead[thread][0] || top.result_hi != lastRead[thread][1]) {
        std::cerr << "reader response isolation mismatch cycle " << cycle
                  << " thread " << thread << " got " << unsigned(top.result_lo)
                  << ',' << unsigned(top.result_hi) << " expected " << lastRead[thread][0]
                  << ',' << lastRead[thread][1] << '\n'; return 1;
      }
      checksum = checksum * 131 + top.result_lo;
      checksum = checksum * 131 + top.result_hi; ++observations;
    }
    if (rwValid[thread]) {
      if (top.rwresult_lo != lastRWRead[thread][0] || top.rwresult_hi != lastRWRead[thread][1]) {
        std::cerr << "readwriter response isolation mismatch cycle " << cycle
                  << " thread " << thread << '\n'; return 1;
      }
      checksum = checksum * 131 + top.rwresult_lo;
      checksum = checksum * 131 + top.rwresult_hi; ++observations;
    }
    bool targetEnable = cycle % 13 != 0 && (cycle / threads) % 17 != 0;
    if (targetEnable) {
      readValid[thread] = top.ren;
      rwValid[thread] = top.rwen && !top.wmode;
      if (top.ren) lastRead[thread] = memory[thread][ra];
      if (rwValid[thread]) lastRWRead[thread] = memory[thread][rwa];
      if (top.wen) {
        if (top.wmask_lo) memory[thread][wa][0] = top.wdata_lo;
        if (top.wmask_hi) memory[thread][wa][1] = top.wdata_hi;
      }
      if (top.rwen && top.wmode) {
        if (top.rwmask_lo) memory[thread][rwa][0] = top.rwdata_lo;
        if (top.rwmask_hi) memory[thread][rwa][1] = top.rwdata_hi;
      }
      if (top.wmask_lo != top.wmask_hi || top.rwmask_lo != top.rwmask_hi) ++masked;
    } else ++stalled;
    edge(targetEnable);
  }
  top.final();
  std::cout << "SYNCHRONOUS_MEMORY " << threads << ' ' << observations << ' '
            << stalled << ' ' << masked << ' ' << checksum << '\n';
  return 0;
}
