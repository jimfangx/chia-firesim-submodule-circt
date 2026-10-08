# See LICENSE for license details.
# This SFC-produced handoff has one selected multiport SRAM definition and
# four instances reached through repeated two-level parent uses.
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}" --analyze-sram-channels
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM channel preparation failed: ${stdout}\n${stderr}")
endif()
# The legacy parser needs real statement newlines, even with a large emitter
# margin. An oversized margin can make CIRCT print hard breaks as spaces.
file(STRINGS "${OUTPUT}/post-sram-channels.fir" header LIMIT_COUNT 2)
if(NOT header STREQUAL "FIRRTL version 1.2.0;circuit FAMETop :")
  message(FATAL_ERROR "SRAM boundary has an invalid legacy FIRRTL header")
endif()
file(READ "${OUTPUT}/post-sram-channels-all.json" annotations)
string(JSON count LENGTH "${annotations}")
math(EXPR last "${count} - 1")
set(models 0)
set(channels 0)
set(groups 0)
set(protected 0)
set(clock_domains)
set(protected_targets)
set(model_targets)
foreach(index RANGE 0 ${last})
  string(JSON class GET "${annotations}" ${index} class)
  if(class STREQUAL "midas.passes.fame.FAMETransformAnnotation")
    math(EXPR models "${models} + 1")
    string(JSON target GET "${annotations}" ${index} target)
    list(APPEND model_targets "${target}")
  elseif(class STREQUAL "midas.passes.fame.FAMEChannelConnectionAnnotation")
    math(EXPR channels "${channels} + 1")
    string(JSON info GET "${annotations}" ${index} channelInfo class)
    if(info STREQUAL "midas.passes.fame.PipeChannel")
      string(JSON clock GET "${annotations}" ${index} clock)
      list(APPEND clock_domains "${clock}")
    endif()
  elseif(class STREQUAL "midas.passes.fame.FAMEChannelPortsAnnotation")
    math(EXPR groups "${groups} + 1")
  elseif(class STREQUAL "firrtl.transforms.DontTouchAnnotation")
    math(EXPR protected "${protected} + 1")
    string(JSON target GET "${annotations}" ${index} target)
    list(APPEND protected_targets "${target}")
  endif()
endforeach()
list(REMOVE_DUPLICATES clock_domains)
list(REMOVE_DUPLICATES protected_targets)
list(REMOVE_DUPLICATES model_targets)
list(LENGTH clock_domains clocks)
list(LENGTH protected_targets unique_protected)
list(LENGTH model_targets unique_models)
if(NOT models EQUAL 2 OR NOT unique_models EQUAL models OR
   NOT channels EQUAL 53 OR NOT groups EQUAL 66 OR NOT clocks EQUAL 4 OR
   NOT protected EQUAL 70 OR NOT unique_protected EQUAL protected)
  message(FATAL_ERROR "SRAM graph differs from Scala: models=${models}, channels=${channels}, groups=${groups}, clocks=${clocks}, DontTouch=${protected}/${unique_protected}")
endif()
message(STATUS "Passed SRAM fanout, four clock domains, and unique model/port annotations")

# This boundary constructs hardware from the wrapped handoff itself; it does
# not ingest an SFC FAME-transformed model. The shared definition is clocked
# once, and all four promoted instances must receive host clock/reset ports.
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/virtual-clocks" --rewrite-sram-clocks
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM virtual clocks failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/virtual-clocks/post-sram-clocks.fir" clock_fir)
string(REGEX MATCHALL "ram\\.hostClock <= hostClock" host_clocks "${clock_fir}")
string(REGEX MATCHALL "ram\\.hostReset <= hostReset" host_resets "${clock_fir}")
string(REGEX MATCHALL "ram\\.clk <=" stale_clocks "${clock_fir}")
string(REGEX MATCHALL "\\.clk <= clk_buffer\\.O" memory_clocks "${clock_fir}")
list(LENGTH host_clocks host_clock_count)
list(LENGTH host_resets host_reset_count)
list(LENGTH stale_clocks stale_clock_count)
list(LENGTH memory_clocks memory_clock_count)
string(FIND "${clock_fir}" "clk_enabled <= mux(targetCycleFinishing, UInt<1>(1), clk_enabled)" enable_next)
if(NOT host_clock_count EQUAL 4 OR NOT host_reset_count EQUAL 4 OR
   NOT stale_clock_count EQUAL 0 OR NOT memory_clock_count EQUAL 3 OR
   enable_next LESS 0)
  message(FATAL_ERROR "SRAM host/virtual clock wiring differs: clocks=${host_clock_count}, resets=${host_reset_count}, stale=${stale_clock_count}, memory=${memory_clock_count}, enable=${enable_next}")
