// See LICENSE for license details.
// Run with Verilator --cc --exe against independently emitted SFC/CIRCT RTL.
#include "VModel.h"
#include "verilated.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  unsigned count = argc == 2 ? std::atoi(argv[1]) : 4;
  if (count < 2 || count > 4)
    return 2;
  VModel top;
  auto edge = [&]() {
    top.clock = 0;
    top.eval();
    top.clock = 1;
    top.eval();
    top.clock = 0;
    top.eval();
  };
  // Storage starts unspecified in both compilers. Reset every selected thread
  // through the public interface before observing initialized values.
  top.reset = 1;
  top.enable = 0;
  top.data = 0;
  for (unsigned slot = 0; slot < count; ++slot) {
    top.thread = slot;
    edge();
  }
  std::vector<unsigned> state(count, 7);
  uint64_t checksum = 0;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    unsigned thread = (cycle * 7 + cycle / 13) % count;
    top.thread = thread;
    top.reset = cycle % 37 == 0;
    top.enable = (cycle + thread) % 5 != 0;
    top.data = (cycle * 17 + thread * 29) & 255;
    top.eval();
    if (top.result != state[thread]) {
      std::cerr << "read mismatch before edge " << cycle << '\n';
      return 1;
    }
    state[thread] = top.reset ? 7 : top.enable ? top.data : state[thread];
    edge();
    for (unsigned slot = 0; slot < count; ++slot) {
      top.thread = slot;
      top.eval();
      if (top.result != state[slot]) {
        std::cerr << "state isolation mismatch after edge " << cycle
                  << " slot " << slot << '\n';
        return 1;
      }
      checksum = checksum * 131 + top.result + slot;
    }
  }
  top.final();
  std::cout << "RTL " << count << " " << checksum << '\n';
  return 0;
}
