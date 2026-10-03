// See LICENSE for license details.
#pragma once

#include "llvm/ADT/StringRef.h"

namespace goldengate {
struct AnnotationClasses {
  static constexpr llvm::StringLiteral FAMETransform =
      "midas.passes.fame.FAMETransformAnnotation";
  static constexpr llvm::StringLiteral ChannelConnection =
      "midas.passes.fame.FAMEChannelConnectionAnnotation";
  static constexpr llvm::StringLiteral ChannelPorts =
      "midas.passes.fame.FAMEChannelPortsAnnotation";
  static constexpr llvm::StringLiteral ChannelFanout =
      "midas.passes.fame.FAMEChannelFanoutAnnotation";
  static constexpr llvm::StringLiteral HostClock =
      "midas.passes.fame.FAMEHostClock";
  static constexpr llvm::StringLiteral HostReset =
      "midas.passes.fame.FAMEHostReset";
  static constexpr llvm::StringLiteral HostClockSource =
      "midas.passes.HostClockSource";
  static constexpr llvm::StringLiteral OutputFile =
      "midas.stage.GoldenGateOutputFileAnnotation";
  static constexpr llvm::StringLiteral InternalXDC = "midas.InternalXDCAnnotation";
  static constexpr llvm::StringLiteral XDCPaths =
      "midas.targetutils.xdc.XDCPathToCircuitAnnotation";
  static constexpr llvm::StringLiteral XDCOutput = "midas.passes.XDCOutputAnnotation";
  static constexpr llvm::StringLiteral XDCSynthesis =
      "midas.targetutils.xdc.XDCFiles$Synthesis$";
  static constexpr llvm::StringLiteral XDCImplementation =
      "midas.targetutils.xdc.XDCFiles$Implementation$";
  static constexpr llvm::StringLiteral FAMEModel =
      "midas.targetutils.FirrtlFAMEModelAnnotation";
  static constexpr llvm::StringLiteral MemModel =
      "midas.targetutils.FirrtlMemModelAnnotation";
  static constexpr llvm::StringLiteral DontTouch =
      "firrtl.transforms.DontTouchAnnotation";
  static constexpr llvm::StringLiteral AutoCounter =
      "midas.targetutils.AutoCounterFirrtlAnnotation";
  static constexpr llvm::StringLiteral InternalAutoCounter =
      "midas.InternalAutoCounterFirrtlAnnotation";
  static constexpr llvm::StringLiteral AutoCounterCoverModule =
      "midas.targetutils.AutoCounterCoverModuleFirrtlAnnotation";
  static constexpr llvm::StringLiteral AutoCounterAccumulate =
      "midas.targetutils.PerfCounterOps$Accumulate$";
  static constexpr llvm::StringLiteral AutoCounterIdentity =
      "midas.targetutils.PerfCounterOps$Identity$";
  static constexpr llvm::StringLiteral EnableModelMultiThreading =
      "midas.targetutils.FirrtlEnableModelMultiThreadingAnnotation";
  static constexpr llvm::StringLiteral BridgeIO =
      "firesim.lib.bridgeutils.BridgeIOAnnotation";
  static constexpr llvm::StringLiteral Bridge =
      "firesim.lib.bridgeutils.BridgeAnnotation";
  static constexpr llvm::StringLiteral ChannelClockInfo =
      "midas.passes.ChannelClockInfoAnnotation";
  static constexpr llvm::StringLiteral TriggerSource =
      "midas.targetutils.TriggerSourceAnnotation";
  static constexpr llvm::StringLiteral TriggerSink =
      "midas.targetutils.TriggerSinkAnnotation";
  static constexpr llvm::StringLiteral InternalTriggerSource =
      "midas.InternalTriggerSourceAnnotation";
  static constexpr llvm::StringLiteral InternalTriggerSink =
      "midas.InternalTriggerSinkAnnotation";
  static constexpr llvm::StringLiteral PipeChannel =
      "midas.passes.fame.PipeChannel";
  static constexpr llvm::StringLiteral DecoupledForwardChannel =
      "midas.passes.fame.DecoupledForwardChannel";
  static constexpr llvm::StringLiteral DecoupledReverseChannel =
      "midas.passes.fame.DecoupledReverseChannel$";
  static constexpr llvm::StringLiteral TargetClockChannel =
      "midas.passes.fame.TargetClockChannel";
};
} // namespace goldengate
