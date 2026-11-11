# MemModel annotations select candidates for an optional pass. They do not
# enable GenerateMultiCycleRamModels in the supported base platform profiles.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK)
  message(FATAL_ERROR "COMPILER and WORK required")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(work "${WORK}/${suffix}")
file(MAKE_DIRECTORY "${work}")
file(WRITE "${work}/input.fir" [=[FIRRTL version 1.2.0
circuit Profile:
  module Profile:
    input clock : Clock
    mem rf:
      data-type => UInt<64>
      depth => 31
      read-latency => 0
      write-latency => 1
      reader => r
      writer => w
      read-under-write => undefined
    rf.r.clk <= clock
    rf.r.addr <= UInt<5>(0)
    rf.r.en <= UInt<1>(1)
    rf.w.clk <= clock
    rf.w.addr <= UInt<5>(1)
    rf.w.en <= UInt<1>(1)
    rf.w.data <= rf.r.data
    rf.w.mask <= UInt<1>(1)
]=])
file(WRITE "${work}/input.json" [=[
[{"class":"midas.passes.fame.FAMEChannelConnectionAnnotation",
  "globalName":"clocks","sinks":["~Profile|Profile>clock"],
  "channelInfo":{"class":"midas.passes.fame.TargetClockChannel",
    "clockInfo":[{"name":"base","multiplier":1,"divisor":1}],
    "perClockMFMR":[1]}},
 {"class":"midas.targetutils.FirrtlMemModelAnnotation",
  "target":"~Profile|Profile>rf"}]
]=])

foreach(config IN ITEMS BaseF2Config
    WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config
    BaseXilinxAlveoU250Config)
  set(out "${work}/${config}")
  execute_process(COMMAND "${COMPILER}" "${work}/input.fir"
    --annotation-file "${work}/input.json" --output-dir "${out}"
    --compile-baseline --emit-debug-snapshots --platform-config "${config}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 15)
  file(WRITE "${out}.log" "${output}\n${error}")
  # The no-data-channel fixture deliberately stops before simulator assembly.
  if(NOT result EQUAL 1 OR NOT error MATCHES "FAME model has no data channels")
    message(FATAL_ERROR "${config}: unexpected boundary ${result}\n${output}\n${error}")
  endif()
  file(READ "${out}/post-extract-model.mlir" ir)
  if(NOT ir MATCHES "firrtl.module @Profile\\(in %clock: !firrtl.clock\\)"
      OR ir MATCHES "firrtl.module @rf\\("
      OR NOT ir MATCHES "firrtl.mem interesting_name Undefined.*depth = 31 : i64, name = \"rf\".*readLatency = 0 : i32, writeLatency = 1 : i32"
      OR NOT ir MATCHES "data: uint<64>, mask: uint<1>")
    message(FATAL_ERROR "${config}: selected memory must retain its original inline geometry and ports")
  endif()
  file(READ "${out}/post-extract-model-all.json" annotations)
  if(NOT annotations MATCHES "midas.targetutils.FirrtlMemModelAnnotation"
      OR NOT annotations MATCHES "~FAMETop\\|Profile>rf"
      OR annotations MATCHES "midas.passes.fame.Model(Read|Write|ReadWrite)Port")
    message(FATAL_ERROR "${config}: memory selection was consumed or optional model ports were introduced")
  endif()
endforeach()

# Explicit optional boundaries must still extract that same memory, consuming
# its selection and introducing one read and one write model port.
foreach(mode IN ITEMS label-sram-models extract-sram-models)
  set(out "${work}/${mode}")
  execute_process(COMMAND "${COMPILER}" "${work}/input.fir"
    --annotation-file "${work}/input.json" --output-dir "${out}" "--${mode}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 15)
  file(WRITE "${out}.log" "${output}\n${error}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${mode}: explicit SRAM boundary failed ${result}\n${output}\n${error}")
  endif()
  file(READ "${out}/post-wrap-sram-models.mlir" ir)
  file(READ "${out}/post-wrap-sram-models-all.json" annotations)
  string(REGEX MATCHALL "midas.passes.fame.ModelReadPort" reads "${annotations}")
  string(REGEX MATCHALL "midas.passes.fame.ModelWritePort" writes "${annotations}")
  list(LENGTH reads read_count)
  list(LENGTH writes write_count)
  if(NOT ir MATCHES "firrtl.module @rf\\(" OR NOT read_count EQUAL 1
      OR NOT write_count EQUAL 1 OR annotations MATCHES "FirrtlMemModelAnnotation")
    message(FATAL_ERROR "${mode}: explicit SRAM wrapper/annotation contract changed")
  endif()
endforeach()
# Exercise the production configuration gate as well as the standalone passes.
# This probe intentionally lacks a complete bridge/platform graph, so inspect
# its extraction handoff and allow the later assembly boundary to reject it.
foreach(config IN ITEMS WithMultiCycleRamModels_BaseF2Config MCRams_BaseF2Config
    WithMultiCycleRamModels_BaseXilinxAlveoU250Config explicit-native-enable)
  set(extra)
  set(profile "${config}")
  if(config STREQUAL "explicit-native-enable")
    set(profile BaseF2Config)
    set(extra --enable-multicycle-ram-models)
  endif()
  set(out "${work}/enabled-${config}")
  execute_process(COMMAND "${COMPILER}" "${work}/input.fir"
    --annotation-file "${work}/input.json" --output-dir "${out}"
    --compile-baseline --emit-debug-snapshots --platform-config "${profile}" ${extra}
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 15)
  file(WRITE "${out}.log" "${output}\n${error}")
  if(NOT EXISTS "${out}/post-extract-model-all.json")
    message(FATAL_ERROR "${config}: did not reach enabled SRAM extraction ${result}\n${output}\n${error}")
  endif()
  file(READ "${out}/post-extract-model.mlir" ir)
  file(READ "${out}/post-extract-model-all.json" annotations)
  string(REGEX MATCHALL "midas.passes.fame.ModelReadPort" reads "${annotations}")
  string(REGEX MATCHALL "midas.passes.fame.ModelWritePort" writes "${annotations}")
  list(LENGTH reads read_count)
  list(LENGTH writes write_count)
  if(NOT ir MATCHES "firrtl.module @rf\\(" OR NOT read_count EQUAL 1
      OR NOT write_count EQUAL 1 OR annotations MATCHES "FirrtlMemModelAnnotation"
      OR NOT ir MATCHES "depth = 31 : i64" OR NOT ir MATCHES "data: uint<64>")
    message(FATAL_ERROR "${config}: enabled extraction lost source geometry or typed memory-port selections")
  endif()
endforeach()
message(STATUS "Base profiles retain inline SRAMs; configured F2/U250 and explicit native enables extract selected SRAMs")