endif()
file(READ "${OUTPUT}/virtual-clocks/post-sram-clocks-all.json" clock_annotations)
if(NOT clock_annotations STREQUAL annotations)
  message(FATAL_ERROR "Clock substep changed retained SRAM/data/channel annotations")
endif()
message(STATUS "Passed native SRAM virtual clock hardware on four promoted instances")

# A clock target retained outside the local data groups has no deletion policy.
# Do not silently remove that annotated clock while preserving a stale target.
file(READ "${FIXTURES}/SRAMModelChannels.json" clock_seed)
string(JSON clock_seed_count LENGTH "${clock_seed}")
string(JSON clock_seed SET "${clock_seed}" ${clock_seed_count}
  "{\"class\":\"firrtl.transforms.DontTouchAnnotation\",\"target\":\"~FAMETop|ram>clk\"}")
file(WRITE "${OUTPUT}/protected-clock.json" "${clock_seed}")
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${OUTPUT}/protected-clock.json"
  --output-dir "${OUTPUT}/protected-clock" --rewrite-sram-clocks
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "SRAM virtual clock has a retained annotation target")
  message(FATAL_ERROR "Annotated SRAM clock was not rejected at the virtual-clock boundary: ${stdout}\n${stderr}")
endif()

# FAME operates on module definitions, so four RAM instances must have only
# two local output dependency records. Read latency 1 breaks both data paths.
file(READ "${OUTPUT}/post-sram-channel-dependencies.json" dependencies)
string(JSON dependency_count LENGTH "${dependencies}")
if(NOT dependency_count EQUAL 46)
  message(FATAL_ERROR "SRAM dependency graph has ${dependency_count} outputs, expected 46")
endif()
set(output_keys)
set(ram_outputs 0)
foreach(index RANGE 0 45)
  string(JSON module GET "${dependencies}" ${index} module)
  string(JSON output GET "${dependencies}" ${index} output_channel)
  list(APPEND output_keys "${module}/${output}")
  if(module STREQUAL "ram")
    math(EXPR ram_outputs "${ram_outputs} + 1")
    string(JSON inputs LENGTH "${dependencies}" ${index} input_channels)
    if(NOT inputs EQUAL 0)
      message(FATAL_ERROR "Synchronous SRAM output ${output} has combinational inputs")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES output_keys)
list(LENGTH output_keys unique_outputs)
if(NOT unique_outputs EQUAL dependency_count OR NOT ram_outputs EQUAL 2)
  message(FATAL_ERROR "Repeated SRAM instances duplicated local FAME dependencies")
endif()

# Existing labels and protection must also survive without being re-emitted.
file(READ "${FIXTURES}/SRAMModelChannels.json" seeded)
string(JSON seed_count LENGTH "${seeded}")
string(JSON seeded SET "${seeded}" ${seed_count}
  "{\"class\":\"midas.passes.fame.FAMETransformAnnotation\",\"target\":\"~FAMETop|Top\"}")
math(EXPR seed_count "${seed_count} + 1")
string(JSON seeded SET "${seeded}" ${seed_count}
  "{\"class\":\"firrtl.transforms.DontTouchAnnotation\",\"target\":\"~FAMETop|Top>clock\"}")
file(WRITE "${OUTPUT}/seeded.json" "${seeded}")
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${OUTPUT}/seeded.json"
  --output-dir "${OUTPUT}/seeded" --analyze-sram-channels
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Seeded SRAM preparation failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/seeded/post-sram-channels-all.json" seeded_output)
string(JSON seeded_count LENGTH "${seeded_output}")
if(NOT seeded_count EQUAL count)
  message(FATAL_ERROR "Existing annotations were duplicated: ${count} -> ${seeded_count}")
