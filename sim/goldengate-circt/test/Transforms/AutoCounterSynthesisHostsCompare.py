#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual source->CSV-host compiler output with unchanged SFC widgets.

Each source-derived constructor supplies the actual emitted widget and its MMIO
positions. Host-token stimuli are external; no FAME/target-token binding or
global platform composition is claimed by this supporting boundary.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from AutoCounterBridgeCompare import DRIVER


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("sfc", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--firtool", type=Path, required=True)
    parser.add_argument("--verilator", type=Path, required=True)
    args = parser.parse_args()
    native, sfc, out = args.native.resolve(), args.sfc.resolve(), args.evidence.resolve()
    out.mkdir(parents=True, exist_ok=True)
    annotations = json.loads((native / "post-autocounter-csv-all.json").read_text())
    constructors = [a["widgetConstructorKey"] for a in annotations
                    if a["class"] == "firesim.lib.bridgeutils.BridgeIOAnnotation"]
    assert len(constructors) == 2
    mlir = (native / "post-autocounter-csv.mlir").read_text()
    banks = re.findall(r"goldengate\.mmioRegisters = \[(.*?)\]", mlir)
    assert len(banks) == len(constructors)
    commands, receipts, artifacts = [], [], []

    def run(command, log, trace=None):
        command = list(map(str, command))
        commands.append(command)
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with log.open("w") as stream:
            if trace:
                with trace.open("w") as data:
                    subprocess.run(command, stdout=data, stderr=stream, check=True)
            else:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)

    for index, (key, bank) in enumerate(zip(constructors, banks)):
        case = out / f"host-{index}"
        case.mkdir(exist_ok=True)
        rows = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i64, readable = (true|false), writeable = (true|false)\}', bank)
        events = key["eventMetadata"]
        labels = ["local_cycle"] + [event["label"] for event in events]
        registry = [f"autocounter_{part}_{label}" for label in labels for part in ["low", "high"]]
        registry += ["cycles_low", "cycles_high", "readrate_low", "readrate_high", "init_done", "countersready", "readdone"]
        expected = [(name, 4 * i) for i, name in enumerate(registry)]
        assert [(name, int(offset)) for name, offset, _, _ in rows] == expected
        abi = "".join(f"{name}={offset}\n" for name, offset in expected)
        assert (sfc / f"host-{index}/abi.txt").read_text() == abi, "SFC positional MMIO ABI differs"
        addresses = dict(expected)
        driver = DRIVER
        driver = driver.replace("Verilated::commandArgs(argc,argv);d=new VWidget;",
                                "Verilated::commandArgs(argc,argv);Verilated::randReset(2);d=new VWidget;")
        driver = driver.replace("RESET=1;C(aw_bits_len)=0;", "RESET=1;HV=0;FR=0;TRIGGER=0;GLOBALRESET=0;C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;C(ar_valid)=0;C(r_ready)=0;C(aw_bits_addr)=0;C(ar_bits_addr)=0;C(aw_bits_id)=0;C(ar_bits_id)=0;C(w_bits_data)=0;C(aw_bits_len)=0;")
        driver = driver.replace("for(unsigned n=0;n<4;++n)step();RESET=0;",
                                "CLOCK=0;d->eval();CLOCK=1;d->eval();for(unsigned n=0;n<4;++n)step();RESET=0;", 1)
        driver = driver.replace("#define EVENT0 d->@EVENT0@\n#define EVENT1 d->@EVENT1@", "")
        assignments = []
        for i, event in enumerate(events):
            # Wide identities exercise the upper MMIO word; narrow accumulate
            # events exercise source-derived field widths and distinct slots.
            expression = f"((uint64_t(cycle)<<{32+i})^(UINT64_MAX-cycle*{17+2*i}))"
            if event["width"] < 64:
                expression += f" & UINT64_C({(1 << event['width']) - 1})"
            assignments.append(f"d->@EVENT{i}@={expression};")
        driver = driver.replace("EVENT0=(cycle%7==0)?UINT64_MAX:(cycle*11+3);\n  EVENT1=(cycle<<40)^(UINT64_MAX-cycle*17);", "\n  ".join(assignments))
        for old, name in [(32, "readrate_low"), (36, "readrate_high"), (40, "init_done"), (44, "countersready"), (48, "readdone")]:
            for op in ["busWrite", "busRead"]:
                driver = driver.replace(f"{op}({old},", f"{op}(@{name}@,")
        for name, address in addresses.items():
            driver = driver.replace(f"@{name}@", str(address))
        driver = driver.replace("word<8;", f"word<{2*len(labels)+2};")
        traces = []
        for kind in ["native", "sfc"]:
            target = "GGAutoCounterBridge" + (f"_{index}" if index else "") + "Host" if kind == "native" else "AutoCounterBridgeModule"
            rtl = native / f"post-autocounter-csv-host-{index}.sv" if kind == "native" else case / "sfc.sv"
            if kind == "sfc":
                run([args.firtool.resolve(), sfc / f"host-{index}/host.fir", "--disable-all-randomization", "--disable-annotation-unknown", "-o", rtl], case / "sfc-syntax.log")
            prefix = "hBits_" if kind == "native" else "hPort_hBits_"
            mapping = dict(CTRL="ctrl_", CLOCK="hostClock", RESET="hostReset", HV="hValid", HR="hReady", FV="fromHostValid", FR="fromHostReady")
            if kind == "sfc":
                mapping = dict(CTRL="io_ctrl_", CLOCK="clock", RESET="reset", HV="hPort_toHost_hValid", HR="hPort_toHost_hReady", FV="hPort_fromHost_hValid", FR="hPort_fromHost_hReady")
            mapping.update(TRIGGER=prefix+key["triggerName"], GLOBALRESET=prefix+key["resetPortName"])
            mapping.update({f"EVENT{i}": prefix+event["portName"] for i, event in enumerate(events)})
            source = driver
            for token, value in mapping.items():
                source = source.replace(f"@{token}@", value)
            assert not re.search(r"@[A-Z][A-Z0-9_]*@", source), "unbound driver field"
            cpp, obj = case / f"{kind}.cpp", case / f"{kind}-obj"
            cpp.write_text(source)
            run([args.verilator.resolve(), "--cc", "--exe", "--build", "-j", "2", "-Wno-fatal", "--assert", "--top-module", target, "--prefix", "VWidget", "--Mdir", obj, rtl, cpp], case / f"{kind}-build.log")
            trace = case / f"{kind}.trace"
            run([obj / "VWidget", f"+verilator+seed+{101 if kind == 'native' else 202}"], case / f"{kind}-run.log", trace)
            traces.append(trace)
            artifacts += [rtl, cpp, trace]
        assert traces[0].read_bytes() == traces[1].read_bytes(), f"host-{index}: cycle-accurate MMIO/sample mismatch"
        receipts.append(dict(widget=index, passed=True, ordered_events=len(events), exact_mmio_registers=rows, cycles=len(traces[0].read_text().splitlines())))
    artifacts += [native / "post-autocounter-csv.mlir", native / "post-autocounter-csv-all.json", out / "commands.json", Path(__file__).resolve(), Path(__file__).with_name("AutoCounterBridgeCompare.py")]
    result = dict(passed=True, scope="source annotations -> exact ordered CSV constructors -> actual local F2 host engines", full_feature_acceptance=False, widgets=receipts,
                  checks=["exact SFC register labels, addresses and constructor event order", "actual compiler-emitted host RTL", "accepted-token stalls, trigger/global reset, host reset", "period 0/3/17 sampling, full-queue sample drops, CSV reads", "independent AW/W timing, R/B stalls and response IDs"],
                  assumptions=["external host-token stimuli; target source outputs are not connected through FAME in this boundary", "SFC local widget elaboration uses F2 and base1/1 clock metadata; MMIO geometry is independent of clock ratio", "independent randomized initial states (native seed101/SFC seed202), then one asserted-reset edge before observations; sample slots remain unreset", "sample queue payloads observed only after valid enqueue and countersready; no preclear or zero-out policy changes"],
                  remaining=["select unused cover records before production normalization", "clock/trigger/reset wiring, FAME tokenization and actual host binding", "global MMIO/header/platform assembly and complete Print", "enabled representative semantic, LEC, SEC and F2 Linux evidence"],
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts})
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
