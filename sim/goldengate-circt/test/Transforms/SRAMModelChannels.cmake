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
