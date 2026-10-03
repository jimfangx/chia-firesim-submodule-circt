cmake_minimum_required(VERSION 3.20)
project(goldengate_circt LANGUAGES CXX)

find_package(CIRCT REQUIRED CONFIG)

add_executable(goldengate-circt
  tools/goldengate.cpp
  lib/Support/AnnotationEmission.cpp
  lib/Support/XDCEmission.cpp
  lib/Support/MetasimInterfaceHeader.cpp
  lib/Support/SimulationMasterHeader.cpp
  lib/Support/ClockBridgeHeader.cpp
  lib/Support/ResetPulseHeader.cpp
  lib/Support/LoadMemHeader.cpp
  lib/Support/PeekPokeHeader.cpp
  lib/Support/UARTHeader.cpp
  lib/Support/TSIHeader.cpp
  lib/Support/BlockDevHeader.cpp
  lib/Support/TracerVHeader.cpp
  lib/Support/CPUManagedStreamHeader.cpp
  lib/Support/FASEDHeader.cpp
  lib/Support/SimulatorRTL.cpp
  lib/Support/TargetUtils.cpp
  lib/Analysis/ChannelAnalysis.cpp
  lib/Analysis/BridgeAnalysis.cpp
  lib/Analysis/AutoCounterAnalysis.cpp
  lib/Analysis/ChannelClockInfo.cpp
  lib/Analysis/HierarchyAnalysis.cpp
  lib/Analysis/ModelAnalysis.cpp
  lib/Analysis/FAMEPortAnalysis.cpp
  lib/Analysis/CombDependencyAnalysis.cpp
  lib/Transforms/FAMEOutputValid.cpp
  lib/Transforms/FAMEDefaults.cpp
  lib/Transforms/FindDefaultClocks.cpp
  lib/Transforms/PromotePassthroughConnections.cpp
  lib/Transforms/ExtractModel.cpp
  lib/Transforms/WrapTop.cpp
  lib/Transforms/LowerTypes.cpp
  lib/Transforms/CoerceAsyncToSyncReset.cpp
  lib/Transforms/LabelMultiThreadedInstances.cpp
  lib/Transforms/TriggerWiring.cpp
  lib/Transforms/ChannelExcision.cpp
  lib/Transforms/InferModelPorts.cpp
  lib/Transforms/FAMEInputReady.cpp
  lib/Transforms/FAMEFinishing.cpp
  lib/Transforms/FAMEHostControl.cpp
  lib/Transforms/FAMEInputChannel.cpp
  lib/Transforms/FAMEOutputChannel.cpp
  lib/Transforms/FAMEPipeChannel.cpp
  lib/Transforms/FAMEReadyValidChannel.cpp
  lib/Transforms/FAMEClockChannel.cpp
  lib/Transforms/SingleClockBridge.cpp
  lib/Transforms/ClockBridgeControl.cpp
  lib/Transforms/ControlErrorSlave.cpp
  lib/Transforms/ControlAddressDecode.cpp
  lib/Transforms/ControlWriteRoute.cpp
  lib/Transforms/ControlWriteDispatch.cpp
  lib/Transforms/ControlWidgetWrites.cpp
  lib/Transforms/ControlReadDispatch.cpp
  lib/Transforms/ControlReadTracker.cpp
  lib/Transforms/ControlTransactionTracker.cpp
  lib/Transforms/ControlWriteTracker.cpp
  lib/Transforms/SimulationMaster.cpp
  lib/Transforms/SimulationMasterControl.cpp
  lib/Transforms/BlockDevWriteLatency.cpp
  lib/Transforms/BlockDevReadLatency.cpp
  lib/Transforms/FASEDTokenEngine.cpp
  lib/Transforms/FASEDHostOutstanding.cpp
  lib/Transforms/FASEDIngressIssue.cpp
  lib/Transforms/FASEDHostMemory.cpp
  lib/Transforms/FASEDAddressTranslation.cpp
  lib/Transforms/FASEDReadDeinterleaver.cpp
  lib/Transforms/FASEDHostMemoryBuffer.cpp
  lib/Transforms/HostMemoryWriteArbiter.cpp
  lib/Transforms/HostMemoryWriteResponses.cpp
  lib/Transforms/HostMemoryReadArbiter.cpp
  lib/Transforms/HostMemoryReadResponses.cpp
  lib/Transforms/HostMemoryOutputBuffer.cpp
  lib/Transforms/HostMemoryPort.cpp
  lib/Transforms/CPUStreamPort.cpp
  lib/Transforms/FPGATopShell.cpp
  lib/Transforms/F1Shim.cpp
  lib/Transforms/XilinxHostSpecialization.cpp
  lib/Transforms/FASEDIngressDeadlock.cpp
  lib/Transforms/FASEDReadBuffer.cpp
  lib/Transforms/FASEDReadScheduler.cpp
  lib/Transforms/FASEDWriteEgress.cpp
  lib/Transforms/FASEDResponseReleaser.cpp
  lib/Transforms/FASEDTimingCycle.cpp
  lib/Transforms/FASEDReadLatency.cpp
  lib/Transforms/FASEDWriteLatency.cpp
  lib/Transforms/FASEDTimingAWQueue.cpp
  lib/Transforms/FASEDWritePairing.cpp
  lib/Transforms/FASEDWriteRetirement.cpp
  lib/Transforms/FASEDWriteAdmission.cpp
  lib/Transforms/FASEDStatistics.cpp
  lib/Transforms/FASEDHistograms.cpp
  lib/Transforms/FASEDMMIOBank.cpp
  lib/Transforms/FASEDResponseErrors.cpp
  lib/Transforms/FASEDFunctionalModelRegister.cpp
  lib/Transforms/FASEDLatencyRegisters.cpp
  lib/Transforms/FASEDRequestLimits.cpp
  lib/Transforms/FASEDReadAdmission.cpp
  lib/Transforms/FASEDIngressOrder.cpp
  lib/Transforms/FASEDIngressCredits.cpp
  lib/Transforms/FASEDIngressARQueue.cpp
  lib/Transforms/FASEDIngressWQueue.cpp
  lib/Transforms/FASEDIngressAWQueue.cpp
  lib/Transforms/BlockDevResponseScheduler.cpp
  lib/Transforms/BlockDevTokenEngine.cpp
  lib/Transforms/BlockDevRequestQueue.cpp
  lib/Transforms/BlockDevDataQueue.cpp
  lib/Transforms/BlockDevReadResponseQueue.cpp
  lib/Transforms/BlockDevMMIOBank.cpp
  lib/Transforms/BlockDevWriteAckQueue.cpp
  lib/Transforms/TSITokenEngine.cpp
  lib/Transforms/TSIWordQueues.cpp
  lib/Transforms/TSIMMIOBank.cpp
  lib/Transforms/ControlReadArbiter.cpp
  lib/Transforms/ControlWriteArbiter.cpp
  lib/Transforms/ResetPulseBridge.cpp
  lib/Transforms/UARTSerialEngine.cpp
  lib/Transforms/UARTByteQueues.cpp
  lib/Transforms/UARTMMIOBank.cpp
  lib/Transforms/PeekPokeCycleEngine.cpp
  lib/Transforms/PeekPokeMMIOBank.cpp
  lib/Transforms/TracerVTokenEngine.cpp
  lib/Transforms/TracerVTriggerConfig.cpp
  lib/Transforms/TracerVStreamQueue.cpp
  lib/Transforms/CPUStreamRead.cpp
  lib/Transforms/CPUStreamCountBank.cpp
  lib/Transforms/EmptyCPUStreamWrite.cpp
  lib/Transforms/CPUStreamWriteBuffer.cpp
  lib/Transforms/CPUStreamReadBuffer.cpp
  lib/Transforms/CPUStreamResponseBuffer.cpp
  lib/Transforms/CPUStreamWriteResponseBuffer.cpp
  lib/Transforms/LoadMemWriter.cpp
  lib/Transforms/LoadMemRequests.cpp
  lib/Transforms/LoadMemWriteMMIO.cpp
  lib/Transforms/LoadMemWriteData.cpp
  lib/Transforms/LoadMemReadRequests.cpp
  lib/Transforms/LoadMemReadData.cpp
  lib/Transforms/FAMEFiredState.cpp)
