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

# CheckCombLoops metadata has a defined sink/source rename policy. Preserve
# source order and multiplicity; validate all targets before channel mutation.
file(READ "${FIXTURES}/SRAMModelChannels.json" path_seed)
string(JSON path_count LENGTH "${path_seed}")
string(JSON path_seed SET "${path_seed}" ${path_count}
  "{\"class\":\"firrtl.transforms.CombinationalPath\",\"sink\":\"~FAMETop|ram>r_data\",\"sources\":[\"~FAMETop|ram>r_en\",\"~FAMETop|ram>r_addr\",\"~FAMETop|ram>r_en\"]}")
file(WRITE "${OUTPUT}/combinational-path.json" "${path_seed}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
  --annotation-file "${OUTPUT}/combinational-path.json"
  --output-dir "${OUTPUT}/combinational-path" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM CombinationalPath transfer failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/combinational-path/post-sram-fame-all.json" path_result)
string(JSON path_result_count LENGTH "${path_result}")
math(EXPR path_last "${path_result_count} - 1")
set(paths 0)
foreach(index RANGE 0 ${path_last})
  string(JSON class GET "${path_result}" ${index} class)
  if(class STREQUAL "firrtl.transforms.CombinationalPath")
    math(EXPR paths "${paths} + 1")
    string(JSON sink GET "${path_result}" ${index} sink)
    string(JSON sources LENGTH "${path_result}" ${index} sources)
    string(JSON first GET "${path_result}" ${index} sources 0)
    string(JSON second GET "${path_result}" ${index} sources 1)
    string(JSON third GET "${path_result}" ${index} sources 2)
    if(NOT sink STREQUAL "~FAMETop|ram>r_data_source.bits" OR NOT sources EQUAL 3 OR
       NOT first STREQUAL "~FAMETop|ram>r_en_sink.bits" OR
       NOT second STREQUAL "~FAMETop|ram>r_addr_sink.bits" OR NOT third STREQUAL first)
      message(FATAL_ERROR "CombinationalPath payload identity/order differs")
    endif()
  endif()
endforeach()
if(NOT paths EQUAL 1)
  message(FATAL_ERROR "CombinationalPath annotation multiplicity differs")
endif()
foreach(bad_sink "~FAMETop|ram>r_addr" "~FAMETop|ram>missing")
  string(JSON invalid_path SET "${path_seed}" ${path_count} sink "\"${bad_sink}\"")
  file(WRITE "${OUTPUT}/invalid-path.json" "${invalid_path}")
  execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
    --annotation-file "${OUTPUT}/invalid-path.json"
    --output-dir "${OUTPUT}/invalid-path" --rewrite-sram-fame
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(status EQUAL 0 OR NOT stderr MATCHES "CombinationalPath needs a ground output sink and input sources" OR
     EXISTS "${OUTPUT}/invalid-path/post-sram-fame.fir")
    message(FATAL_ERROR "Invalid CombinationalPath sink was not rejected atomically: ${stdout}\n${stderr}")
  endif()
endforeach()
message(STATUS "Passed CombinationalPath ordered payload transfer and atomic invalid-target rejection")

# Transform both ends of the closed four-instance graph and put actual native
# queues between parent and SRAM channels. Internal data channels must not be
# exposed as bridge inputs; only the Boolean clock packet crosses the wrapper.
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/transport" --rewrite-sram-transport
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM parent transport failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/transport/post-sram-transport.fir" transport_fir)
string(REGEX MATCHALL "inst PipeChannel_[^\n]* of GGFAMEPipe" queues "${transport_fir}")
list(LENGTH queues queue_count)
string(REGEX MATCHALL "reg [^\n]*_fired_[0-9]+ : UInt<1>" transport_fired "${transport_fir}")
list(LENGTH transport_fired transport_fired_count)
string(FIND "${transport_fir}" "circuit GGFAMEPipeWrapper :" active_wrapper)
string(FIND "${transport_fir}" "bits : UInt<1>[1]" boolean_clock)
string(FIND "${transport_fir}" "clock_sink.bits" raw_clock_token)
string(FIND "${transport_fir}" "m0_p0_ram_r_data_fired_0 <= mux(targetCycleFinishing, not(asUInt(clock_sink.bits))" parent_input_rule)
string(FIND "${transport_fir}" "m0_p0_ram_r_addr_fired_0 <= mux(targetCycleFinishing, not(clock_enabled)" parent_output_rule)
if(NOT queue_count EQUAL 52 OR NOT transport_fired_count EQUAL 65 OR
   active_wrapper LESS 0 OR boolean_clock LESS 0 OR raw_clock_token LESS 0 OR
   parent_input_rule LESS 0 OR parent_output_rule LESS 0)
  message(FATAL_ERROR "SRAM transport lacks complete native hardware: queues=${queue_count}, fired=${transport_fired_count}")
endif()
message(STATUS "Passed native 52-pipe parent/SRAM transport and raw/buffered clock FSM rules")

execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/parent-fame" --rewrite-sram-parent-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM parent FAME boundary failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/parent-fame/post-sram-parent-fame.fir" parent_fir)
string(REGEX MATCHALL "reg [^\n]*_fired_[0-9]+ : UInt<1>" parent_fired "${parent_fir}")
list(LENGTH parent_fired parent_fired_count)
if(NOT parent_fired_count EQUAL 65 OR parent_fir MATCHES "circuit GGFAMEPipeWrapper" OR
   parent_fir MATCHES "inst PipeChannel_" OR NOT parent_fir MATCHES "circuit FAMETop")
  message(FATAL_ERROR "Parent FAME boundary has missing FSMs or premature queues")
