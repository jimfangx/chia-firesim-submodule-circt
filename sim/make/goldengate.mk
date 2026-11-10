# See LICENSE for license details.

####################################
# Golden Gate Invocation           #
####################################

# Simulation memory map emitted by the MIDAS compiler
header := $(GENERATED_DIR)/$(BASE_FILE_NAME).const.h
verilog_header := $(GENERATED_DIR)/$(BASE_FILE_NAME).const.vh
partition_header := $(GENERATED_DIR)/$(BASE_FILE_NAME).partition.const.h
peer2peer_map := $(GENERATED_DIR)/$(BASE_FILE_NAME).peer2peer.const.yaml

# The midas-generated simulator RTL which will be baked into the FPGA shell project
simulator_verilog := $(GENERATED_DIR)/$(BASE_FILE_NAME).sv
simulator_xdc := $(GENERATED_DIR)/$(BASE_FILE_NAME).synthesis.xdc

# Pre-simulation-mapping annotations which includes all Bridge Annotations
# extracted used to generate new runtime configurations.
fame_annos := $(GENERATED_DIR)/post-bridge-extraction.json

# Keep the Scala implementation available as the reference while the CIRCT
# compiler is brought up. The normal compiler selection is CIRCT.
GOLDENGATE_COMPILER ?= circt
GOLDENGATE_CIRCT_BUILD_TYPE ?= Release
GOLDENGATE_CIRCT_PREFIX ?= $(abspath $(dir $(shell which firtool))/..)
goldengate_circt_build := $(GENERATED_DIR)/goldengate-circt-build
# Enabled CSV and printf counters use the ordinary full-platform MMIO/driver path.
GOLDENGATE_CIRCT_AUTOCOUNTER ?= $(if $(filter WithAutoCounter,$(subst _, ,$(PLATFORM_CONFIG))),csv,disabled)
GOLDENGATE_CIRCT_MULTITHREADING ?= disabled
# Compile-time LoopbackNIC is distinct from the driver's +nic-loopbackN option.
GOLDENGATE_CIRCT_NIC_MODE ?= normal
# Translate the standard AutoILA compiler fragments; explicit variables also
# support native invocations without a Scala compiler configuration instance.
goldengate_circt_config_fragments := $(subst _, ,$(PLATFORM_CONFIG))
GOLDENGATE_CIRCT_AUTOILA ?= $(if $(filter WithAutoILA,$(goldengate_circt_config_fragments)),enabled,disabled)
GOLDENGATE_CIRCT_ILA_DEPTH ?= $(or $(patsubst ILADepth%,%,$(firstword $(filter ILADepth%,$(goldengate_circt_config_fragments)))),1024)
GOLDENGATE_CIRCT_ILA_PROBE_TRIGGERS ?= 2

.PHONY: verilog compile
verilog: $(simulator_verilog)
compile: $(simulator_verilog)

# Disable FIRRTL 1.4 deduplication because it creates multiple failures
# Run the 1.3 version instead (checked-in). If dedup must be completely disabled,
# pass --no-legacy-dedup as well
ifeq ($(GOLDENGATE_COMPILER),circt)
ifeq ($(GOLDENGATE_CIRCT_AUTOILA),enabled)
goldengate_circt_ila_flags := --enable-autoila --ila-depth $(GOLDENGATE_CIRCT_ILA_DEPTH) --ila-probe-triggers $(GOLDENGATE_CIRCT_ILA_PROBE_TRIGGERS)
else ifneq ($(GOLDENGATE_CIRCT_AUTOILA),disabled)
$(error GOLDENGATE_CIRCT_AUTOILA must be enabled or disabled)
endif
ifeq ($(GOLDENGATE_CIRCT_AUTOCOUNTER),printf)
goldengate_circt_feature_flags += --enable-autocounter-printf
else ifeq ($(GOLDENGATE_CIRCT_AUTOCOUNTER),csv)
goldengate_circt_feature_flags += --enable-autocounter-csv
else ifneq ($(GOLDENGATE_CIRCT_AUTOCOUNTER),disabled)
$(error GOLDENGATE_CIRCT_AUTOCOUNTER must be disabled, csv or printf)
endif
ifeq ($(GOLDENGATE_CIRCT_MULTITHREADING),enabled)
goldengate_circt_feature_flags += --enable-multithreading
else ifneq ($(GOLDENGATE_CIRCT_MULTITHREADING),disabled)
$(error GOLDENGATE_CIRCT_MULTITHREADING must be enabled or disabled)
endif
ifeq ($(GOLDENGATE_CIRCT_NIC_MODE),loopback)
goldengate_circt_feature_flags += --nic-mode=loopback
else ifneq ($(GOLDENGATE_CIRCT_NIC_MODE),normal)
$(error GOLDENGATE_CIRCT_NIC_MODE must be normal or loopback)
endif
.PHONY: goldengate-circt-force
goldengate-circt-force:

# An existing SFC .sv must never make the CIRCT selection appear successful.
$(simulator_verilog) $(simulator_xdc) $(header) $(verilog_header) $(partition_header) $(peer2peer_map) $(fame_annos) &: $(FIRRTL_FILE) $(ANNO_FILE) goldengate-circt-force
	cmake -S $(firesim_base_dir)/goldengate-circt -B $(goldengate_circt_build) -G Ninja \
		-DCIRCT_DIR=$(GOLDENGATE_CIRCT_PREFIX)/lib/cmake/circt \
		-DCMAKE_BUILD_TYPE=$(GOLDENGATE_CIRCT_BUILD_TYPE) \
		-DBUILD_TESTING=OFF \
		-DZLIB_ROOT=$(abspath $(GOLDENGATE_CIRCT_PREFIX)/..)
	# Build the compiler here; CTest executables are built by focused validation.
	cmake --build $(goldengate_circt_build) --target goldengate-circt
	rm -f $(GENERATED_DIR)/circt-ingestion/firrtl_black_box_resource_files.f
	# A disabled rebuild must not retain a previous enabled build's ILA IP.
	rm -f $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).*.ipgen.tcl $(GENERATED_DIR)/$(BASE_FILE_NAME).*.ipgen.tcl
	$(goldengate_circt_build)/goldengate-circt $(FIRRTL_FILE) \
		--annotation-file $(ANNO_FILE) --output-dir $(GENERATED_DIR)/circt-ingestion \
		--compile-baseline --platform-config $(PLATFORM_CONFIG) \
		--output-filename-base $(BASE_FILE_NAME) $(goldengate_circt_ila_flags) $(goldengate_circt_feature_flags)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).sv $(simulator_verilog)
	@if test -f $(GENERATED_DIR)/circt-ingestion/firrtl_black_box_resource_files.f; then \
		while IFS= read -r blackbox || test -n "$$blackbox"; do \
			if test -n "$$blackbox"; then \
				cat "$$blackbox" >> $(simulator_verilog) || exit 1; \
				printf '\n' >> $(simulator_verilog) || exit 1; \
			fi; \
		done < $(GENERATED_DIR)/circt-ingestion/firrtl_black_box_resource_files.f; \
	fi
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).memory-origins.json $(GENERATED_DIR)/$(BASE_FILE_NAME).memory-origins.json
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).synthesis.xdc $(simulator_xdc)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).implementation.xdc $(GENERATED_DIR)/$(BASE_FILE_NAME).implementation.xdc
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).defines.vh $(GENERATED_DIR)/$(BASE_FILE_NAME).defines.vh
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).const.h $(header)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).const.vh $(verilog_header)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).partition.const.h $(partition_header)
	cp $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).peer2peer.const.yaml $(peer2peer_map)
	@for collateral in $(GENERATED_DIR)/circt-ingestion/$(BASE_FILE_NAME).*.ipgen.tcl; do \
		if test -f "$$collateral"; then cp "$$collateral" $(GENERATED_DIR)/; fi; \
	done
	cp $(GENERATED_DIR)/circt-ingestion/post-bridge-extraction-all.json $(fame_annos)
else ifeq ($(GOLDENGATE_COMPILER),sfc)
$(simulator_verilog) $(simulator_xdc) $(header) $(verilog_header) $(partition_header) $(peer2peer_map) $(fame_annos) &: $(FIRRTL_FILE) $(ANNO_FILE) $(FIRESIM_MAIN_CP)
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
