// See LICENSE for license details.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Only this header sees the macro: standard headers are already included.
// Seed the final uint64_t credit without executing four billion earlier steps.
#define private public
#include "systematic_scheduler.h"
#undef private

namespace {
unsigned checks = 0;

void require(bool condition, const std::string &message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Exercise the real scheduler's stopping condition and 32-bit FPGA credits,
// rather than independently parsing or exposing its private max_cycles field.
void check_horizon(uint64_t horizon, const std::vector<std::string> &args) {
  systematic_scheduler_t scheduler(args);
  uint64_t advanced = 0;
  while (!scheduler.finished_scheduled_tasks()) {
    require(advanced < horizon,
            "scheduler failed to stop at requested horizon");
    const uint64_t expected = std::min(MAX_MIDAS_STEP, horizon - advanced);
    const uint32_t step = scheduler.get_largest_stepsize();
    require(step == expected,
            "scheduler clipped or truncated horizon " +
                std::to_string(horizon));
    advanced += step;
    scheduler.run_scheduled_tasks();
  }
  require(advanced == horizon, "scheduler finished before requested horizon");
}

template <typename Exception>
void check_invalid(const std::string &value) {
  try {
    systematic_scheduler_t scheduler({"+max-cycles=" + value});
  } catch (const Exception &error) {
    require(std::string(error.what()).find("max-cycles") != std::string::npos,
            "invalid value diagnostic must identify max-cycles");
    return;
  }
  throw std::runtime_error("invalid max-cycles value was accepted");
}

void check_tasks() {
  constexpr uint64_t horizon = 100000000000ULL;
  constexpr uint64_t first_task = 4294967296ULL;
  constexpr uint64_t period = 10000000000ULL;
  systematic_scheduler_t scheduler({"+max-cycles=100000000000"});
  uint64_t advanced = 0;
  uint64_t task_calls = 0;
  scheduler.register_task(first_task, [&]() {
    require(advanced == first_task + task_calls * period,
            "scheduled task ran at wrong cycle");
    ++task_calls;
    return period;
  });
  while (!scheduler.finished_scheduled_tasks()) {
    require(advanced < horizon, "tasks prevented scheduler stopping");
    const uint64_t next_task = first_task + task_calls * period;
    const uint64_t expected =
        std::min({MAX_MIDAS_STEP, horizon - advanced, next_task - advanced});
    const uint32_t step = scheduler.get_largest_stepsize();
    require(step == expected,
            "scheduler missed task or final clipping boundary");
    advanced += step;
    scheduler.run_scheduled_tasks();
  }
  require(advanced == horizon, "tasks changed requested horizon");
  require(task_calls == 10, "tasks beyond uint32_t were not all executed");
}

void check_final_uint64_credit() {
  constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
  for (uint64_t horizon : {maximum - 1, maximum}) {
    systematic_scheduler_t scheduler(
        {"+max-cycles=" + std::to_string(horizon)});
    scheduler.current_cycle = maximum - 3;
    require(!scheduler.finished_scheduled_tasks(),
            "final uint64_t credit finished early");
    require(scheduler.get_largest_stepsize() == horizon - (maximum - 3),
            "final uint64_t credit wrapped or missed horizon");
    require(scheduler.finished_scheduled_tasks(),
            "final uint64_t credit failed to stop");
  }
}
} // namespace

int main() {
  try {
    check_horizon(100000000000ULL, {"+max-cycles=100000000000"});
    for (uint64_t horizon : {0ULL,
                             1ULL,
                             2147483647ULL,
                             2147483648ULL,
                             4294967295ULL,
                             4294967296ULL}) {
      check_horizon(horizon, {"+max-cycles=" + std::to_string(horizon)});
    }
    check_horizon(100000000000ULL,
                  {"+max-cycles=1", "+max-cycles=100000000000"});
    check_horizon(42, {"+max-cycles=00042"});
    check_tasks();
    check_final_uint64_credit();

    for (const auto &args : std::vector<std::vector<std::string>>{
             {},
             {"+max-cycles-extra=0"},
             {"+max-cycles=18446744073709551615"}}) {
      systematic_scheduler_t scheduler(args);
      for (unsigned i = 0; i < 3; ++i) {
        require(!scheduler.finished_scheduled_tasks(),
                "default or UINT64_MAX horizon finished prematurely");
        require(scheduler.get_largest_stepsize() == MAX_MIDAS_STEP,
                "default or UINT64_MAX horizon truncated a credit");
      }
    }
    for (const char *value : {"",
                              "-1",
                              "-0",
                              "+1",
                              "0x10",
                              "1e3",
                              "123junk",
                              "123 ",
                              " 123",
                              "\t1",
                              "1\n"}) {
      check_invalid<std::invalid_argument>(value);
    }
    check_invalid<std::invalid_argument>(std::string("1\0junk", 6));
    check_invalid<std::out_of_range>("18446744073709551616");
    check_invalid<std::out_of_range>("999999999999999999999999999999999999");
    std::cout << "PASS systematic_scheduler: " << checks
              << " checks; horizon=100000000000 with 10 scheduled tasks\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL systematic_scheduler: " << error.what() << '\n';
    return 1;
  }
}
