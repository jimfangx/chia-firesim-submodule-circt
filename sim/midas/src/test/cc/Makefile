# See LICENSE for license details.
# Standalone driver regression; no target elaboration, FPGA SDK or simulator.

test_dir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
core_dir := $(abspath $(test_dir)/../../main/cc/core)
OUT_DIR ?= $(test_dir)/build
CXXFLAGS ?= -O2 -g
CXX_STANDARD ?= c++20

.PHONY: test
test: $(OUT_DIR)/systematic_scheduler_test
	$<

$(OUT_DIR)/systematic_scheduler_test: $(test_dir)/systematic_scheduler_test.cc $(core_dir)/systematic_scheduler.cc $(core_dir)/systematic_scheduler.h
	mkdir -p $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=$(CXX_STANDARD) -Wall -Wextra -Werror -I$(core_dir) $(test_dir)/systematic_scheduler_test.cc $(core_dir)/systematic_scheduler.cc $(LDFLAGS) -o $@
