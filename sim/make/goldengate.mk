# See LICENSE for license details.

####################################
# Golden Gate Invocation           #
####################################

# Simulation memory map emitted by the MIDAS compiler
header := $(GENERATED_DIR)/$(BASE_FILE_NAME).const.h

# The midas-generated simulator RTL which will be baked into the FPGA shell project
simulator_verilog := $(GENERATED_DIR)/$(BASE_FILE_NAME).sv
simulator_xdc := $(GENERATED_DIR)/$(BASE_FILE_NAME).synthesis.xdc

# Pre-simulation-mapping annotations which includes all Bridge Annotations
# extracted used to generate new runtime configurations.
fame_annos := $(GENERATED_DIR)/post-bridge-extraction.json

# Keep the Scala implementation available as the reference while the CIRCT
# compiler is brought up. The normal compiler selection is CIRCT.
GOLDENGATE_COMPILER ?= circt
GOLDENGATE_CIRCT_PREFIX ?= $(abspath $(dir $(shell which firtool))/..)
goldengate_circt_build := $(GENERATED_DIR)/goldengate-circt-build
# The base FireSim compiler configs inherit EnableAutoCounter=false. Until
# enabled counter synthesis is ported, an explicit enabled request must fail.
GOLDENGATE_CIRCT_AUTOCOUNTER ?= disabled
GOLDENGATE_CIRCT_MULTITHREADING ?= disabled

.PHONY: verilog compile
verilog: $(simulator_verilog)
compile: $(simulator_verilog)

# Disable FIRRTL 1.4 deduplication because it creates multiple failures
# Run the 1.3 version instead (checked-in). If dedup must be completely disabled,
# pass --no-legacy-dedup as well
ifeq ($(GOLDENGATE_COMPILER),circt)
ifneq ($(findstring WithAutoCounter,$(PLATFORM_CONFIG)),)
$(error CIRCT Golden Gate has not ported the WithAutoCounter compiler config)
endif
ifneq ($(GOLDENGATE_CIRCT_AUTOCOUNTER),disabled)
$(error CIRCT Golden Gate currently supports GOLDENGATE_CIRCT_AUTOCOUNTER=disabled only)
endif
ifneq ($(findstring WithModelMultiThreading,$(PLATFORM_CONFIG)),)
$(error CIRCT Golden Gate has not ported model multithreading)
endif
ifneq ($(findstring MTModels,$(PLATFORM_CONFIG)),)
$(error CIRCT Golden Gate has not ported model multithreading)
endif
ifneq ($(GOLDENGATE_CIRCT_MULTITHREADING),disabled)
$(error CIRCT Golden Gate currently supports GOLDENGATE_CIRCT_MULTITHREADING=disabled only)
endif
.PHONY: goldengate-circt-force
goldengate-circt-force:

# An existing SFC .sv must never make the CIRCT selection appear successful.
$(simulator_verilog) $(simulator_xdc) $(header) $(fame_annos) &: $(FIRRTL_FILE) $(ANNO_FILE) goldengate-circt-force
	cmake -S $(firesim_base_dir)/goldengate-circt -B $(goldengate_circt_build) -G Ninja \
		-DCIRCT_DIR=$(GOLDENGATE_CIRCT_PREFIX)/lib/cmake/circt \
		-DZLIB_ROOT=$(abspath $(GOLDENGATE_CIRCT_PREFIX)/..)
	cmake --build $(goldengate_circt_build)
	$(goldengate_circt_build)/goldengate-circt $(FIRRTL_FILE) \
		--annotation-file $(ANNO_FILE) --output-dir $(GENERATED_DIR)/circt-ingestion \
		--compile-baseline --output-filename-base $(BASE_FILE_NAME)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).sv $(simulator_verilog)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).synthesis.xdc $(simulator_xdc)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).implementation.xdc $(GENERATED_DIR)/$(BASE_FILE_NAME).implementation.xdc
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).defines.vh $(GENERATED_DIR)/$(BASE_FILE_NAME).defines.vh
	@echo 'CIRCT Golden Gate emitted simulator RTL and XDC; driver headers and blackbox collateral remain unported, so compilation stops here.' >&2
	@exit 1
else ifeq ($(GOLDENGATE_COMPILER),sfc)
$(simulator_verilog) $(simulator_xdc) $(header) $(fame_annos) &: $(FIRRTL_FILE) $(ANNO_FILE) $(FIRESIM_MAIN_CP)
	$(call run_jar_scala_main,$(firesim_base_dir),$(FIRESIM_MAIN_CP),midas.stage.GoldenGateMain,\
		-i $(FIRRTL_FILE) \
		-td $(GENERATED_DIR) \
		-faf $(ANNO_FILE) \
		-ggcp $(PLATFORM_CONFIG_PACKAGE) \
		-ggcs $(PLATFORM_CONFIG) \
		--output-filename-base $(BASE_FILE_NAME) \
		--allow-unrecognized-annotations \
		--no-dedup)
	grep -sh ^ $(GENERATED_DIR)/firrtl_black_box_resource_files.f | \
		xargs cat >> $(simulator_verilog) # Append blackboxes to FPGA wrapper, if any
else
$(error Unknown GOLDENGATE_COMPILER '$(GOLDENGATE_COMPILER)'; use circt or sfc)
endif

####################################
# Runtime-Configuration Generation #
####################################

# This reads in the annotations from a generated target, elaborates a
# FASEDTimingModel if a BridgeAnnoation for one exists, and asks for user input
# to generate a runtime configuration that is compatible with the generated
# hardware (BridgeModule). Useful for modelling a memory system that differs from the default.
.PHONY: conf
conf: $(fame_annos) $(FIRESIM_MAIN_CP)
	mkdir -p $(GENERATED_DIR)
	$(call run_jar_scala_main,$(firesim_base_dir),$(FIRESIM_MAIN_CP),midas.stage.RuntimeConfigGeneratorMain,\
		-td $(GENERATED_DIR) \
		-faf $(fame_annos) \
		-ggcp $(PLATFORM_CONFIG_PACKAGE) \
		-ggcs $(PLATFORM_CONFIG))
