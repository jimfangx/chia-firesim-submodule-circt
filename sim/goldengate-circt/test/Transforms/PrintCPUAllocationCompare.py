#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the Print CPU boundary with the immutable SFC CPU stream engine.

Arguments: iteration evidence directory, immutable U250 generated RTL.
Print is optional in that fixture; its transport uses the same CPU stream ABI
as TracerV. Compare resolved handshake terms and occupancy forwarding, retaining
the distinction between a selected Print candidate and the recorded engine.
"""
import json
import re
import sys
from pathlib import Path


def module(text, name):
    matches = re.findall(r"\bmodule\s+" + re.escape(name) + r"\s*\((.*?)\);(.*?)\bendmodule", text, re.S)
    assert len(matches) == 1, f"expected one {name} module"
    return matches[0]


def clean(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def instance(body, kind, name):
    matches = re.findall(r"\b" + kind + r"\s+" + name + r"\s*\((.*?)\);", body, re.S)
    assert len(matches) == 1, f"expected one {kind} {name} instance"
    return dict(re.findall(r"\.(\w+)\s*\(\s*(\w+)\s*\)", matches[0]))


def equations(body):
    return dict(re.findall(r"\b(?:wire(?:\s*\[[^]]+\])?|assign)\s+(\w+)\s*=\s*([^;]+);", body))


def terms(expression, equations, names, active=()):
    result = set()
    for term in expression.split("&"):
        term = term.strip()
        if term in names:
            result.add(names[term])
        else:
            assert term in equations and term not in active, f"unresolved handshake term {term}"
            result.update(terms(equations[term], equations, names, active + (term,)))
    return result


evidence, golden = Path(sys.argv[1]), Path(sys.argv[2])
candidate = evidence / "candidate"
native = clean((candidate / "post-print-cpu-streams.sv").read_text())
reference = clean(golden.read_text())
_, read = module(native, "GGCPUStreamRead")
_, engine = module(reference, "CPUManagedStreamEngine")
nq, rq = equations(read), equations(engine)
prefix = "TRACERVBRIDGEMODULE_0_to_cpu_stream_"
native_grants = [name for name, expression in nq.items()
                 if re.fullmatch(r"ar_bits_addr\[63:19\]\s*==\s*45'h0", expression.strip())]
assert len(native_grants) == 1
native_lasts = [name for name, expression in nq.items()
                if re.fullmatch(r"readBeatCounter\s*==\s*\{1'h0,\s*ar_bits_len\}", expression.strip())]
assert len(native_lasts) == 1
assert re.fullmatch(r"auto_cpu_managed_axi4_in_ar_bits_addr\[63:19\]\s*==\s*45'h0",
                    rq[prefix + "grant"].strip()), "SFC stream window differs"
nn = {"ar_valid": "ar_valid", "r_ready": "r_ready", "stream_valid": "queue_valid",
      native_grants[0]: "grant", native_lasts[0]: "last"}
rn = {"auto_cpu_managed_axi4_in_ar_valid": "ar_valid", "auto_cpu_managed_axi4_in_r_ready": "r_ready",
      prefix + "ser_des_io_narrow_out_valid": "queue_valid", prefix + "grant": "grant",
      prefix + "lastReadBeat": "last"}
handshakes = {}
for output, reference_output in {
    "r_valid": "auto_cpu_managed_axi4_in_r_valid",
    "ar_ready": "auto_cpu_managed_axi4_in_ar_ready",
    "stream_ready": prefix + "ser_des_io_narrow_out_ready",
}.items():
    actual, expected = terms(nq[output], nq, nn), terms(rq[reference_output], rq, rn)
    assert actual == expected, f"{output} handshake differs: {actual} != {expected}"
    handshakes[output] = sorted(actual)
assert nq["r_bits_id"].strip() == "ar_bits_id"
assert rq["auto_cpu_managed_axi4_in_r_bits_id"].strip() == "auto_cpu_managed_axi4_in_ar_bits_id"
assert nq["r_bits_data"].strip() == "stream_bits"
assert rq["auto_cpu_managed_axi4_in_r_bits_data"].strip() == prefix + "ser_des_io_narrow_out_bits"
assert re.search(r"reg\s*\[8:0\]\s*readBeatCounter", read)
assert re.search(r"reg\s*\[8:0\]\s*" + prefix + "readBeatCounter", engine)
_, counts = module(native, "GGCPUStreamCountBank")
assert equations(counts)["mcr_read_0_bits"].strip() == "{19'h0, count}"
assert rq["crFile_io_mcr_read_0_bits"].strip() == "{{19'd0}, " + prefix + "outgoingQueueIO_q_io_count}"
_, read_wrapper = module(native, "GGCPUStreamReadWrapper")
bound = instance(read_wrapper, "GGPrintBridgeHostWrapper", "sim")
transport = instance(read_wrapper, "GGCPUStreamRead", "cpuStreamRead")
for leaf in ("ready", "valid", "bits"):
    assert bound["print_0_stream_" + leaf] == transport["stream_" + leaf], "Print queue is detached from CPU transport"
_, count_wrapper = module(native, "GGCPUStreamCountWrapper")
read_instance = instance(count_wrapper, "GGCPUStreamReadWrapper", "sim")
count_instance = instance(count_wrapper, "GGCPUStreamCountBank", "streamCount")
assert read_instance["print_0_stream_count"] == count_instance["count"], "Print occupancy is detached from MCR"
mlir = (candidate / "post-print-cpu-streams.mlir").read_text()
assert re.search(r'goldengate.sourceStreams = \[\{bufferBaseAddress = 0 : i64, depth = 6144 : i64, index = 0 : i64, name = "PRINTBRIDGEMODULE_0_to_cpu_stream", port = "print_0_stream", widthBytes = 64 : i64\}\]', mlir)
report = {"golden": str(golden), "candidate": str(candidate / "post-print-cpu-streams.sv"),
          "handshakes": handshakes, "window_bytes": 524288, "depth": 6144,
          "stream_bits": 512, "count_bits": 13, "count_word": 0,
          "print_stream_index": 0, "queue_and_count_connected": True,
          "reference_has_print_host": False}
(evidence / "cpu-allocation-comparison.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS Print CPU AXI handshake, queue/count connectivity and SFC stream geometry")
