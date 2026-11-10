# See LICENSE for license details.

# Defined for each platform

simif_dir = $(firesim_base_dir)/midas/src/main/cc
midas_h  = $(shell find $(simif_dir) -name "*.h")
midas_cc = $(shell find $(simif_dir) -name "*.cc")

common_cxx_flags := $(TARGET_CXX_FLAGS) -Wno-unused-variable -DFIRESIM
common_ld_flags := $(TARGET_LD_FLAGS) -lrt

# Native compile-time NIC loopback uses its own bridge lifecycle adapter. Keep
# it separate from the unchanged MIDAS oracle and make header edits rebuild the
# actual driver when the CIRCT compiler is selected.
ifeq ($(GOLDENGATE_COMPILER),circt)
goldengate_circt_runtime := $(firesim_base_dir)/goldengate-circt/runtime
midas_h += $(wildcard $(goldengate_circt_runtime)/bridges/*.h)
common_cxx_flags += -I$(goldengate_circt_runtime)
endif
