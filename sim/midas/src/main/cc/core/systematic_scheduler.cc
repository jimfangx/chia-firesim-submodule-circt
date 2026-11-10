// See LICENSE for license details

#include "systematic_scheduler.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <limits>
#include <stdexcept>

systematic_scheduler_t::systematic_scheduler_t(
    const std::vector<std::string> &args) {
  for (const auto &arg : args) {
    if (arg.find("+max-cycles=") == 0) {
      uint64_t parsed_cycles = 0;
      const char *end = arg.data() + arg.size();
      const auto result =
          std::from_chars(arg.data() + 12, end, parsed_cycles, 10);
      if (result.ec == std::errc::result_out_of_range) {
        throw std::out_of_range("max-cycles exceeds uint64_t range: " + arg);
      }
      if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument(
            "Expected +max-cycles=<unsigned 64-bit decimal integer>: " + arg);
      }
      max_cycles = parsed_cycles;
    }
  }
}

void systematic_scheduler_t::register_task(uint64_t first_cycle,
                                           task_t &&task) {
  tasks.push_back(task_tuple_t{std::move(task), first_cycle});
}

uint32_t systematic_scheduler_t::get_largest_stepsize() {
  // Saturate before adding so a valid 64-bit horizon near UINT64_MAX does
  // not wrap the final credit back to the beginning of simulation.
  uint64_t next_cycle =
      current_cycle +
      std::min(default_step_size,
               std::numeric_limits<uint64_t>::max() - current_cycle);
  if (max_cycles && *max_cycles < next_cycle) {
    next_cycle = *max_cycles;
  }

  for (auto &t : tasks) {
    if (t.next_cycle < next_cycle) {
      next_cycle = t.next_cycle;
    }
  }

  assert(next_cycle - current_cycle <= MAX_MIDAS_STEP);
  uint32_t step = next_cycle - current_cycle;
  assert(step != 0); // Check for forward progress.
  current_cycle = next_cycle;
  return step;
}

void systematic_scheduler_t::run_scheduled_tasks() {
  for (auto &t : tasks) {
    if (t.next_cycle == current_cycle) {
      t.next_cycle += t.task();
    }
  }
}