endif()
message(STATUS "Passed native parent/SRAM FAME boundary before queue construction")

# Debug selections use both ReferenceTarget and SFC ComponentName spelling.
# Transfer only their target member, preserve duplicates and signed payloads,
# and leave functional FAME hardware unchanged.
file(READ "${FIXTURES}/SRAMModelChannels.json" debug_annos)
string(JSON debug_count LENGTH "${debug_annos}")
foreach(probe "~FAMETop|ram>r.addr" "FAMETop.ram.r.addr"
              "~FAMETop|ram>r.data" "FAMETop.ram.r.data" "FAMETop.ram.r.addr")
  string(JSON debug_annos SET "${debug_annos}" ${debug_count}
    "{\"class\":\"midas.InternalFirrtlFpgaDebugAnnotation\",\"target\":\"${probe}\"}")
  math(EXPR debug_count "${debug_count} + 1")
endforeach()
file(WRITE "${OUTPUT}/debug.json" "${debug_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
  --annotation-file "${OUTPUT}/debug.json"
  --output-dir "${OUTPUT}/debug" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM debug transfer failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/debug/post-sram-fame.fir" debug_fir)
if(NOT debug_fir STREQUAL fame_fir)
  message(FATAL_ERROR "Debug selections changed SRAM FAME hardware")
endif()
file(READ "${OUTPUT}/debug/post-sram-fame-all.json" debug_output)
foreach(probe "~FAMETop|ram>r_addr_sink.bits" "FAMETop.ram.r_addr_sink.bits"
              "~FAMETop|ram>r_data_source.bits" "FAMETop.ram.r_data_source.bits")
  string(FIND "${debug_output}" "\"${probe}\"" found)
  if(found LESS 0)
    message(FATAL_ERROR "SRAM debug payload target missing: ${probe}")
  endif()
endforeach()
string(JSON debug_annos SET "${debug_annos}" ${debug_count}
  "{\"class\":\"midas.InternalFirrtlFpgaDebugAnnotation\",\"target\":\"~FAMETop|ram>r.addr\",\"unrelated\":\"~FAMETop|ram>r.data\"}")
file(WRITE "${OUTPUT}/invalid-debug.json" "${debug_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/single.fir"
  --annotation-file "${OUTPUT}/invalid-debug.json"
  --output-dir "${OUTPUT}/invalid-debug" --rewrite-sram-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "debug selection needs a single ground port target" OR
   EXISTS "${OUTPUT}/invalid-debug/post-sram-fame.fir")
  message(FATAL_ERROR "Malformed debug metadata was not rejected atomically: ${stdout}\n${stderr}")
endif()
message(STATUS "Passed SRAM debug payload identity and strict metadata rejection")

# Bridge passthroughs are promoted top connections, not model ports. Exercise
# unsigned and signed payloads and retained source-list order/multiplicity.
file(READ "${FIXTURES}/SRAMModelChannels.fir" passthrough_fir)
string(REPLACE "    inst Top of Top" "    input loop_in : UInt<8>\n    output loop_out : UInt<8>\n    input signed_in : SInt<13>\n    output signed_out : SInt<13>\n    loop_out <= loop_in\n    signed_out <= signed_in\n    inst Top of Top" passthrough_fir "${passthrough_fir}")
file(WRITE "${OUTPUT}/passthrough.fir" "${passthrough_fir}")
file(READ "${FIXTURES}/SRAMModelChannels.json" passthrough_annos)
string(JSON passthrough_count LENGTH "${passthrough_annos}")
foreach(port loop_in loop_out signed_in signed_out)
  if(port MATCHES "_in$")
    set(endpoint sinks)
  else()
    set(endpoint sources)
  endif()
  string(JSON passthrough_annos SET "${passthrough_annos}" ${passthrough_count}
    "{\"class\":\"midas.passes.fame.FAMEChannelConnectionAnnotation\",\"globalName\":\"external_${port}\",\"channelInfo\":{\"class\":\"midas.passes.fame.PipeChannel\",\"latency\":0},\"${endpoint}\":[\"~FAMETop|FAMETop>${port}\"]}")
  math(EXPR passthrough_count "${passthrough_count} + 1")
endforeach()
string(JSON passthrough_annos SET "${passthrough_annos}" ${passthrough_count}
  "{\"class\":\"firrtl.transforms.CombinationalPath\",\"sink\":\"~FAMETop|FAMETop>signed_out\",\"sources\":[\"~FAMETop|FAMETop>signed_in\",\"~FAMETop|FAMETop>signed_in\"]}")
math(EXPR passthrough_count "${passthrough_count} + 1")
file(WRITE "${OUTPUT}/passthrough.json" "${passthrough_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/passthrough.fir"
  --annotation-file "${OUTPUT}/passthrough.json"
  --output-dir "${OUTPUT}/passthrough" --rewrite-sram-parent-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Top passthrough FAME failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/passthrough/post-sram-parent-fame.fir" passthrough_output)
foreach(pair loop signed)
  if(pair STREQUAL "loop")
    set(payload "UInt<8>")
  else()
    set(payload "SInt<13>")
  endif()
  foreach(direction input output)
    if(direction STREQUAL "input")
      set(suffix in_sink)
    else()
      set(suffix out_source)
    endif()
    string(FIND "${passthrough_output}" "${direction} external_${pair}_${suffix} : { flip ready : UInt<1>, valid : UInt<1>, bits : ${payload} }" port)
    if(port LESS 0)
      message(FATAL_ERROR "Top passthrough ${pair} ${direction} Decoupled ABI differs")
    endif()
  endforeach()
  string(FIND "${passthrough_output}" "external_${pair}_out_source <= external_${pair}_in_sink" connection)
  if(connection LESS 0 OR passthrough_output MATCHES "(input|output) ${pair}_(in|out) :")
    message(FATAL_ERROR "Top passthrough ${pair} has stale scalar ports or incomplete handshake")
  endif()
endforeach()
file(READ "${OUTPUT}/passthrough/post-sram-parent-fame-all.json" passthrough_metadata)
string(JSON metadata_count LENGTH "${passthrough_metadata}")
math(EXPR metadata_last "${metadata_count} - 1")
set(path_matches 0)
foreach(index RANGE 0 ${metadata_last})
  string(JSON class GET "${passthrough_metadata}" ${index} class)
  if(class STREQUAL "firrtl.transforms.CombinationalPath")
    math(EXPR path_matches "${path_matches} + 1")
    string(JSON sink GET "${passthrough_metadata}" ${index} sink)
    string(JSON sources LENGTH "${passthrough_metadata}" ${index} sources)
    string(JSON first GET "${passthrough_metadata}" ${index} sources 0)
    string(JSON second GET "${passthrough_metadata}" ${index} sources 1)
    if(NOT sink STREQUAL "~FAMETop|FAMETop>external_signed_out_source.bits" OR
       NOT sources EQUAL 2 OR NOT first STREQUAL "~FAMETop|FAMETop>external_signed_in_sink.bits" OR
       NOT first STREQUAL second)
      message(FATAL_ERROR "Top passthrough CombinationalPath payload identity/multiplicity differs")
    endif()
  endif()
endforeach()
if(NOT path_matches EQUAL 1)
  message(FATAL_ERROR "Top passthrough lost or duplicated CombinationalPath")
endif()
set(passthrough_debug "${passthrough_annos}")
set(passthrough_debug_count ${passthrough_count})
foreach(probe "~FAMETop|FAMETop>signed_in" "FAMETop.FAMETop.signed_out")
  string(JSON passthrough_debug SET "${passthrough_debug}" ${passthrough_debug_count}
    "{\"class\":\"midas.InternalFirrtlFpgaDebugAnnotation\",\"target\":\"${probe}\"}")
  math(EXPR passthrough_debug_count "${passthrough_debug_count} + 1")
endforeach()
file(WRITE "${OUTPUT}/passthrough-debug.json" "${passthrough_debug}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/passthrough.fir"
  --annotation-file "${OUTPUT}/passthrough-debug.json"
  --output-dir "${OUTPUT}/passthrough-debug" --rewrite-sram-parent-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Signed passthrough debug transfer failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/passthrough-debug/post-sram-parent-fame.fir" passthrough_debug_fir)
file(READ "${OUTPUT}/passthrough-debug/post-sram-parent-fame-all.json" passthrough_debug_output)
string(FIND "${passthrough_debug_output}" "~FAMETop|FAMETop>external_signed_in_sink.bits" input_probe)
string(FIND "${passthrough_debug_output}" "FAMETop.FAMETop.external_signed_out_source.bits" output_probe)
if(NOT passthrough_debug_fir STREQUAL passthrough_output OR input_probe LESS 0 OR output_probe LESS 0)
  message(FATAL_ERROR "Signed passthrough debug changed hardware or lost payload identity")
endif()
string(JSON passthrough_annos SET "${passthrough_annos}" ${passthrough_count}
  "{\"class\":\"example.UnsupportedPortAnnotation\",\"target\":\"~FAMETop|FAMETop>loop_in\"}")
file(WRITE "${OUTPUT}/passthrough-unsupported.json" "${passthrough_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/passthrough.fir"
  --annotation-file "${OUTPUT}/passthrough-unsupported.json"
  --output-dir "${OUTPUT}/passthrough-unsupported" --rewrite-sram-parent-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "data target has unsupported retained annotation metadata" OR
   EXISTS "${OUTPUT}/passthrough-unsupported/post-sram-parent-fame.fir")
  message(FATAL_ERROR "Unsupported top passthrough metadata was not rejected atomically: ${stdout}\n${stderr}")
endif()
message(STATUS "Passed unsigned/signed top passthrough Decoupled wiring and ordered metadata transfer")

# Generated channel names may not replace unrelated surviving scalar ports.
string(REPLACE "    input loop_in : UInt<8>" "    input external_loop_in_sink : UInt<8>\n    input loop_in : UInt<8>" collision_fir "${passthrough_fir}")
file(WRITE "${OUTPUT}/passthrough-name-collision.fir" "${collision_fir}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/passthrough-name-collision.fir"
  --annotation-file "${OUTPUT}/passthrough.json"
  --output-dir "${OUTPUT}/passthrough-name-collision" --rewrite-sram-parent-fame
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "channel name already exists" OR
   EXISTS "${OUTPUT}/passthrough-name-collision/post-sram-parent-fame.fir")
  message(FATAL_ERROR "Top passthrough name collision was not rejected atomically: ${stdout}\n${stderr}")
endif()

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

# Carry native gate identity through transport and RAM replacement before XDC
# export. Virtual SRAM gates must disappear with the original wrapper bodies;
# only the surviving hub gate is constrained, even with four RAM instances.
file(READ "${FIXTURES}/SRAMModelChannels.json" timing_annos)
string(JSON timing_count LENGTH "${timing_annos}")
string(JSON timing_annos SET "${timing_annos}" ${timing_count}
  "{\"class\":\"midas.targetutils.xdc.XDCPathToCircuitAnnotation\",\"preLinkPath\":\"pre/link\",\"postLinkPath\":\"post/link\"}")
string(JSON timing_annos SET "${timing_annos}" 1 channelInfo perClockMFMR 0 "3")
file(WRITE "${OUTPUT}/timing-models.json" "${timing_annos}")
file(READ "${FIXTURES}/SRAMModelChannels.fir" timing_input)
string(REGEX REPLACE "      readwriter => rw\n" "" timing_input "${timing_input}")
string(REGEX REPLACE "    ram\\.rw[^\n]*\n" "" timing_input "${timing_input}")
file(WRITE "${OUTPUT}/timing-models.fir" "${timing_input}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/timing-models.fir"
  --annotation-file "${OUTPUT}/timing-models.json"
  --output-dir "${OUTPUT}/timing-models" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM timing/transport boundary failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/timing-models/post-sram-models.fir" timing_fir)
file(READ "${OUTPUT}/timing-models/post-sram-models.implementation.xdc" implementation_xdc)
file(READ "${OUTPUT}/timing-models/post-sram-models.synthesis.xdc" synthesis_xdc)
string(REGEX MATCHALL "inst [^\n]*_buffer of AbstractClockGate" surviving_gates "${timing_fir}")
list(LENGTH surviving_gates gate_count)
if(NOT gate_count EQUAL 1 OR NOT timing_fir MATCHES "module RamModel" OR
   NOT timing_fir MATCHES "inst model of RamModel" OR
   NOT implementation_xdc MATCHES "post/link/target_FAMETop/Top/clock_buffer/O" OR
   NOT implementation_xdc MATCHES "set_multicycle_path 3 -setup" OR
   NOT implementation_xdc MATCHES "set_multicycle_path 2 -hold" OR
   synthesis_xdc MATCHES "create_generated_clock")
  message(FATAL_ERROR "Native timing/storage/clock collateral differs: gates=${gate_count}, XDC=${implementation_xdc}")
endif()
# A late collateral failure must leave no partial candidate output, despite
# already constructing FAME, transport and RAM bodies on the staged circuit.
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/timing-models.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/missing-xdc-path" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "exactly one circuit path annotation" OR
   EXISTS "${OUTPUT}/missing-xdc-path/post-sram-models.fir")
  message(FATAL_ERROR "Missing XDC path did not reject the complete staged boundary: ${stdout}\n${stderr}")
endif()
message(STATUS "Passed native SRAM timing/transport/XDC boundary and late failure rejection")

# Full simulator assembly needs the same hardware before a platform hierarchy
# and XDC path exist. Keep generated-clock snippets and native gate identity
# for the late constraint pass; do not prematurely serialize path-dependent XDC.
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/timing-models.fir"
  --annotation-file "${FIXTURES}/SRAMModelChannels.json"
  --output-dir "${OUTPUT}/deferred-xdc" --rewrite-sram-hardware
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "SRAM hardware with deferred XDC failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/deferred-xdc/post-sram-hardware.fir" deferred_fir)
file(READ "${OUTPUT}/deferred-xdc/post-sram-hardware-all.json" deferred_annos)
if(NOT deferred_fir STREQUAL timing_fir OR
   deferred_annos MATCHES "midas.passes.XDCOutputAnnotation" OR
   EXISTS "${OUTPUT}/deferred-xdc/post-sram-models.implementation.xdc")
  message(FATAL_ERROR "Deferred XDC changed hardware or emitted premature constraints")
endif()
message(STATUS "Passed identical SRAM hardware before platform XDC path resolution")

execute_process(COMMAND "${COMPILER}" "${FIXTURES}/SRAMModelChannels.fir"
  --annotation-file "${OUTPUT}/timing-models.json"
  --output-dir "${OUTPUT}/unsupported-timing-port" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "does not support readwrite ports" OR
   EXISTS "${OUTPUT}/unsupported-timing-port/post-sram-models.fir")
  message(FATAL_ERROR "Unsupported timing ABI was not rejected: ${stdout}\n${stderr}")
endif()

# Parent bridge channels may carry several ordered fields even when each SRAM
# command remains scalar. Reverse order and mix signed/unsigned widths to catch
# accidental sorting, scalar bits renames and incorrect pipe packing.
string(REPLACE "    inst Top of Top" "    input request_addr : UInt<2>\n    input request_data : SInt<8>\n    output reply_addr : UInt<2>\n    output reply_data : SInt<8>\n    output external_clock : Clock\n    inst Top of Top\n    Top.request_addr <= request_addr\n    Top.request_data <= request_data\n    reply_addr <= Top.reply_addr\n    reply_data <= Top.reply_data\n    external_clock <= Top.external_clock" grouped_fir "${timing_input}")
string(REPLACE "    inst m0 of Middle" "    input request_addr : UInt<2>\n    input request_data : SInt<8>\n    output reply_addr : UInt<2>\n    output reply_data : SInt<8>\n    output external_clock : Clock\n    external_clock <= clock\n    reply_addr <= xor(request_addr, UInt<2>(1))\n    reply_data <= asSInt(not(asUInt(request_data)))\n    inst m0 of Middle" grouped_fir "${grouped_fir}")
set(grouped_annos "${timing_annos}")
string(JSON grouped_count LENGTH "${grouped_annos}")
foreach(direction request reply)
  if(direction STREQUAL "request")
    set(endpoint sinks)
  else()
    set(endpoint sources)
  endif()
  string(JSON grouped_annos SET "${grouped_annos}" ${grouped_count}
    "{\"class\":\"midas.passes.fame.FAMEChannelConnectionAnnotation\",\"globalName\":\"${direction}\",\"channelInfo\":{\"class\":\"midas.passes.fame.PipeChannel\",\"latency\":0},\"clock\":\"~FAMETop|FAMETop>external_clock\",\"${endpoint}\":[\"~FAMETop|FAMETop>${direction}_data\",\"~FAMETop|FAMETop>${direction}_addr\"]}")
  math(EXPR grouped_count "${grouped_count} + 1")
endforeach()
string(JSON grouped_annos SET "${grouped_annos}" ${grouped_count}
  "{\"class\":\"firrtl.transforms.CombinationalPath\",\"sink\":\"~FAMETop|Top>reply_data\",\"sources\":[\"~FAMETop|Top>request_data\",\"~FAMETop|Top>request_data\"]}")
file(WRITE "${OUTPUT}/grouped-parent.fir" "${grouped_fir}")
file(WRITE "${OUTPUT}/grouped-parent.json" "${grouped_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/grouped-parent.fir"
  --annotation-file "${OUTPUT}/grouped-parent.json"
  --output-dir "${OUTPUT}/grouped-parent" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Grouped parent SRAM transport failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/grouped-parent/post-sram-models.fir" grouped_output)
foreach(direction request reply)
  if(direction STREQUAL "request")
    set(suffix sink)
  else()
    set(suffix source)
  endif()
  string(FIND "${grouped_output}" "${direction}_${suffix} : { flip ready : UInt<1>, valid : UInt<1>, bits : { _data : SInt<8>, _addr : UInt<2> } }" ordered_payload)
  if(ordered_payload LESS 0)
    message(FATAL_ERROR "Grouped parent ${direction} payload order/type differs")
  endif()
endforeach()
string(REGEX MATCHALL "inst PipeChannel_[^\n]* of GGFAMEPipe" grouped_queues "${grouped_output}")
list(LENGTH grouped_queues grouped_queue_count)
string(REGEX MATCHALL "inst PipeChannel_[^\n]* of GGFAMEPipe" timing_queues "${timing_fir}")
list(LENGTH timing_queues timing_queue_count)
math(EXPR expected_grouped_queue_count "${timing_queue_count} + 2")
if(NOT grouped_queue_count EQUAL expected_grouped_queue_count)
  message(FATAL_ERROR "Expected one pipe per grouped token, got ${grouped_queue_count}")
endif()
file(READ "${OUTPUT}/grouped-parent/post-sram-models-all.json" grouped_metadata)
string(JSON grouped_metadata_count LENGTH "${grouped_metadata}")
math(EXPR grouped_metadata_last "${grouped_metadata_count} - 1")
set(grouped_paths 0)
foreach(index RANGE 0 ${grouped_metadata_last})
  string(JSON class GET "${grouped_metadata}" ${index} class)
  if(class STREQUAL "firrtl.transforms.CombinationalPath")
    math(EXPR grouped_paths "${grouped_paths} + 1")
    string(JSON sink GET "${grouped_metadata}" ${index} sink)
    string(JSON first GET "${grouped_metadata}" ${index} sources 0)
    string(JSON second GET "${grouped_metadata}" ${index} sources 1)
    if(NOT sink STREQUAL "~GGFAMEPipeWrapper|Top>reply_source.bits._data" OR
       NOT first STREQUAL "~GGFAMEPipeWrapper|Top>request_sink.bits._data" OR
       NOT first STREQUAL second)
      message(FATAL_ERROR "Grouped parent field identities/multiplicity differ")
    endif()
  endif()
endforeach()
if(NOT grouped_paths EQUAL 1)
  message(FATAL_ERROR "Grouped parent lost or duplicated retained dependency metadata")
endif()
message(STATUS "Passed ordered mixed-width parent channels, token pipes and field metadata")

# Use the same selected SRAM hierarchy with forward target-valid and reverse
# target-ready tokens in both bridge directions. The wrapper must replace each
# pair with one ReadyValidChannel, keeping all internal SRAM command pipes.
string(REPLACE "    input request_addr : UInt<2>"
  "    input request_valid : UInt<1>\n    output request_ready : UInt<1>\n    output reply_valid : UInt<1>\n    input reply_ready : UInt<1>\n    input request_addr : UInt<2>" rv_fir "${grouped_fir}")
string(REPLACE "    Top.request_addr <= request_addr"
  "    Top.request_valid <= request_valid\n    request_ready <= Top.request_ready\n    reply_valid <= Top.reply_valid\n    Top.reply_ready <= reply_ready\n    Top.request_addr <= request_addr" rv_fir "${rv_fir}")
string(REPLACE "    external_clock <= clock"
  "    request_ready <= not(reply_ready)\n    reply_valid <= not(request_valid)\n    external_clock <= clock" rv_fir "${rv_fir}")
set(rv_annos "${timing_annos}")
string(JSON rv_count LENGTH "${rv_annos}")
foreach(direction request reply)
  if(direction STREQUAL "request")
    set(endpoint sinks)
    set(reverse_endpoint sources)
    set(valid_field validSink)
    set(ready_field readySource)
  else()
    set(endpoint sources)
    set(reverse_endpoint sinks)
    set(valid_field validSource)
    set(ready_field readySink)
  endif()
  string(JSON rv_annos SET "${rv_annos}" ${rv_count}
    "{\"class\":\"midas.passes.fame.FAMEChannelConnectionAnnotation\",\"globalName\":\"${direction}_fwd\",\"channelInfo\":{\"class\":\"midas.passes.fame.DecoupledForwardChannel\",\"${valid_field}\":\"~FAMETop|FAMETop>${direction}_valid\",\"${ready_field}\":\"~FAMETop|FAMETop>${direction}_ready\"},\"clock\":\"~FAMETop|FAMETop>external_clock\",\"${endpoint}\":[\"~FAMETop|FAMETop>${direction}_data\",\"~FAMETop|FAMETop>${direction}_addr\",\"~FAMETop|FAMETop>${direction}_valid\"]}")
  math(EXPR rv_count "${rv_count} + 1")
  string(JSON rv_annos SET "${rv_annos}" ${rv_count}
    "{\"class\":\"midas.passes.fame.FAMEChannelConnectionAnnotation\",\"globalName\":\"${direction}_rev\",\"channelInfo\":{\"class\":\"midas.passes.fame.DecoupledReverseChannel$\"},\"clock\":\"~FAMETop|FAMETop>external_clock\",\"${reverse_endpoint}\":[\"~FAMETop|FAMETop>${direction}_ready\"]}")
  math(EXPR rv_count "${rv_count} + 1")
endforeach()
file(WRITE "${OUTPUT}/ready-valid-parent.fir" "${rv_fir}")
file(WRITE "${OUTPUT}/ready-valid-parent.json" "${rv_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/ready-valid-parent.fir"
  --annotation-file "${OUTPUT}/ready-valid-parent.json"
  --output-dir "${OUTPUT}/ready-valid-parent" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Ready/valid parent SRAM transport failed: ${stdout}\n${stderr}")
endif()
file(READ "${OUTPUT}/ready-valid-parent/post-sram-models.fir" rv_output)
string(REGEX MATCHALL "inst ReadyValidChannel_[^\n]* of GGFAMEReadyValid10" rv_channels "${rv_output}")
list(LENGTH rv_channels rv_channel_count)
string(REGEX MATCHALL "inst PipeChannel_[^\n]* of GGFAMEPipe" rv_pipes "${rv_output}")
list(LENGTH rv_pipes rv_pipe_count)
if(NOT rv_channel_count EQUAL 2 OR NOT rv_pipe_count EQUAL timing_queue_count)
  message(FATAL_ERROR "Ready/valid pair or SRAM pipe multiplicity differs: ${rv_channel_count}/${rv_pipe_count}")
endif()
foreach(direction request reply)
  if(direction STREQUAL "request")
    set(suffix sink)
  else()
    set(suffix source)
  endif()
  string(FIND "${rv_output}" "${direction}_fwd_${suffix} : { flip ready : UInt<1>, valid : UInt<1>, bits : { valid : UInt<1>, bits : { data : SInt<8>, addr : UInt<2> } } }" normalized_payload)
  if(normalized_payload LESS 0)
    message(FATAL_ERROR "Ready/valid wrapper lost target-valid or mixed signed payload")
  endif()
endforeach()
# A reverse token with an unmatched pair name is not a valid handshake pair. The
# complete clone must fail without publishing a partial timing-model circuit.
string(REPLACE "\"globalName\" : \"reply_rev\"" "\"globalName\" : \"unpaired_rev\"" unpaired_annos "${rv_annos}")
file(WRITE "${OUTPUT}/ready-valid-unpaired.json" "${unpaired_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/ready-valid-parent.fir"
  --annotation-file "${OUTPUT}/ready-valid-unpaired.json"
  --output-dir "${OUTPUT}/ready-valid-unpaired" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "lacks a boundary handshake pair" OR
   EXISTS "${OUTPUT}/ready-valid-unpaired/post-sram-models.fir")
  message(FATAL_ERROR "Unpaired ready/valid transport published a partial SRAM candidate: ${stdout}\n${stderr}")
endif()
message(STATUS "Passed both parent ready/valid orientations and atomic unpaired rejection")

# A target on any grouped leaf still needs a defined annotation transfer.
math(EXPR grouped_count "${grouped_count} + 1")
string(JSON grouped_annos SET "${grouped_annos}" ${grouped_count}
  "{\"class\":\"example.UnsupportedPortAnnotation\",\"target\":\"~FAMETop|Top>request_addr\"}")
file(WRITE "${OUTPUT}/grouped-unsupported.json" "${grouped_annos}")
execute_process(COMMAND "${COMPILER}" "${OUTPUT}/grouped-parent.fir"
  --annotation-file "${OUTPUT}/grouped-unsupported.json"
  --output-dir "${OUTPUT}/grouped-unsupported" --rewrite-sram-models
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(status EQUAL 0 OR NOT stderr MATCHES "data target has unsupported retained annotation metadata" OR
   EXISTS "${OUTPUT}/grouped-unsupported/post-sram-models.fir")
  message(FATAL_ERROR "Unknown grouped leaf metadata was not rejected atomically: ${stdout}\n${stderr}")
endif()