target_compile_features(goldengate-circt PRIVATE cxx_std_17)
target_compile_options(goldengate-circt PRIVATE -fno-rtti)
target_include_directories(goldengate-circt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
target_link_libraries(goldengate-circt PRIVATE
  CIRCTImportFIRFile CIRCTExportFIRRTL CIRCTFirtool CIRCTFIRRTL CIRCTFIRRTLTransforms CIRCTHW CIRCTOM CIRCTSupport
  MLIRIR MLIRSupport LLVMSupport)

include(CTest)
if(BUILD_TESTING)
  add_executable(goldengate-fame-unused-outputs-test
    test/Transforms/FAMEUnusedOutputsTest.cpp lib/Transforms/FAMEInputChannel.cpp)
  target_compile_features(goldengate-fame-unused-outputs-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fame-unused-outputs-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fame-unused-outputs-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fame-unused-outputs-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fame-unused-outputs COMMAND goldengate-fame-unused-outputs-test)
  add_executable(goldengate-fame-clock-gate-test
    test/Transforms/FAMEClockGateTest.cpp lib/Transforms/FAMEInputChannel.cpp)
  target_compile_features(goldengate-fame-clock-gate-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fame-clock-gate-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fame-clock-gate-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fame-clock-gate-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fame-clock-gate COMMAND goldengate-fame-clock-gate-test)
  add_test(NAME goldengate-fame-multiple-virtual-clocks COMMAND goldengate-circt
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMultipleVirtualClocks.fir
    --annotation-file ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMultipleVirtualClocks.anno.json
    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/multiple-virtual-clocks
    --rewrite-fame-output-valid-from ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMultipleVirtualClocks.json)
  add_test(NAME goldengate-fame-multiple-virtual-clocks-check COMMAND goldengate-fame-fired-state-test
    --clock-controls ${CMAKE_CURRENT_BINARY_DIR}/multiple-virtual-clocks/output-valid.mlir)
  set_tests_properties(goldengate-fame-multiple-virtual-clocks PROPERTIES FIXTURES_SETUP fame-multiple-virtual-clocks)
  set_tests_properties(goldengate-fame-multiple-virtual-clocks-check PROPERTIES FIXTURES_REQUIRED fame-multiple-virtual-clocks)
  add_test(NAME goldengate-fame-mixed-clock-models COMMAND goldengate-circt
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMixedClockModels.fir
    --annotation-file ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMixedClockModels.anno.json
    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/mixed-clock-models
    --rewrite-fame-output-valid-from ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEMixedClockModels.json)
  add_test(NAME goldengate-fame-mixed-clock-models-check COMMAND goldengate-fame-fired-state-test
    --mixed-controls ${CMAKE_CURRENT_BINARY_DIR}/mixed-clock-models/output-valid.mlir)
  add_test(NAME goldengate-fame-mixed-clock-gates-check COMMAND goldengate-fame-fired-state-test
    --clock-controls ${CMAKE_CURRENT_BINARY_DIR}/mixed-clock-models/output-valid.mlir)
  set_tests_properties(goldengate-fame-mixed-clock-models PROPERTIES FIXTURES_SETUP fame-mixed-clock-models)
  set_tests_properties(goldengate-fame-mixed-clock-models-check goldengate-fame-mixed-clock-gates-check
    PROPERTIES FIXTURES_REQUIRED fame-mixed-clock-models)
  add_test(NAME goldengate-fame-input-only-models COMMAND goldengate-circt
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEInputOnlyModels.fir
    --annotation-file ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEInputOnlyModels.anno.json
    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/input-only-models
    --rewrite-fame-output-valid-from ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEInputOnlyModels.json)
  add_test(NAME goldengate-fame-input-only-models-check COMMAND goldengate-fame-fired-state-test
    --input-only-controls ${CMAKE_CURRENT_BINARY_DIR}/input-only-models/output-valid.mlir)
  add_test(NAME goldengate-fame-input-only-clock-gates-check COMMAND goldengate-fame-fired-state-test
    --clock-controls ${CMAKE_CURRENT_BINARY_DIR}/input-only-models/output-valid.mlir)
  set_tests_properties(goldengate-fame-input-only-models PROPERTIES FIXTURES_SETUP fame-input-only-models)
  set_tests_properties(goldengate-fame-input-only-models-check goldengate-fame-input-only-clock-gates-check
    PROPERTIES FIXTURES_REQUIRED fame-input-only-models)
  add_test(NAME goldengate-fame-model-selection COMMAND ${CMAKE_COMMAND}
    -DCOMPILER=$<TARGET_FILE:goldengate-circt>
    -DCHECKER=$<TARGET_FILE:goldengate-fame-fired-state-test>
    -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/model-selection
    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEModelSelection.cmake)
  add_executable(goldengate-fame-virtual-clock-port-test
    test/Transforms/FAMEVirtualClockPortTest.cpp lib/Transforms/FAMEInputChannel.cpp)
  target_compile_features(goldengate-fame-virtual-clock-port-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fame-virtual-clock-port-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fame-virtual-clock-port-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fame-virtual-clock-port-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fame-virtual-clock-port COMMAND goldengate-fame-virtual-clock-port-test)
  add_executable(goldengate-fame-fired-state-test
    test/Transforms/FAMEFiredStateTest.cpp lib/Transforms/FAMEFiredState.cpp
    lib/Transforms/FAMEFinishing.cpp)
  target_compile_features(goldengate-fame-fired-state-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fame-fired-state-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fame-fired-state-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fame-fired-state-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fame-fired-state COMMAND goldengate-fame-fired-state-test)
  add_test(NAME goldengate-fame-virtual-clock-controls COMMAND goldengate-circt
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEVirtualClock.fir
    --annotation-file ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEVirtualClock.anno.json
    --output-dir ${CMAKE_CURRENT_BINARY_DIR}/virtual-clock-controls
    --rewrite-fame-output-valid-from ${CMAKE_CURRENT_SOURCE_DIR}/test/Transforms/FAMEVirtualClock.json)
  add_test(NAME goldengate-fame-virtual-clock-controls-check COMMAND goldengate-fame-fired-state-test
    --virtual-controls ${CMAKE_CURRENT_BINARY_DIR}/virtual-clock-controls/output-valid.mlir)
  add_test(NAME goldengate-fame-virtual-clock-gate-check COMMAND goldengate-fame-fired-state-test
    --clock-controls ${CMAKE_CURRENT_BINARY_DIR}/virtual-clock-controls/output-valid.mlir)
  set_tests_properties(goldengate-fame-virtual-clock-controls PROPERTIES FIXTURES_SETUP fame-virtual-controls)
  set_tests_properties(goldengate-fame-virtual-clock-controls-check PROPERTIES FIXTURES_REQUIRED fame-virtual-controls)
  set_tests_properties(goldengate-fame-virtual-clock-gate-check PROPERTIES FIXTURES_REQUIRED fame-virtual-controls)
  add_executable(goldengate-coerce-reset-test
    test/Transforms/CoerceAsyncToSyncResetTest.cpp
    lib/Transforms/CoerceAsyncToSyncReset.cpp lib/Support/SimulatorRTL.cpp)
  target_compile_features(goldengate-coerce-reset-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-coerce-reset-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-coerce-reset-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-coerce-reset-test PRIVATE
    CIRCTFirtool CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-coerce-reset COMMAND goldengate-coerce-reset-test
    ${CMAKE_CURRENT_BINARY_DIR}/coerce-reset.sv)
  add_executable(goldengate-fased-header-test
    test/Support/FASEDHeaderTest.cpp
    lib/Support/FASEDHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp
    lib/Transforms/FASEDMMIOBank.cpp lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-fased-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-header COMMAND goldengate-fased-header-test)
  add_executable(goldengate-cpu-managed-stream-header-test
    test/Support/CPUManagedStreamHeaderTest.cpp
    lib/Support/CPUManagedStreamHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp
    lib/Transforms/TracerVStreamQueue.cpp lib/Transforms/CPUStreamRead.cpp
    lib/Transforms/CPUStreamCountBank.cpp lib/Transforms/EmptyCPUStreamWrite.cpp)
  target_compile_features(goldengate-cpu-managed-stream-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-managed-stream-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-managed-stream-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-managed-stream-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-managed-stream-header COMMAND goldengate-cpu-managed-stream-header-test)
  add_executable(goldengate-tracerv-header-test
    test/Support/TracerVHeaderTest.cpp
    lib/Support/TracerVHeader.cpp lib/Support/BlockDevHeader.cpp
    lib/Support/TSIHeader.cpp lib/Support/UARTHeader.cpp
    lib/Support/PeekPokeHeader.cpp lib/Support/LoadMemHeader.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp lib/Support/TargetUtils.cpp
    lib/Transforms/TracerVTokenEngine.cpp lib/Transforms/TracerVTriggerConfig.cpp
    lib/Transforms/TracerVStreamQueue.cpp lib/Transforms/CPUStreamRead.cpp)
  target_compile_features(goldengate-tracerv-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tracerv-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tracerv-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-tracerv-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tracerv-header COMMAND goldengate-tracerv-header-test)
  add_executable(goldengate-blockdev-header-test
    test/Support/BlockDevHeaderTest.cpp
    lib/Support/BlockDevHeader.cpp lib/Support/TSIHeader.cpp lib/Support/UARTHeader.cpp lib/Support/PeekPokeHeader.cpp lib/Support/LoadMemHeader.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp lib/Support/TargetUtils.cpp
    lib/Transforms/BlockDevMMIOBank.cpp)
  target_compile_features(goldengate-blockdev-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-header COMMAND goldengate-blockdev-header-test)
  add_executable(goldengate-tsi-header-test
    test/Support/TSIHeaderTest.cpp
    lib/Support/TSIHeader.cpp lib/Support/UARTHeader.cpp lib/Support/PeekPokeHeader.cpp lib/Support/LoadMemHeader.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp lib/Support/TargetUtils.cpp
    lib/Transforms/TSITokenEngine.cpp lib/Transforms/TSIWordQueues.cpp lib/Transforms/TSIMMIOBank.cpp)
  target_compile_features(goldengate-tsi-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tsi-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tsi-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-tsi-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tsi-header COMMAND goldengate-tsi-header-test)
  add_executable(goldengate-uart-header-test
    test/Support/UARTHeaderTest.cpp
    lib/Support/UARTHeader.cpp lib/Support/PeekPokeHeader.cpp lib/Support/LoadMemHeader.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp lib/Support/TargetUtils.cpp
    lib/Transforms/UARTSerialEngine.cpp lib/Transforms/UARTByteQueues.cpp lib/Transforms/UARTMMIOBank.cpp)
  target_compile_features(goldengate-uart-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-uart-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-uart-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-uart-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-uart-header COMMAND goldengate-uart-header-test)
  add_executable(goldengate-peek-poke-header-test
    test/Support/PeekPokeHeaderTest.cpp
    lib/Support/PeekPokeHeader.cpp lib/Support/LoadMemHeader.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Support/AnnotationEmission.cpp lib/Support/TargetUtils.cpp
    lib/Transforms/PeekPokeCycleEngine.cpp lib/Transforms/PeekPokeMMIOBank.cpp)
  target_compile_features(goldengate-peek-poke-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-peek-poke-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-peek-poke-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-peek-poke-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-peek-poke-header COMMAND goldengate-peek-poke-header-test)
  add_executable(goldengate-loadmem-header-test
    test/Support/LoadMemHeaderTest.cpp
    lib/Support/LoadMemHeader.cpp lib/Support/ResetPulseHeader.cpp
    lib/Support/ClockBridgeHeader.cpp lib/Support/SimulationMasterHeader.cpp
    lib/Support/MetasimInterfaceHeader.cpp lib/Support/AnnotationEmission.cpp
    lib/Transforms/LoadMemWriter.cpp lib/Transforms/LoadMemRequests.cpp
    lib/Transforms/LoadMemWriteMMIO.cpp lib/Transforms/LoadMemWriteData.cpp
    lib/Transforms/LoadMemReadRequests.cpp lib/Transforms/LoadMemReadData.cpp)
  target_compile_features(goldengate-loadmem-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-header COMMAND goldengate-loadmem-header-test)
  add_executable(goldengate-reset-pulse-header-test
    test/Support/ResetPulseHeaderTest.cpp
    lib/Support/ResetPulseHeader.cpp lib/Support/ClockBridgeHeader.cpp lib/Support/SimulationMasterHeader.cpp
    lib/Support/MetasimInterfaceHeader.cpp lib/Transforms/ResetPulseBridge.cpp
    lib/Support/TargetUtils.cpp lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-reset-pulse-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-reset-pulse-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-reset-pulse-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-reset-pulse-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-reset-pulse-header COMMAND goldengate-reset-pulse-header-test)
  add_executable(goldengate-clock-bridge-header-test
    test/Support/ClockBridgeHeaderTest.cpp
    lib/Support/ClockBridgeHeader.cpp lib/Support/SimulationMasterHeader.cpp
    lib/Support/MetasimInterfaceHeader.cpp lib/Transforms/SingleClockBridge.cpp
    lib/Support/TargetUtils.cpp lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-clock-bridge-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-clock-bridge-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-clock-bridge-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-clock-bridge-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-clock-bridge-header COMMAND goldengate-clock-bridge-header-test)
  add_executable(goldengate-simulation-master-header-test
    test/Support/SimulationMasterHeaderTest.cpp
    lib/Support/SimulationMasterHeader.cpp lib/Support/MetasimInterfaceHeader.cpp
    lib/Transforms/SimulationMaster.cpp lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-simulation-master-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-simulation-master-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-simulation-master-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-simulation-master-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-simulation-master-header COMMAND goldengate-simulation-master-header-test)
  add_executable(goldengate-metasim-interface-header-test
    test/Support/MetasimInterfaceHeaderTest.cpp
    lib/Support/MetasimInterfaceHeader.cpp lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-metasim-interface-header-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-metasim-interface-header-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-metasim-interface-header-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-metasim-interface-header-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-metasim-interface-header COMMAND goldengate-metasim-interface-header-test)
  add_executable(goldengate-simulator-rtl-test
    test/Support/SimulatorRTLTest.cpp lib/Support/SimulatorRTL.cpp
    lib/Transforms/LowerTypes.cpp lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-simulator-rtl-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-simulator-rtl-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-simulator-rtl-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-simulator-rtl-test PRIVATE
    CIRCTFirtool CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_executable(goldengate-xilinx-host-specialization-test
    test/Transforms/XilinxHostSpecializationTest.cpp
    lib/Transforms/XilinxHostSpecialization.cpp
    lib/Support/XDCEmission.cpp lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-xilinx-host-specialization-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-xilinx-host-specialization-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-xilinx-host-specialization-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-xilinx-host-specialization-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-xilinx-host-specialization COMMAND goldengate-xilinx-host-specialization-test)
  add_executable(goldengate-xdc-emission-test
    test/Support/XDCEmissionTest.cpp lib/Support/XDCEmission.cpp
    lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-xdc-emission-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-xdc-emission-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-xdc-emission-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-xdc-emission-test PRIVATE
    CIRCTFIRRTL CIRCTHW CIRCTSupport MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-xdc-emission COMMAND goldengate-xdc-emission-test)
  add_executable(goldengate-fased-mmio-bank-test
    test/Transforms/FASEDMMIOBankTest.cpp lib/Transforms/FASEDMMIOBank.cpp)
  target_compile_features(goldengate-fased-mmio-bank-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-mmio-bank-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-mmio-bank-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-mmio-bank-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-mmio-bank COMMAND goldengate-fased-mmio-bank-test)
  add_executable(goldengate-fased-histograms-test
    test/Transforms/FASEDHistogramsTest.cpp lib/Transforms/FASEDHistograms.cpp)
  target_compile_features(goldengate-fased-histograms-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-histograms-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-histograms-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-histograms-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-histograms COMMAND goldengate-fased-histograms-test)
  add_executable(goldengate-fased-statistics-test
    test/Transforms/FASEDStatisticsTest.cpp lib/Transforms/FASEDStatistics.cpp)
  target_compile_features(goldengate-fased-statistics-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-statistics-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-statistics-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-statistics-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-statistics COMMAND goldengate-fased-statistics-test)
  add_executable(goldengate-fased-response-errors-test
    test/Transforms/FASEDResponseErrorsTest.cpp lib/Transforms/FASEDResponseErrors.cpp)
  target_compile_features(goldengate-fased-response-errors-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-response-errors-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-response-errors-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-response-errors-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-response-errors COMMAND goldengate-fased-response-errors-test)
  add_executable(goldengate-fased-functional-model-register-test
    test/Transforms/FASEDFunctionalModelRegisterTest.cpp lib/Transforms/FASEDFunctionalModelRegister.cpp)
  target_compile_features(goldengate-fased-functional-model-register-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-functional-model-register-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-functional-model-register-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-functional-model-register-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-functional-model-register COMMAND goldengate-fased-functional-model-register-test)
  add_executable(goldengate-fased-latency-registers-test
    test/Transforms/FASEDLatencyRegistersTest.cpp lib/Transforms/FASEDLatencyRegisters.cpp)
  target_compile_features(goldengate-fased-latency-registers-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-latency-registers-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-latency-registers-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-latency-registers-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-latency-registers COMMAND goldengate-fased-latency-registers-test)
  add_executable(goldengate-fased-request-limits-test
    test/Transforms/FASEDRequestLimitsTest.cpp lib/Transforms/FASEDRequestLimits.cpp)
  target_compile_features(goldengate-fased-request-limits-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-request-limits-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-request-limits-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-request-limits-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-request-limits COMMAND goldengate-fased-request-limits-test)
  add_executable(goldengate-fased-read-admission-test
    test/Transforms/FASEDReadAdmissionTest.cpp lib/Transforms/FASEDReadAdmission.cpp)
  target_compile_features(goldengate-fased-read-admission-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-read-admission-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-read-admission-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-read-admission-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-read-admission COMMAND goldengate-fased-read-admission-test)
  add_executable(goldengate-fased-write-admission-test
    test/Transforms/FASEDWriteAdmissionTest.cpp lib/Transforms/FASEDWriteAdmission.cpp)
  target_compile_features(goldengate-fased-write-admission-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-write-admission-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-write-admission-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-write-admission-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-write-admission COMMAND goldengate-fased-write-admission-test)
  add_executable(goldengate-fased-write-retirement-test
    test/Transforms/FASEDWriteRetirementTest.cpp lib/Transforms/FASEDWriteRetirement.cpp)
  target_compile_features(goldengate-fased-write-retirement-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-write-retirement-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-write-retirement-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-write-retirement-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-write-retirement COMMAND goldengate-fased-write-retirement-test)
  add_executable(goldengate-fased-write-pairing-test
    test/Transforms/FASEDWritePairingTest.cpp lib/Transforms/FASEDWritePairing.cpp)
  target_compile_features(goldengate-fased-write-pairing-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-write-pairing-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-write-pairing-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-write-pairing-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-write-pairing COMMAND goldengate-fased-write-pairing-test)
  add_executable(goldengate-fased-timing-aw-queue-test
    test/Transforms/FASEDTimingAWQueueTest.cpp lib/Transforms/FASEDTimingAWQueue.cpp)
  target_compile_features(goldengate-fased-timing-aw-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-timing-aw-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-timing-aw-queue-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-timing-aw-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-timing-aw-queue COMMAND goldengate-fased-timing-aw-queue-test)
  add_executable(goldengate-fased-write-latency-test
    test/Transforms/FASEDWriteLatencyTest.cpp lib/Transforms/FASEDWriteLatency.cpp)
  target_compile_features(goldengate-fased-write-latency-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-write-latency-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-write-latency-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-write-latency-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-write-latency COMMAND goldengate-fased-write-latency-test)
  add_executable(goldengate-fased-read-latency-test
    test/Transforms/FASEDReadLatencyTest.cpp lib/Transforms/FASEDReadLatency.cpp)
  target_compile_features(goldengate-fased-read-latency-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-read-latency-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-read-latency-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-read-latency-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-read-latency COMMAND goldengate-fased-read-latency-test)
  add_executable(goldengate-fased-timing-cycle-test
    test/Transforms/FASEDTimingCycleTest.cpp lib/Transforms/FASEDTimingCycle.cpp)
  target_compile_features(goldengate-fased-timing-cycle-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-timing-cycle-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-timing-cycle-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-timing-cycle-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-timing-cycle COMMAND goldengate-fased-timing-cycle-test)
  add_executable(goldengate-fased-response-releaser-test
    test/Transforms/FASEDResponseReleaserTest.cpp lib/Transforms/FASEDResponseReleaser.cpp)
  target_compile_features(goldengate-fased-response-releaser-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-response-releaser-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-response-releaser-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-response-releaser-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-response-releaser COMMAND goldengate-fased-response-releaser-test)
  add_executable(goldengate-fased-write-egress-test
    test/Transforms/FASEDWriteEgressTest.cpp lib/Transforms/FASEDWriteEgress.cpp)
  target_compile_features(goldengate-fased-write-egress-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-write-egress-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-write-egress-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-write-egress-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-write-egress COMMAND goldengate-fased-write-egress-test)
  add_executable(goldengate-fased-read-scheduler-test
    test/Transforms/FASEDReadSchedulerTest.cpp lib/Transforms/FASEDReadScheduler.cpp)
  target_compile_features(goldengate-fased-read-scheduler-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-read-scheduler-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-read-scheduler-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-read-scheduler-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-read-scheduler COMMAND goldengate-fased-read-scheduler-test)
  add_executable(goldengate-fased-read-buffer-test
    test/Transforms/FASEDReadBufferTest.cpp lib/Transforms/FASEDReadBuffer.cpp)
  target_compile_features(goldengate-fased-read-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-read-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-read-buffer-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-read-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-read-buffer COMMAND goldengate-fased-read-buffer-test)
  add_executable(goldengate-fased-ingress-issue-test
    test/Transforms/FASEDIngressIssueTest.cpp lib/Transforms/FASEDIngressIssue.cpp)
  target_compile_features(goldengate-fased-ingress-issue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-issue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-issue-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-issue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-issue COMMAND goldengate-fased-ingress-issue-test)
  add_executable(goldengate-fased-ingress-deadlock-test
    test/Transforms/FASEDIngressDeadlockTest.cpp lib/Transforms/FASEDIngressDeadlock.cpp)
  target_compile_features(goldengate-fased-ingress-deadlock-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-deadlock-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-deadlock-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-deadlock-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-deadlock COMMAND goldengate-fased-ingress-deadlock-test)
  add_executable(goldengate-fased-ingress-order-test
    test/Transforms/FASEDIngressOrderTest.cpp lib/Transforms/FASEDIngressOrder.cpp)
  target_compile_features(goldengate-fased-ingress-order-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-order-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-order-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-order-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-order COMMAND goldengate-fased-ingress-order-test)
  add_executable(goldengate-fased-ingress-credits-test
    test/Transforms/FASEDIngressCreditsTest.cpp lib/Transforms/FASEDIngressCredits.cpp)
  target_compile_features(goldengate-fased-ingress-credits-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-credits-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-credits-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-credits-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-credits COMMAND goldengate-fased-ingress-credits-test)
  add_executable(goldengate-fased-ingress-ar-test
    test/Transforms/FASEDIngressARQueueTest.cpp lib/Transforms/FASEDIngressARQueue.cpp)
  target_compile_features(goldengate-fased-ingress-ar-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-ar-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-ar-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-ar-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-ar COMMAND goldengate-fased-ingress-ar-test)
  add_executable(goldengate-fased-ingress-w-test
    test/Transforms/FASEDIngressWQueueTest.cpp lib/Transforms/FASEDIngressWQueue.cpp)
  target_compile_features(goldengate-fased-ingress-w-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-w-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-w-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-w-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-w COMMAND goldengate-fased-ingress-w-test)
  add_executable(goldengate-fased-ingress-aw-test
    test/Transforms/FASEDIngressAWQueueTest.cpp lib/Transforms/FASEDIngressAWQueue.cpp)
  target_compile_features(goldengate-fased-ingress-aw-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-ingress-aw-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-ingress-aw-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-ingress-aw-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-ingress-aw COMMAND goldengate-fased-ingress-aw-test)
  add_executable(goldengate-fased-host-outstanding-test
    test/Transforms/FASEDHostOutstandingTest.cpp lib/Transforms/FASEDHostOutstanding.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-fased-host-outstanding-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-host-outstanding-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-host-outstanding-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-host-outstanding-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-host-outstanding COMMAND goldengate-fased-host-outstanding-test)
  add_executable(goldengate-fased-token-test
    test/Transforms/FASEDTokenEngineTest.cpp lib/Transforms/FASEDTokenEngine.cpp lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-fased-token-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-token-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-token-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-token-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-token COMMAND goldengate-fased-token-test)
  add_executable(goldengate-blockdev-response-scheduler-test
    test/Transforms/BlockDevResponseSchedulerTest.cpp lib/Transforms/BlockDevResponseScheduler.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-blockdev-response-scheduler-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-response-scheduler-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-response-scheduler-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-response-scheduler-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-response-scheduler COMMAND goldengate-blockdev-response-scheduler-test)
  add_executable(goldengate-blockdev-read-latency-test
    test/Transforms/BlockDevReadLatencyTest.cpp lib/Transforms/BlockDevReadLatency.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-blockdev-read-latency-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-read-latency-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-read-latency-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-read-latency-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-read-latency COMMAND goldengate-blockdev-read-latency-test)
  add_executable(goldengate-blockdev-write-latency-test
    test/Transforms/BlockDevWriteLatencyTest.cpp lib/Transforms/BlockDevWriteLatency.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-blockdev-write-latency-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-write-latency-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-write-latency-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-write-latency-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-write-latency COMMAND goldengate-blockdev-write-latency-test)
  add_executable(goldengate-blockdev-mmio-bank-test
    test/Transforms/BlockDevMMIOBankTest.cpp
    lib/Transforms/BlockDevMMIOBank.cpp)
  target_compile_features(goldengate-blockdev-mmio-bank-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-mmio-bank-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-mmio-bank-test PRIVATE
    include ${CIRCT_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-mmio-bank-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-mmio-bank COMMAND goldengate-blockdev-mmio-bank-test)
  add_executable(goldengate-blockdev-write-ack-queue-test
    test/Transforms/BlockDevWriteAckQueueTest.cpp
    lib/Transforms/BlockDevWriteAckQueue.cpp)
  target_compile_features(goldengate-blockdev-write-ack-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-write-ack-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-write-ack-queue-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-write-ack-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-write-ack-queue COMMAND goldengate-blockdev-write-ack-queue-test)
  add_executable(goldengate-blockdev-read-response-queue-test
    test/Transforms/BlockDevReadResponseQueueTest.cpp
    lib/Transforms/BlockDevReadResponseQueue.cpp lib/Transforms/BlockDevDataQueue.cpp)
  target_compile_features(goldengate-blockdev-read-response-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-read-response-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-read-response-queue-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-read-response-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-read-response-queue COMMAND goldengate-blockdev-read-response-queue-test)
  add_executable(goldengate-blockdev-data-queue-test
    test/Transforms/BlockDevDataQueueTest.cpp lib/Transforms/BlockDevDataQueue.cpp)
  target_compile_features(goldengate-blockdev-data-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-data-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-data-queue-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-data-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-data-queue COMMAND goldengate-blockdev-data-queue-test)
  add_executable(goldengate-blockdev-request-queue-test
    test/Transforms/BlockDevRequestQueueTest.cpp lib/Transforms/BlockDevRequestQueue.cpp)
  target_compile_features(goldengate-blockdev-request-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-request-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-request-queue-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-request-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-request-queue COMMAND goldengate-blockdev-request-queue-test)
  add_executable(goldengate-blockdev-token-test
    test/Transforms/BlockDevTokenEngineTest.cpp lib/Transforms/BlockDevTokenEngine.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-blockdev-token-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-token-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-token-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-token-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-token COMMAND goldengate-blockdev-token-test)
  add_executable(goldengate-tsi-mmio-test
    test/Transforms/TSIMMIOBankTest.cpp lib/Transforms/TSIMMIOBank.cpp)
  target_compile_features(goldengate-tsi-mmio-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tsi-mmio-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tsi-mmio-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-tsi-mmio-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tsi-mmio COMMAND goldengate-tsi-mmio-test)
  add_executable(goldengate-tsi-word-queues-test
    test/Transforms/TSIWordQueuesTest.cpp lib/Transforms/TSIWordQueues.cpp)
  target_compile_features(goldengate-tsi-word-queues-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tsi-word-queues-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tsi-word-queues-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-tsi-word-queues-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tsi-word-queues COMMAND goldengate-tsi-word-queues-test)
  add_executable(goldengate-tsi-token-test
    test/Transforms/TSITokenEngineTest.cpp lib/Transforms/TSITokenEngine.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-tsi-token-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tsi-token-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tsi-token-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-tsi-token-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tsi-token COMMAND goldengate-tsi-token-test)
  add_executable(goldengate-simulation-master-control-test
    test/Transforms/SimulationMasterControlTest.cpp
    lib/Transforms/SimulationMasterControl.cpp lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-simulation-master-control-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-simulation-master-control-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-simulation-master-control-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-simulation-master-control-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-simulation-master-control COMMAND goldengate-simulation-master-control-test)
  add_executable(goldengate-control-master-test
    test/Transforms/ControlMasterTest.cpp lib/Transforms/SimulationMasterControl.cpp)
  target_compile_features(goldengate-control-master-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-master-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-master-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-master-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-master COMMAND goldengate-control-master-test)
  add_executable(goldengate-fased-host-memory-test
    test/Transforms/FASEDHostMemoryTest.cpp lib/Transforms/FASEDHostMemory.cpp)
  target_compile_features(goldengate-fased-host-memory-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-host-memory-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-host-memory-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-host-memory-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-host-memory COMMAND goldengate-fased-host-memory-test)
  add_executable(goldengate-host-memory-read-responses-test
    test/Transforms/HostMemoryReadResponsesTest.cpp lib/Transforms/HostMemoryReadResponses.cpp
    lib/Transforms/HostMemoryReadArbiter.cpp lib/Transforms/HostMemoryWriteResponses.cpp
    lib/Transforms/HostMemoryWriteArbiter.cpp)
  target_compile_features(goldengate-host-memory-read-responses-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-read-responses-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-read-responses-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-read-responses-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-read-responses COMMAND goldengate-host-memory-read-responses-test)
  add_executable(goldengate-host-memory-read-arbiter-test
    test/Transforms/HostMemoryReadArbiterTest.cpp lib/Transforms/HostMemoryReadArbiter.cpp
    lib/Transforms/HostMemoryWriteResponses.cpp lib/Transforms/HostMemoryWriteArbiter.cpp)
  target_compile_features(goldengate-host-memory-read-arbiter-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-read-arbiter-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-read-arbiter-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-read-arbiter-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-read-arbiter COMMAND goldengate-host-memory-read-arbiter-test)
  add_executable(goldengate-host-memory-write-responses-test
    test/Transforms/HostMemoryWriteResponsesTest.cpp lib/Transforms/HostMemoryWriteResponses.cpp
    lib/Transforms/HostMemoryWriteArbiter.cpp)
  target_compile_features(goldengate-host-memory-write-responses-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-write-responses-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-write-responses-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-write-responses-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-write-responses COMMAND goldengate-host-memory-write-responses-test)
  add_executable(goldengate-host-memory-write-arbiter-test
    test/Transforms/HostMemoryWriteArbiterTest.cpp lib/Transforms/HostMemoryWriteArbiter.cpp)
  target_compile_features(goldengate-host-memory-write-arbiter-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-write-arbiter-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-write-arbiter-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-write-arbiter-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-write-arbiter COMMAND goldengate-host-memory-write-arbiter-test)
  add_executable(goldengate-cpu-stream-port-test
    test/Transforms/CPUStreamPortTest.cpp lib/Transforms/CPUStreamPort.cpp)
  target_compile_features(goldengate-cpu-stream-port-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-port-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-port-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-port-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-port COMMAND goldengate-cpu-stream-port-test)
  add_executable(goldengate-fpga-top-shell-test
    test/Transforms/FPGATopShellTest.cpp lib/Transforms/FPGATopShell.cpp)
  target_compile_features(goldengate-fpga-top-shell-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fpga-top-shell-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fpga-top-shell-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fpga-top-shell-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fpga-top-shell COMMAND goldengate-fpga-top-shell-test)
  add_executable(goldengate-f1-shim-test
    test/Transforms/F1ShimTest.cpp lib/Transforms/F1Shim.cpp lib/Transforms/FPGATopShell.cpp
    lib/Support/AnnotationEmission.cpp)
  target_compile_features(goldengate-f1-shim-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-f1-shim-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-f1-shim-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-f1-shim-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-f1-shim COMMAND goldengate-f1-shim-test)
  add_executable(goldengate-host-memory-port-test
    test/Transforms/HostMemoryPortTest.cpp lib/Transforms/HostMemoryPort.cpp
    lib/Transforms/HostMemoryOutputBuffer.cpp)
  target_compile_features(goldengate-host-memory-port-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-port-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-port-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-port-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-port COMMAND goldengate-host-memory-port-test)
  add_executable(goldengate-host-memory-output-buffer-test
    test/Transforms/HostMemoryOutputBufferTest.cpp lib/Transforms/HostMemoryOutputBuffer.cpp)
  target_compile_features(goldengate-host-memory-output-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-host-memory-output-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-host-memory-output-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-host-memory-output-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-host-memory-output-buffer COMMAND goldengate-host-memory-output-buffer-test)
  add_executable(goldengate-fased-host-memory-buffer-test
    test/Transforms/FASEDHostMemoryBufferTest.cpp lib/Transforms/FASEDHostMemoryBuffer.cpp
    lib/Transforms/FASEDReadDeinterleaver.cpp lib/Transforms/FASEDAddressTranslation.cpp)
  target_compile_features(goldengate-fased-host-memory-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-host-memory-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-host-memory-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-host-memory-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-host-memory-buffer COMMAND goldengate-fased-host-memory-buffer-test)
  add_executable(goldengate-fased-read-deinterleaver-test
    test/Transforms/FASEDReadDeinterleaverTest.cpp lib/Transforms/FASEDReadDeinterleaver.cpp
    lib/Transforms/FASEDAddressTranslation.cpp)
  target_compile_features(goldengate-fased-read-deinterleaver-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-read-deinterleaver-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-read-deinterleaver-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-read-deinterleaver-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-read-deinterleaver COMMAND goldengate-fased-read-deinterleaver-test)
  add_executable(goldengate-fased-address-translation-test
    test/Transforms/FASEDAddressTranslationTest.cpp lib/Transforms/FASEDAddressTranslation.cpp)
  target_compile_features(goldengate-fased-address-translation-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-address-translation-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-address-translation-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-address-translation-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-address-translation COMMAND goldengate-fased-address-translation-test)
  add_executable(goldengate-simulation-master-test
    test/Transforms/SimulationMasterTest.cpp lib/Transforms/SimulationMaster.cpp)
  target_compile_features(goldengate-simulation-master-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-simulation-master-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-simulation-master-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-simulation-master-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-simulation-master COMMAND goldengate-simulation-master-test)
  add_executable(goldengate-control-write-tracker-test
    test/Transforms/ControlWriteTrackerTest.cpp
    lib/Transforms/ControlWriteTracker.cpp
    lib/Transforms/ControlTransactionTracker.cpp)
  target_compile_features(goldengate-control-write-tracker-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-write-tracker-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-write-tracker-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-write-tracker-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-write-tracker COMMAND goldengate-control-write-tracker-test)
  add_executable(goldengate-control-write-arbiter-test
    test/Transforms/ControlWriteArbiterTest.cpp
    lib/Transforms/ControlWriteArbiter.cpp)
  target_compile_features(goldengate-control-write-arbiter-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-write-arbiter-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-write-arbiter-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-write-arbiter-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-write-arbiter COMMAND goldengate-control-write-arbiter-test)
  add_executable(goldengate-control-read-arbiter-test
    test/Transforms/ControlReadArbiterTest.cpp
    lib/Transforms/ControlReadArbiter.cpp)
  target_compile_features(goldengate-control-read-arbiter-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-read-arbiter-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-read-arbiter-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-read-arbiter-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-read-arbiter COMMAND goldengate-control-read-arbiter-test)
  add_executable(goldengate-control-read-tracker-test
    test/Transforms/ControlReadTrackerTest.cpp
    lib/Transforms/ControlReadTracker.cpp
    lib/Transforms/ControlTransactionTracker.cpp)
  target_compile_features(goldengate-control-read-tracker-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-read-tracker-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-read-tracker-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-read-tracker-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-read-tracker COMMAND goldengate-control-read-tracker-test)
  add_executable(goldengate-control-read-dispatch-test
    test/Transforms/ControlReadDispatchTest.cpp
    lib/Transforms/ControlReadDispatch.cpp)
  target_compile_features(goldengate-control-read-dispatch-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-read-dispatch-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-read-dispatch-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-read-dispatch-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-read-dispatch COMMAND goldengate-control-read-dispatch-test)
  add_executable(goldengate-control-widget-writes-test
    test/Transforms/ControlWidgetWritesTest.cpp
    lib/Transforms/ControlWidgetWrites.cpp)
  target_compile_features(goldengate-control-widget-writes-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-widget-writes-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-widget-writes-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-widget-writes-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-widget-writes COMMAND goldengate-control-widget-writes-test)
  add_executable(goldengate-pipe-channel-test
    test/Transforms/FAMEPipeChannelTest.cpp
    lib/Transforms/FAMEPipeChannel.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-pipe-channel-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-pipe-channel-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-pipe-channel-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-pipe-channel-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-pipe-channel COMMAND goldengate-pipe-channel-test)
  add_executable(goldengate-ready-valid-channel-test
    test/Transforms/FAMEReadyValidChannelTest.cpp
    lib/Transforms/FAMEReadyValidChannel.cpp
    lib/Transforms/FAMEPipeChannel.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-ready-valid-channel-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-ready-valid-channel-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-ready-valid-channel-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-ready-valid-channel-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-ready-valid-channel COMMAND goldengate-ready-valid-channel-test)
  add_executable(goldengate-clock-channel-test
    test/Transforms/FAMEClockChannelTest.cpp
    lib/Transforms/FAMEClockChannel.cpp
    lib/Transforms/FAMEPipeChannel.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-clock-channel-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-clock-channel-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-clock-channel-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-clock-channel-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-clock-channel COMMAND goldengate-clock-channel-test)
  add_executable(goldengate-single-clock-bridge-test
    test/Transforms/SingleClockBridgeTest.cpp
    lib/Transforms/SingleClockBridge.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-single-clock-bridge-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-single-clock-bridge-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-single-clock-bridge-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-single-clock-bridge-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-single-clock-bridge COMMAND goldengate-single-clock-bridge-test)
  add_executable(goldengate-clock-bridge-control-test
    test/Transforms/ClockBridgeControlTest.cpp
    lib/Transforms/ClockBridgeControl.cpp
    lib/Transforms/CPUStreamCountBank.cpp)
  target_compile_features(goldengate-clock-bridge-control-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-clock-bridge-control-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-clock-bridge-control-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-clock-bridge-control-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-clock-bridge-control COMMAND goldengate-clock-bridge-control-test)
  add_executable(goldengate-blockdev-control-test
    test/Transforms/BlockDevControlTest.cpp
    lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-blockdev-control-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-blockdev-control-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-blockdev-control-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-blockdev-control-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-blockdev-control COMMAND goldengate-blockdev-control-test)
  add_executable(goldengate-fased-control-test
    test/Transforms/FASEDControlTest.cpp
    lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-fased-control-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-fased-control-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-fased-control-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-fased-control-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-fased-control COMMAND goldengate-fased-control-test)
  add_executable(goldengate-reset-pulse-bridge-test
    test/Transforms/ResetPulseBridgeTest.cpp
    lib/Transforms/ResetPulseBridge.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-reset-pulse-bridge-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-reset-pulse-bridge-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-reset-pulse-bridge-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-reset-pulse-bridge-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-reset-pulse-bridge COMMAND goldengate-reset-pulse-bridge-test)
  add_executable(goldengate-uart-serial-test
    test/Transforms/UARTSerialEngineTest.cpp
    lib/Transforms/UARTSerialEngine.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-uart-serial-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-uart-serial-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-uart-serial-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-uart-serial-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-uart-serial COMMAND goldengate-uart-serial-test)
  add_executable(goldengate-uart-byte-queues-test
    test/Transforms/UARTByteQueuesTest.cpp
    lib/Transforms/UARTByteQueues.cpp)
  target_compile_features(goldengate-uart-byte-queues-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-uart-byte-queues-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-uart-byte-queues-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-uart-byte-queues-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-uart-byte-queues COMMAND goldengate-uart-byte-queues-test)
  add_executable(goldengate-uart-mmio-test
    test/Transforms/UARTMMIOBankTest.cpp
    lib/Transforms/UARTMMIOBank.cpp
    lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-uart-mmio-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-uart-mmio-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-uart-mmio-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-uart-mmio-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-uart-mmio COMMAND goldengate-uart-mmio-test)
  add_executable(goldengate-peek-poke-cycle-test
    test/Transforms/PeekPokeCycleEngineTest.cpp
    lib/Transforms/PeekPokeCycleEngine.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-peek-poke-cycle-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-peek-poke-cycle-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-peek-poke-cycle-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-peek-poke-cycle-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-peek-poke-cycle COMMAND goldengate-peek-poke-cycle-test)
  add_executable(goldengate-peek-poke-mmio-test
    test/Transforms/PeekPokeMMIOBankTest.cpp
    lib/Transforms/PeekPokeMMIOBank.cpp
    lib/Transforms/ClockBridgeControl.cpp)
  target_compile_features(goldengate-peek-poke-mmio-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-peek-poke-mmio-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-peek-poke-mmio-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-peek-poke-mmio-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-peek-poke-mmio COMMAND goldengate-peek-poke-mmio-test)
  add_executable(goldengate-tracerv-token-test
    test/Transforms/TracerVTokenEngineTest.cpp
    lib/Transforms/TracerVTokenEngine.cpp
    lib/Transforms/TracerVTriggerConfig.cpp
    lib/Support/TargetUtils.cpp)
  target_compile_features(goldengate-tracerv-token-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tracerv-token-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tracerv-token-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-tracerv-token-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tracerv-token COMMAND goldengate-tracerv-token-test)
  add_executable(goldengate-tracerv-stream-queue-test
    test/Transforms/TracerVStreamQueueTest.cpp
    lib/Transforms/TracerVStreamQueue.cpp)
  target_compile_features(goldengate-tracerv-stream-queue-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-tracerv-stream-queue-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-tracerv-stream-queue-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-tracerv-stream-queue-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-tracerv-stream-queue COMMAND goldengate-tracerv-stream-queue-test)
  add_executable(goldengate-cpu-stream-read-test
    test/Transforms/CPUStreamReadTest.cpp
    lib/Transforms/CPUStreamRead.cpp)
  target_compile_features(goldengate-cpu-stream-read-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-read-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-read-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-read-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-read COMMAND goldengate-cpu-stream-read-test)
  add_executable(goldengate-empty-cpu-stream-write-test
    test/Transforms/EmptyCPUStreamWriteTest.cpp
    lib/Transforms/EmptyCPUStreamWrite.cpp)
  target_compile_features(goldengate-empty-cpu-stream-write-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-empty-cpu-stream-write-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-empty-cpu-stream-write-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-empty-cpu-stream-write-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-empty-cpu-stream-write COMMAND goldengate-empty-cpu-stream-write-test)
  add_executable(goldengate-cpu-stream-write-buffer-test
    test/Transforms/CPUStreamWriteBufferTest.cpp
    lib/Transforms/CPUStreamWriteBuffer.cpp)
  target_compile_features(goldengate-cpu-stream-write-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-write-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-write-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-write-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-write-buffer COMMAND goldengate-cpu-stream-write-buffer-test)
  add_executable(goldengate-cpu-stream-read-buffer-test
    test/Transforms/CPUStreamReadBufferTest.cpp
    lib/Transforms/CPUStreamReadBuffer.cpp
    lib/Transforms/CPUStreamWriteBuffer.cpp)
  target_compile_features(goldengate-cpu-stream-read-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-read-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-read-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-read-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-read-buffer COMMAND goldengate-cpu-stream-read-buffer-test)
  add_executable(goldengate-cpu-stream-response-buffer-test
    test/Transforms/CPUStreamResponseBufferTest.cpp
    lib/Transforms/CPUStreamResponseBuffer.cpp)
  target_compile_features(goldengate-cpu-stream-response-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-response-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-response-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-response-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-response-buffer COMMAND goldengate-cpu-stream-response-buffer-test)
  add_executable(goldengate-cpu-stream-write-response-buffer-test
    test/Transforms/CPUStreamWriteResponseBufferTest.cpp
    lib/Transforms/CPUStreamWriteResponseBuffer.cpp)
  target_compile_features(goldengate-cpu-stream-write-response-buffer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-cpu-stream-write-response-buffer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-cpu-stream-write-response-buffer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-cpu-stream-write-response-buffer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-cpu-stream-write-response-buffer COMMAND goldengate-cpu-stream-write-response-buffer-test)
  add_executable(goldengate-loadmem-writer-test
    test/Transforms/LoadMemWriterTest.cpp
    lib/Transforms/LoadMemWriter.cpp)
  target_compile_features(goldengate-loadmem-writer-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-writer-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-writer-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-writer-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-writer COMMAND goldengate-loadmem-writer-test)
  add_executable(goldengate-loadmem-requests-test
    test/Transforms/LoadMemRequestsTest.cpp
    lib/Transforms/LoadMemRequests.cpp)
  target_compile_features(goldengate-loadmem-requests-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-requests-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-requests-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-requests-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-requests COMMAND goldengate-loadmem-requests-test)
  add_executable(goldengate-loadmem-write-mmio-test
    test/Transforms/LoadMemWriteMMIOTest.cpp
    lib/Transforms/LoadMemWriteMMIO.cpp)
  target_compile_features(goldengate-loadmem-write-mmio-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-write-mmio-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-write-mmio-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-write-mmio-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-write-mmio COMMAND goldengate-loadmem-write-mmio-test)
  add_executable(goldengate-loadmem-write-data-test
    test/Transforms/LoadMemWriteDataTest.cpp
    lib/Transforms/LoadMemWriteData.cpp)
  target_compile_features(goldengate-loadmem-write-data-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-write-data-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-write-data-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-write-data-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-write-data COMMAND goldengate-loadmem-write-data-test)
  add_executable(goldengate-loadmem-read-requests-test
    test/Transforms/LoadMemReadRequestsTest.cpp
    lib/Transforms/LoadMemReadRequests.cpp)
  target_compile_features(goldengate-loadmem-read-requests-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-read-requests-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-read-requests-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-read-requests-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-read-requests COMMAND goldengate-loadmem-read-requests-test)
  add_executable(goldengate-loadmem-read-data-test
    test/Transforms/LoadMemReadDataTest.cpp
    lib/Transforms/LoadMemReadData.cpp)
  target_compile_features(goldengate-loadmem-read-data-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-loadmem-read-data-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-loadmem-read-data-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-loadmem-read-data-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-loadmem-read-data COMMAND goldengate-loadmem-read-data-test)
  add_executable(goldengate-control-error-slave-test
    test/Transforms/ControlErrorSlaveTest.cpp
    lib/Transforms/ControlErrorSlave.cpp)
  target_compile_features(goldengate-control-error-slave-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-error-slave-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-error-slave-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-error-slave-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-error-slave COMMAND goldengate-control-error-slave-test)
  add_executable(goldengate-control-address-decode-test
    test/Transforms/ControlAddressDecodeTest.cpp
    lib/Transforms/ControlAddressDecode.cpp)
  target_compile_features(goldengate-control-address-decode-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-address-decode-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-address-decode-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-address-decode-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-address-decode COMMAND goldengate-control-address-decode-test)
  add_executable(goldengate-control-write-route-test
    test/Transforms/ControlWriteRouteTest.cpp
    lib/Transforms/ControlWriteRoute.cpp)
  target_compile_features(goldengate-control-write-route-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-write-route-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-write-route-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-write-route-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-write-route COMMAND goldengate-control-write-route-test)
  add_executable(goldengate-control-write-dispatch-test
    test/Transforms/ControlWriteDispatchTest.cpp
    lib/Transforms/ControlWriteDispatch.cpp)
  target_compile_features(goldengate-control-write-dispatch-test PRIVATE cxx_std_17)
  target_compile_options(goldengate-control-write-dispatch-test PRIVATE -fno-rtti)
  target_include_directories(goldengate-control-write-dispatch-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${MLIR_INCLUDE_DIRS} ${CIRCT_INCLUDE_DIRS})
  target_link_libraries(goldengate-control-write-dispatch-test PRIVATE
    CIRCTFIRRTL CIRCTHW MLIRParser MLIRIR MLIRSupport LLVMSupport)
  add_test(NAME goldengate-control-write-dispatch COMMAND goldengate-control-write-dispatch-test)
endif()