endif()

# Exercise a single instance as well as all four shared-definition instances.
# Derive a single-instance probe without changing the recorded fanout fixture.
file(READ "${FIXTURES}/SRAMModelChannels.fir" single)
string(REGEX REPLACE "    inst p1 of Parent\n" "" single "${single}")
string(REGEX REPLACE "    p1[^\n]*\n" "" single "${single}")
string(REGEX REPLACE "    inst m1 of Middle\n" "" single "${single}")
string(REGEX REPLACE "    m1[^\n]*\n" "" single "${single}")
file(WRITE "${OUTPUT}/single.fir" "${single}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/single-fame" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Single-instance SRAM FAME failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/single-fame/post-sram-fame.fir" fame_fir)
string(REGEX MATCHALL "reg [^\n]*_fired_[0-9]+ : UInt<1>" fired_regs "${fame_fir}")
list(LENGTH fired_regs fired_count)
string(FIND "${fame_fir}" "targetCycleFinishing <= " finishing)
string(FIND "${fame_fir}" "ram.r_addr_sink <= " top_input)
string(FIND "${fame_fir}" "ram_r_data_source <= Top_m0_p0_ram.r_data_source" top_output)
if(NOT fired_count EQUAL 13 OR finishing LESS 0 OR top_input LESS 0 OR top_output LESS 0)
  message(FATAL_ERROR "SRAM FAME has incomplete state or top bindings: fired=${fired_count}, finishing=${finishing}, input=${top_input}, output=${top_output}")
endif()
file(READ "${OUTPUT}/single-fame/post-sram-fame-all.json" fame_annotations)
string(FIND "${fame_annotations}" "~FAMETop|ram>r_addr_sink.bits" memory_addr)
string(FIND "${fame_annotations}" "~FAMETop|FAMETop>Top_m0_p0_ram_r_data_source.bits" channel_source)
if(memory_addr LESS 0 OR channel_source LESS 0)
  message(FATAL_ERROR "SRAM memory/channel targets were not transferred to payload fields")
endif()
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/repeated-fame" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Shared SRAM data rewrite failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/repeated-fame/post-sram-fame.fir" repeated_fir)
string(REGEX MATCHALL "reg [^\n]*_fired_[0-9]+ : UInt<1>" repeated_fired "${repeated_fir}")
list(LENGTH repeated_fired repeated_fired_count)
if(NOT repeated_fired_count EQUAL 13)
  message(FATAL_ERROR "Shared SRAM definition duplicated channel state: ${repeated_fired_count}")
endif()
foreach(instance Top_m0_p0_ram Top_m0_p1_ram Top_m1_p0_ram Top_m1_p1_ram)
  string(FIND "${repeated_fir}" "${instance}.r_addr_sink <= ${instance}_r_addr_sink" input_binding)
  string(FIND "${repeated_fir}" "${instance}_r_data_source <= ${instance}.r_data_source" output_binding)
  if(input_binding LESS 0 OR output_binding LESS 0)
    message(FATAL_ERROR "Shared SRAM instance ${instance} has stale channel bindings")
  endif()
endforeach()
message(STATUS "Passed native single/shared SRAM FAME state and per-instance payload bindings")

# Unknown metadata referring to a replaced data port needs its own transfer
# policy. Reject it instead of recursively changing arbitrary annotation text.
file(READ "${FIXTURES}/SRAMModelChannels.json" data_seed)
string(JSON data_seed_count LENGTH "${data_seed}")
string(JSON data_seed SET "${data_seed}" ${data_seed_count}
  "{\"class\":\"example.UnsupportedPortAnnotation\",\"target\":\"~FAMETop|ram>r_addr\"}")
file(WRITE "${OUTPUT}/protected-data.json" "${data_seed}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
  --annotation-file "${OUTPUT}/protected-data.json"
  --output-dir "${OUTPUT}/protected-data" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "data target has unsupported retained annotation metadata")
  message(FATAL_ERROR "Unknown SRAM data metadata was not rejected: ${stdout}\n${stderr}")
endif()
