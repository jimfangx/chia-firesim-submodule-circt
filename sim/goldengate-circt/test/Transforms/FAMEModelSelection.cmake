# See LICENSE for license details.
# Exercise the actual CLI selection boundary and verify emitted FIRRTL SSA.
foreach(model IN ITEMS Other Hub)
  if(model STREQUAL "Other")
    set(analysis FAMENoDataVirtual.json)
  else()
    set(analysis FAMEClockOnly.json)
  endif()
  execute_process(COMMAND "${COMPILER}" "${FIXTURES}/FAMENoDataModels.fir"
    --annotation-file "${FIXTURES}/FAMEInputOnlyModels.anno.json"
    --output-dir "${OUTPUT}/${model}"
    --rewrite-fame-output-valid-from "${FIXTURES}/${analysis}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(status EQUAL 0 OR NOT stderr MATCHES "FAME model has no data channels: ${model}")
    message(FATAL_ERROR "No-data ${model} did not reject like Scala: ${status}\n${stdout}\n${stderr}")
  endif()
endforeach()
execute_process(COMMAND "${COMPILER}" "${FIXTURES}/FAMEInputOnlyModels.fir"
  --annotation-file "${FIXTURES}/FAMEInputOnlyModels.anno.json"
  --output-dir "${OUTPUT}/selected"
  --rewrite-fame-output-valid-from "${FIXTURES}/FAMESelectedInputOnly.json"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Selected input-only model failed: ${status}\n${stdout}\n${stderr}")
endif()
execute_process(COMMAND "${CHECKER}" --selected-controls "${OUTPUT}/selected/output-valid.mlir"
  RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Selected/unselected SSA controls differ: ${status}\n${stdout}\n${stderr}")
endif()
message(STATUS "Passed two no-data CLI rejections and selected/unselected model controls")
