// See LICENSE for license details.
// One public-interface scoreboard for complete unchanged SFC and native grouping.
#include "VTop.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#ifndef THREADS
#define THREADS 4
#endif
#if THREADS == 2
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix
#elif THREADS == 4
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix, &top.in2_##suffix, &top.in3_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix, &top.out2_##suffix, &top.out3_##suffix
#else
#error only two/four-thread RTL supporting cases
#endif
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv); VTop top;
  std::string control = argc > 1 ? argv[1] : "";
  bool wrongMask = control == "--wrong-mask-model";
  bool differentUndefined = control == "--different-undefined-payload";
  bool corruptWritten = control == "--corrupt-written-payload";
  bool corruptReady = control == "--corrupt-ready";
  std::array<CData*, THREADS> valid{INPUTS(valid)}, ready{INPUTS(ready)}, address{INPUTS(bits_addr)}, data{INPUTS(bits_data)}, write{INPUTS(bits_write)}, mask{INPUTS(bits_mask)}, read{INPUTS(bits_read)};
  std::array<CData*, THREADS> outReady{OUTPUTS(ready)}, outValid{OUTPUTS(valid)}, response{OUTPUTS(bits_data)}, count{OUTPUTS(bits_count)};
  std::array<std::array<unsigned, 3>, THREADS> memory{};
  std::array<std::array<bool, 3>, THREADS> written{};
  std::array<unsigned, THREADS> lastRead{}, expectedCount{};
  std::array<bool, THREADS> definedRead{};
  auto edge = [&] { top.hostClock = 0; top.eval(); top.hostClock = 1; top.eval(); top.hostClock = 0; top.eval(); };
  auto reset = [&] {
    // No target RAM initialization or clearing. Only reset-defined register
    // state is compared after one whole, phase-aligned round-robin rotation.
    top.hostReset = 1;
    for (unsigned i = 0; i < THREADS; ++i) {
      *valid[i] = *outReady[i] = 1;
      *read[i] = *write[i] = *mask[i] = *address[i] = *data[i] = 0;
    }
    for (unsigned i = 0; i < THREADS; ++i) edge();
    top.hostReset = 0; expectedCount.fill(0); definedRead.fill(false);
  };
  top.hostClock = 0; reset();
  uint64_t checksum = 0;
  unsigned observations = 0, undefinedReads = 0, stalls = 0, writes = 0, masked = 0, collisions = 0;
  for (unsigned cycle = 0; cycle < 16384; ++cycle) {
    if (cycle == 4096 || cycle == 8192) reset();
    unsigned slot = cycle % THREADS;
    unsigned selected = slot == 0 ? 1 : slot == 1 ? 0 : 5 - slot;
    for (unsigned i = 0; i < THREADS; ++i) {
      *valid[i] = (cycle * 3 + i * 7) % 11 != 0;
      *outReady[i] = (cycle / (i + 1) + i) % 7 != 0;
      *address[i] = (cycle / THREADS + i) % 3;
      *data[i] = (cycle * 29 + i * 41 + cycle / 7) & 255;
      *write[i] = (cycle + i * 5) % 3 != 0;
      *mask[i] = (cycle / THREADS + i) % 4 != 0;
      *read[i] = (cycle * 7 + i) % 5 != 0;
    }
    top.eval();
    for (unsigned i = 0; i < THREADS; ++i) {
      bool active = i == selected;
      unsigned observedReady = *ready[i] ^ (corruptReady && cycle == 0 && i == 0);
      unsigned observedResponse = *response[i];
      if (differentUndefined && !definedRead[selected]) observedResponse ^= 0xa5;
      if (corruptWritten && definedRead[selected]) observedResponse ^= 1;
      if (observedReady != (active && *outReady[selected]) || *outValid[i] != (active && *valid[selected])) {
        std::cerr << "grouped memory channel leakage cycle " << cycle << " port " << i << '\n'; return 1;
      }
      checksum = checksum * 131 + *ready[i]; checksum = checksum * 131 + *outValid[i];
      if (*outValid[i]) {
        if (*count[i] != expectedCount[selected]) {
          std::cerr << "grouped gated register mismatch cycle " << cycle << '\n'; return 1;
        }
        checksum = checksum * 131 + *count[i];
        if (definedRead[selected]) {
          if (observedResponse != lastRead[selected]) {
            std::cerr << "grouped written response mismatch cycle " << cycle << " thread " << selected
                      << " got " << observedResponse << " expected " << lastRead[selected] << '\n'; return 1;
          }
          checksum = checksum * 131 + observedResponse; ++observations;
        } else ++undefinedReads;
      }
    }
    bool fired = *valid[selected] && *outReady[selected];
    if (!fired) definedRead[selected] = false;
    if (fired) {
      unsigned a = *address[selected];
      // The read samples pre-edge old data. Only previously written words
      // establish a defined payload; unwritten/disabled reads stay unspecified.
      definedRead[selected] = *read[selected] && written[selected][a];
      if (definedRead[selected]) lastRead[selected] = memory[selected][a];
      if (*write[selected]) {
        if ((*mask[selected] != 0) != wrongMask) {
          memory[selected][a] = *data[selected]; written[selected][a] = true; ++writes;
        } else ++masked;
      }
      if (*read[selected] && *write[selected] && *mask[selected]) ++collisions;
      expectedCount[selected] = (expectedCount[selected] + 1) & 255;
    } else ++stalls;
    edge();
  }
  if (observations < 1000 || undefinedReads == 0 || writes == 0 || masked == 0 || collisions == 0 || stalls == 0) return 3;
  top.final();
  std::cout << "GROUPED_SYNC " << THREADS << ' ' << observations << ' ' << undefinedReads << ' '
            << stalls << ' ' << writes << ' ' << masked << ' ' << collisions << ' ' << checksum << '\n';
}
