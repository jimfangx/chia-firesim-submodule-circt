#!/usr/bin/env python3
# See LICENSE for license details.
"""Check enabled AutoCounter/Print in the actual compiler's diagnostic handoff.

This focused supporting case stops at the Print host boundary and makes no
F2/platform, CSV-driver, RTL-equivalence or feature-acceptance claim.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--sfc-oracle", type=Path, help="output from EnabledAutoCounterPrintfOracle")
    parser.add_argument("--platform-config", choices=("BaseF2Config", "BaseXilinxAlveoU250Config"),
                        default="BaseXilinxAlveoU250Config")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    fir = args.work / "input.fir"
    annotations = args.work / "input.json"
    fir.write_text("""FIRRTL version 1.2.0
circuit Profile:
  module Profile:
    input clock : Clock
    input reset : UInt<1>
    input event : UInt<8>
    output observed : UInt<8>
    observed <= event
""")
    counter_class = "midas.InternalAutoCounterFirrtlAnnotation"
    # These are the actual Scala InternalAutoCounterFirrtlAnnotation encodings.
    records = [{
        "class": "midas.passes.fame.FAMEChannelConnectionAnnotation",
        "globalName": "clocks", "sinks": ["~Profile|Profile>clock"],
        "channelInfo": {"class": "midas.passes.fame.TargetClockChannel",
                        "clockInfo": [{"name": "base", "multiplier": 1, "divisor": 1}],
                        "perClockMFMR": [1]},
    }]
    for label, op in (("event_count", "Accumulate"), ("event_value", "Identity")):
        records.append({
            "class": counter_class, "target": "~Profile|Profile>event",
            "clock": "~Profile|Profile>clock", "reset": "~Profile|Profile>reset",
            "label": label, "description": label, "coverGenerated": False,
            "opType": {"class": "midas.targetutils.PerfCounterOps$" + op + "$"},
        })
    annotations.write_text(json.dumps(records, indent=2) + "\n")
    command = [str(args.compiler.resolve()), str(fir.resolve()),
               "--annotation-file", str(annotations.resolve()),
               "--output-dir", str((args.work / "enabled").resolve()),
               "--compile-baseline", "--stop-after-print-host-binding",
               "--enable-autocounter-printf", "--platform-config", args.platform_config]
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (args.work / "stdout.log").write_text(result.stdout)
    (args.work / "stderr.log").write_text(result.stderr)
    output = args.work / "enabled"
    # A minimal profile can stop later at FAME/platform checks; it must reach
    # the real shared PrintSynthesis/TriggerWiring handoff without a debug-only
    # synthesis command. Preserve its precise exit/failure in the evidence.
    debug = output / "post-debug-synthesis.mlir"
    assert result.returncode == 1 and "FAME model has no data channels" in result.stderr, result.stdout + result.stderr
    assert debug.is_file(), result.stdout + result.stderr
    ir = debug.read_text()
    assert "event_count_counter" in ir and "event_value_reg" in ir
    assert len(re.findall(r" = firrtl.regreset ", ir)) == 2, ir
    assert "[AutoCounter] event_count: %d" in ir
    assert "[AutoCounter] event_value: %d" in ir
    retained = json.loads((output / "post-debug-synthesis-all.json").read_text())
    assert all(record.get("class") != counter_class for record in retained)
    bridges = [record for record in retained
               if record.get("class") == "firesim.lib.bridgeutils.BridgeIOAnnotation"
               and record.get("widgetClass") == "midas.widgets.PrintBridgeModule"]
    assert len(bridges) == 1, retained
    assert "Synthesized 2 enabled AutoCounter printf events" in result.stdout, result.stdout
    report = {"scope": "production_autocounter_printf_to_enabled_print_handoff",
              "passed": True, "command": command, "returncode": result.returncode,
              "events": 2, "print_bridges": len(bridges),
              "platform_config": args.platform_config,
              "full_feature_acceptance": False,
              "remaining": ["AutoCounterBridge CSV/MMIO strategy", "F2 platform assembly",
                            "matched enabled SFC representative", "LEC", "SEC", "F2 Linux exercise"]}
    if args.sfc_oracle:
        sfc_ir = (args.sfc_oracle / "post-autocounter.fir").read_text()
        native_ir = (output / "post-autocounter.mlir").read_text()
        sfc_records = json.loads((args.sfc_oracle / "post-autocounter.json").read_text())
        native_records = json.loads((output / "post-autocounter-all.json").read_text())
        # Channel clocks run before native AutoCounter; this no-bridge fixture
        # contributes an empty map. TargetDir is SFC compiler bookkeeping.
        for record in native_records:
            if record.get("class") == "midas.passes.ChannelClockInfoAnnotation":
                assert record["infoMap"] == {}
        canonical = lambda records: sorted(json.dumps(r, sort_keys=True) for r in records
            if r.get("class") not in ("firrtl.options.TargetDirAnnotation",
                                       "midas.passes.ChannelClockInfoAnnotation"))
        assert canonical(sfc_records) == canonical(native_records), "SFC/native annotations differ"
        for label, state, payload in (("event_count", "event_count_counter", "event_count_counter"),
                                      ("event_value", "event_value_reg", "event_value")):
            assert f"reg {state} : UInt<64>, clock" in sfc_ir
            assert re.search(r"%" + state + r" = firrtl.regreset %clock, %reset, %\w+ : .*uint<64>", native_ir)
            assert f'{label} <= mux(reset, UInt<1>("h0"), event)' in sfc_ir
            assert re.search(r"firrtl.printf %clock, %\w+, \"\[AutoCounter\] " + label +
                             r': %d\\0A" .*\(%' + payload + r'\)', native_ir)
            assert re.search(r'printf\(clock, .*"\[AutoCounter\] ' + label +
                             r': %d\\n", ' + payload + r'\)', sfc_ir)
        assert "event_count_next = add(event_count_counter, event_count)" in sfc_ir
        assert "firrtl.add %event_count_counter, %event_count" in native_ir
        assert "neq(event_value, event_value_reg)" in sfc_ir
        assert "firrtl.neq %event_value, %event_value_reg" in native_ir
        report["sfc_supporting_comparison"] = {"passed": True,
            "scope": "same-input counter state, reset gates, printf payload/formats and annotations",
            "normalizations": ["resolved FIRRTL widths", "explicit native add truncation/padding",
                               "empty prior channel-clock map", "SFC TargetDir bookkeeping"]}
    bound_files = [args.compiler.resolve(), fir, annotations,
                   output / "post-autocounter.mlir", output / "post-autocounter-all.json",
                   debug, output / "post-debug-synthesis-all.json"]
    if args.sfc_oracle:
        bound_files += [args.sfc_oracle / "post-autocounter.fir",
                        args.sfc_oracle / "post-autocounter.json"]
    report["sha256"] = {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in bound_files}
    (args.work / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
