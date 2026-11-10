# Exercise the real FIRRTL JSON importer and retained-annotation exporter.
# Scala optional bridge constructor fields use explicit JSON null.
if(NOT DEFINED COMPILER OR NOT DEFINED WORK)
  message(FATAL_ERROR "COMPILER and WORK are required")
endif()
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/input.fir" "FIRRTL version 1.2.0\ncircuit NullOption:\n  module NullOption:\n    input clock : Clock\n    skip\n")
file(WRITE "${WORK}/input.json" [=[
[{"class":"test.NullOption","widgetConstructorKey":{
  "class":"firechip.bridgeinterfaces.TraceBundleWidths","retireWidth":1,
  "iaddrWidth":40,"insnWidth":32,"causeWidth":64,"tvalWidth":40,
  "wdataWidth":null},"nested":[null,{"optional":null},"present",false,7]}]
]=])
execute_process(COMMAND "${COMPILER}" "${WORK}/input.fir"
  --annotation-file "${WORK}/input.json" --output-dir "${WORK}/output" --lower-types
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Null annotation round trip failed: ${result}\n${output}\n${error}")
endif()
file(READ "${WORK}/output/post-lower-types-all.json" annotations)
string(JSON key_type TYPE "${annotations}" 0 widgetConstructorKey wdataWidth)
string(JSON nested_type TYPE "${annotations}" 0 nested 0)
string(JSON object_type TYPE "${annotations}" 0 nested 1 optional)
string(JSON text_type TYPE "${annotations}" 0 nested 2)
string(JSON bool_type TYPE "${annotations}" 0 nested 3)
string(JSON integer GET "${annotations}" 0 nested 4)
if(NOT key_type STREQUAL "NULL" OR NOT nested_type STREQUAL "NULL"
    OR NOT object_type STREQUAL "NULL" OR NOT text_type STREQUAL "STRING"
    OR NOT bool_type STREQUAL "BOOLEAN" OR NOT integer STREQUAL "7")
  message(FATAL_ERROR "Retained annotation JSON types changed: ${annotations}")
endif()
message(STATUS "Explicit null constructor fields and nested JSON types preserved")

# Exercise the diagnostic consumer of the lazily parsed external JSON tree.
execute_process(COMMAND "${COMPILER}" "${WORK}/input.fir"
  --annotation-file "${WORK}/input.json" --output-dir "${WORK}/analysis"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Annotation analysis failed: ${result}\n${output}\n${error}")
endif()
file(READ "${WORK}/analysis/analysis.json" analysis)
string(JSON count GET "${analysis}" annotation_count)
string(JSON class_count GET "${analysis}" annotation_classes "test.NullOption")
if(NOT count EQUAL 1 OR NOT class_count EQUAL 1)
  message(FATAL_ERROR "Annotation analysis lost retained classes: ${analysis}")
endif()

# Production compilation relies on the importer to validate external JSON;
# the separate diagnostic JSON tree must not be required to reject bad input.
set(invalid_annotations "{" "{}" "null" "7" "\"text\"" "[null]" "[7]" "[] trailing")
set(index 0)
foreach(annotations IN LISTS invalid_annotations)
  math(EXPR index "${index} + 1")
  file(WRITE "${WORK}/invalid-${index}.json" "${annotations}")
  foreach(mode compile-baseline lower-types)
    execute_process(COMMAND "${COMPILER}" "${WORK}/input.fir"
      --annotation-file "${WORK}/invalid-${index}.json"
      --output-dir "${WORK}/invalid-${index}-${mode}" "--${mode}"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
    if(NOT result EQUAL 1 OR NOT error MATCHES "FIRRTL/annotation import failed")
      message(FATAL_ERROR "${mode} failed to reject malformed annotations ${annotations}: ${result}\n${output}\n${error}")
    endif()
  endforeach()
endforeach()
message(STATUS "Importer rejects malformed annotation JSON in production and diagnostic modes")
