# Test configuration selection before opening a target. In particular, an
# FRFCFS selection must reach import without falling back to latency-pipe RTL.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK)
  message(FATAL_ERROR "COMPILER and WORK are required")
endif()
file(MAKE_DIRECTORY "${WORK}")
function(check_config config expected)
  execute_process(
    COMMAND "${COMPILER}" "${WORK}/absent-input.fir"
      --annotation-file "${WORK}/absent-input.json" --output-dir "${WORK}"
      --compile-baseline --platform-config "${config}" ${ARGN}
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE output
    TIMEOUT 10)
  if(NOT result EQUAL 1 OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "${config}: exit ${result}\n${output}\n${error}")
  endif()
endfunction()
check_config(BaseF2Config "cannot read FIRRTL")
check_config(BaseF2Config "cannot read FIRRTL" --emit-debug-snapshots)
check_config(BaseF2Config "duplicate --emit-debug-snapshots"
  --emit-debug-snapshots --emit-debug-snapshots)
check_config(BaseXilinxAlveoU250Config "cannot read FIRRTL")
check_config(WithAutoILA_BaseF2Config "cannot read FIRRTL")
check_config(BaseF2Config_BaseXilinxAlveoU250Config "duplicate or conflicting base")
check_config(BaseF2Config_BaseF2Config "duplicate or conflicting base")
check_config(WithAutoILA "requires BaseXilinxAlveoU250Config or BaseF2Config")
check_config(WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config
  "cannot read FIRRTL")
check_config(FRFCFS16GBQuadRankLLC4MB_BaseF2Config "cannot read FIRRTL")
check_config(FRFCFS16GBQuadRankLLC4MB_BaseXilinxAlveoU250Config
  "requires BaseF2Config")
check_config(FRFCFS16GBQuadRankLLC4MB_FRFCFS16GBQuadRankLLC4MB_BaseF2Config
  "duplicate FRFCFS16GBQuadRankLLC4MB")
check_config(BaseF2Config "cannot read FIRRTL"
  --stop-after-print-host-binding)
check_config(BaseXilinxAlveoU250Config "cannot read FIRRTL"
  --enable-autocounter-printf)
check_config(BaseXilinxAlveoU250Config "cannot read FIRRTL"
  --enable-autocounter-printf --stop-after-print-host-binding)
check_config(BaseXilinxAlveoU250Config "duplicate --enable-autocounter-printf"
  --enable-autocounter-printf --enable-autocounter-printf)
check_config(BaseF2Config "cannot read FIRRTL"
  --enable-autocounter-printf --stop-after-print-host-binding)
check_config(WithPrintfSynthesis_BaseF2Config "cannot read FIRRTL")
check_config(WithAutoCounterPrintf_BaseF2Config "cannot read FIRRTL")
check_config(WithAutoCounter_WithPrintfSynthesis_BaseF2Config "cannot read FIRRTL")
check_config(BaseF2Config "cannot read FIRRTL" --enable-autocounter-csv)
check_config(BaseF2Config "duplicate --enable-autocounter-csv" --enable-autocounter-csv --enable-autocounter-csv)
check_config(WithAutoCounterPrintf_BaseF2Config "mutually exclusive" --enable-autocounter-csv)
check_config(MTModels_BaseF2Config "cannot read FIRRTL")
check_config(WithModelMultiThreading_BaseF2Config "cannot read FIRRTL")
check_config(BaseF2Config "cannot read FIRRTL" --enable-synth-prints)
check_config(BaseF2Config "duplicate --enable-synth-prints" --enable-synth-prints --enable-synth-prints)
check_config(BaseF2Config "cannot read FIRRTL" --enable-multithreading)
check_config(BaseF2Config "duplicate --enable-multithreading" --enable-multithreading --enable-multithreading)
check_config(BaseF2Config "cannot read FIRRTL" --nic-mode=normal)
check_config(BaseF2Config "cannot read FIRRTL" --nic-mode=loopback)
check_config(BaseF2Config "--nic-mode must be normal or loopback" --nic-mode=invalid)
check_config(BaseF2Config "duplicate --nic-mode" --nic-mode=normal --nic-mode=loopback)
message(STATUS "F2/U250 and enabled Print/threading production configuration selection passed")

# A small diagnostic reaches the real importer and early transformations. Inspect
# the explicit platform handoff before functional materialization, without
# elaborating a target or running a long acceptance job.
file(WRITE "${WORK}/profile.fir" "FIRRTL version 1.2.0\ncircuit Profile:\n  module Profile:\n    input clock : Clock\n    skip\n")
file(WRITE "${WORK}/profile.json" [=[
[{"class":"midas.passes.fame.FAMEChannelConnectionAnnotation",
  "globalName":"clocks","sinks":["~Profile|Profile>clock"],
  "channelInfo":{"class":"midas.passes.fame.TargetClockChannel",
    "clockInfo":[{"name":"base","multiplier":1,"divisor":1}],
    "perClockMFMR":[1]}}]
]=])
foreach(capacity IN ITEMS 16 32)
  if(capacity EQUAL 32)
    set(config WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config)
  else()
    set(config BaseF2Config)
  endif()
  set(profile_dir "${WORK}/profile-${capacity}")
  execute_process(COMMAND "${COMPILER}" "${WORK}/profile.fir"
    --annotation-file "${WORK}/profile.json" --output-dir "${profile_dir}"
    --compile-baseline --emit-debug-snapshots --platform-config "${config}"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_VARIABLE output TIMEOUT 15)
  if(NOT result EQUAL 1 OR NOT error MATCHES "FAME model has no data channels")
    message(FATAL_ERROR "${config} did not reach expected FAME boundary: ${result}\n${output}\n${error}")
  endif()
  file(GLOB snapshots "${profile_dir}/*.mlir")
  if(NOT snapshots)
    message(FATAL_ERROR "${config} emitted no diagnostic snapshots")
  endif()
  foreach(snapshot IN LISTS snapshots)
    file(READ "${snapshot}" ir)
    if(NOT ir MATCHES "goldengate.fasedBaseCapacity = ${capacity} : i32")
      message(FATAL_ERROR "${snapshot}: missing explicit platform capacity ${capacity}")
    endif()
  endforeach()
endforeach()
message(STATUS "Actual F2 importer preserves explicit LBP16 / FRFCFS32 capacity handoff")
