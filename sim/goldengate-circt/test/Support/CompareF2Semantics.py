#!/usr/bin/env python3
"""CHIA's fail-closed F2 pre-build syntax and static-semantic comparison.

Complete input/output receipts are checked before comparing static contracts and
launching paired complete RTL lint and real driver-header syntax checks. These
checks permit the independent Jasper and FPGA verification nodes to start. A
pre-build pass does not assert full behavioral equivalence or synthesis success.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

BASE = "FireSim-generated"
BRIDGES = ("firesim.lib.bridgeutils.BridgeIOAnnotation",
           "midas.passes.fame.FAMEChannelConnectionAnnotation")
MODELS = tuple("midas.passes.fame." + name for name in (
    "FAMEChannelConnectionAnnotation", "FAMEChannelFanoutAnnotation",
    "FAMEChannelPortsAnnotation", "FAMEHostClock", "FAMEHostReset", "FAMETransformAnnotation"))
DEFERRED_OBLIGATIONS = {
    "jasper": [
        "Prove target and bridge logic, memory contents/read-write behavior and initialization, clock/reset/token schedules, MMIO transactions and backpressure with separate LEC and SEC results.",
        "Cover behavioral effects of annotation classes outside the selected pre-build annotation contracts; static inventories do not prove their implementation.",
    ],
    "firesim": [
        "Verify constraint pin persistence after synthesis/link and actual Vivado timing; emitted-RTL hierarchy resolution alone does not establish these properties.",
        "Validate the built CIRCT image, consumed sidecars, driver and workload on F2.",
    ],
}

STATIC_CHECKS = (
    ("bridge_extraction",), ("infer_model_ports",),
    ("public_interfaces", "F1Shim"), ("public_interfaces", "FPGATop"),
    ("driver_abi",), ("defines.vh",), ("synthesis.xdc",),
    ("implementation.xdc",), ("peer2peer.const.yaml",), ("partition.const.h",),
    *(("bridge_extraction", "classes", name) for name in BRIDGES),
    *(("infer_model_ports", "classes", name) for name in MODELS),
)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def checked_file(path, record):
    if (not isinstance(record, dict) or set(record) != {"bytes", "sha256"}
            or type(record["bytes"]) is not int or record["bytes"] < 0
            or not isinstance(record["sha256"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError(f"malformed file record: {path}")
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing or symlink artifact: {path}")
    if path.stat().st_size != record["bytes"] or digest(path) != record["sha256"]:
        raise ValueError(f"artifact hash/size mismatch: {path}")


def bound_file(record):
    if not isinstance(record, dict) or set(record) != {"path", "bytes", "sha256"}:
        raise ValueError("malformed receipt binding")
    path = Path(record["path"])
    if not path.is_absolute():
        raise ValueError("receipt paths must be absolute")
    checked_file(path, {k: record[k] for k in ("bytes", "sha256")})
    return path


def receipt(root, target, kind):
    marker = root / target / "complete.json"
    data = json.loads(marker.read_text())
    if data.get("target") != target or data.get("status") != "complete":
        raise ValueError(f"incomplete/wrong-target receipt: {marker}")
    manifest_path = bound_file(data["manifest"])
    if manifest_path.parent.parent.resolve() != (root / target).resolve():
        raise ValueError("manifest outside selected target archive")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("status") != "complete" or manifest.get("target") != target:
        raise ValueError("incomplete/wrong-target manifest")
    output = manifest_path.parent / kind
    if not output.is_dir() or any(p.is_symlink() for p in (manifest_path.parent, output)):
        raise ValueError("missing or symlink archive directory")
    inventory = manifest["sfc_artifacts" if kind == "sfc" else "circt_inventory"]
    if not isinstance(inventory, dict) or not inventory:
        raise ValueError("empty/malformed artifact inventory")
    paths = set()
    for name, record in inventory.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or relative.as_posix() != name:
            raise ValueError("unsafe artifact inventory path")
        if any(parent.is_symlink() for parent in [output / part for part in relative.parents]):
            raise ValueError("symlink artifact ancestor")
        checked_file(output / relative, record)
        paths.add(name)
    actual = {str(p.relative_to(output)) for p in output.rglob("*") if p.is_file()}
    if actual != paths:
        raise ValueError(f"artifact inventory membership differs: {output}")
    return output, manifest, data["manifest"]


def annotations(path):
    values = json.loads(path.read_text())
    if not isinstance(values, list) or any(not isinstance(a, dict) or
            not isinstance(a.get("class"), str) for a in values):
        raise ValueError(f"malformed annotation array: {path}")
    return values


def boundary(golden, candidate, classes):
    a, b = annotations(golden), annotations(candidate)
    checks = {}
    for cls in classes:
        records = lambda xs: Counter(json.dumps(v, sort_keys=True) for v in xs if v["class"] == cls)
        expected, actual = records(a), records(b)
        checks[cls] = {"golden_count": sum(expected.values()), "candidate_count": sum(actual.values()),
                       "match": expected == actual, "missing": list((expected - actual).elements()),
                       "extra": list((actual - expected).elements())}
    if not checks[classes[0]]["golden_count"]:
        raise ValueError(f"required annotation class absent: {golden}: {classes[0]}")
    return {"match": all(c["match"] for c in checks.values()), "classes": checks,
            "normalization": "dictionary and record order only; duplicates and ordered array values retained"}


def ports(text, name):
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    headers = re.findall(r"\bmodule\s+" + re.escape(name) + r"\s*\((.*?)\);", text, re.S)
    if len(headers) != 1:
        raise ValueError(f"expected one unparameterized module {name}")
    result, direction, width = {}, None, None
    for item in headers[0].split(","):
        item = item.strip()
        m = re.fullmatch(r"(input|output|inout)\s+(?:wire\s+)?(?:\[(\d+):0\]\s*)?(\w+)", item)
        if m:
            direction, msb, pin = m.groups()
            width = int(msb) + 1 if msb else 1
        elif direction and re.fullmatch(r"\w+", item):
            pin = item
        else:
            raise ValueError(f"unrecognized {name} port: {item}")
        if pin in result:
            raise ValueError(f"duplicate {name} port: {pin}")
        result[pin] = [direction, width]
    return result


def compare_values(expected, actual):
    return {"match": expected == actual, "golden": expected, "candidate": actual}


def implementation_clocks(xdc, rtl):
    """Resolve only the selected generated-clock snippet subset in emitted RTL.

    BUFGCE is the checked-in platform primitive (I/CE inputs, O clock output).
    External WRAPPER/CL/firesim_top belongs to the F2 shell, not this RTL.
    This deliberately does not interpret general Tcl or synthesized netlists.
    """
    rtl = re.sub(r"//[^\n]*|/\*.*?\*/", "", rtl, flags=re.S)
    modules = {}
    for match in re.finditer(r"\bmodule\s+([A-Za-z_]\w*)\b(.*?)\bendmodule\b", rtl, re.S):
        if match[1] in modules:
            raise ValueError(f"ambiguous XDC RTL module: {match[1]}")
        modules[match[1]] = match[2]
    lines = [line.strip() for line in xdc.splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    if not lines or len(lines) % 3:
        raise ValueError("unsupported generated-clock XDC command sequence")
    records, used_paths = {}, set()
    for index in range(0, len(lines), 3):
        create = re.fullmatch(
            r"create_generated_clock -name ([A-Za-z_][A-Za-z_0-9.,]*)"
            r" -source \[get_pins -of \[get_clocks host_clock\]\]"
            r" \[get_pins -hierarchical -regexp \.\*(WRAPPER/CL/firesim_top/(?:[A-Za-z_]\w*/)+O)\.\*\]"
            r" -divide_by ([1-9]\d*)", lines[index])
        setup = re.fullmatch(r"set_multicycle_path ([1-9]\d*) -setup -from \[get_clocks (\S+)\] -to \[get_clocks (\S+)\]", lines[index + 1])
        hold = re.fullmatch(r"set_multicycle_path (\d+) -hold\s+-from \[get_clocks (\S+)\] -to \[get_clocks (\S+)\]", lines[index + 2])
        if not create or not setup or not hold:
            raise ValueError("unsupported generated-clock XDC command")
        name, path, divide = create.groups()
        if (name in records or path in used_paths or
                setup[2] != name or setup[3] != name or hold[2] != name or hold[3] != name or
                int(hold[1]) != int(setup[1]) - 1):
            raise ValueError("duplicate or inconsistent generated-clock XDC")
        components = path.removeprefix("WRAPPER/CL/firesim_top/").split("/")
        module, traversal = "F1Shim", []
        for instance in components[:-1]:
            if module not in modules:
                raise ValueError(f"unresolved XDC RTL module: {module}")
            # Generated hierarchy instances have no parameters or escaped names.
            # Reject additional/ambiguous instances instead of guessing aliases.
            found = re.findall(r"(?m)^\s*([A-Za-z_]\w*)\s+" + re.escape(instance) +
                               r"\s*\((.*?)\);", modules[module], re.S)
            if len(found) != 1:
                raise ValueError(f"unresolved or ambiguous XDC instance: {module}/{instance}")
            child, connections = found[0]
            traversal.append({"parent": module, "instance": instance, "module": child})
            module = child
        pins = re.findall(r"\.([A-Za-z_]\w*)\s*\(", connections)
        if module != "BUFGCE" or sorted(pins) != ["CE", "I", "O"]:
            raise ValueError("XDC endpoint must resolve to supported BUFGCE/O")
        records[name] = {"path": path, "resolved_hierarchy": traversal,
                         "gate_instance": components[-2], "primitive": module, "pin": "O",
                         "divide_by": int(divide), "setup": int(setup[1]), "hold": int(hold[1])}
        used_paths.add(path)
    return records


def compare_implementation_clocks(golden, candidate):
    fields = ("gate_instance", "primitive", "pin", "divide_by", "setup", "hold")
    normalized = lambda records: {name: {key: record[key] for key in fields}
                                  for name, record in records.items()}
    return {"match": normalized(golden) == normalized(candidate),
            "golden": golden, "candidate": candidate,
            "normalization": "Snippet order and resolved parent-instance spelling only; clock names, gate identities and clock parameters retained.",
            "scope": "Emitted RTL hierarchy and supported BUFGCE/O primitive only; external F2 shell prefix, post-synthesis/link pin persistence and Vivado timing are not verified."}


def stream_metadata(text):
    """Compare manager-visible stream allocations, rejecting duplicate YAML keys."""
    import yaml

    class UniqueLoader(yaml.SafeLoader):
        pass

    def mapping(loader, node):
        result = {}
        for key, value in loader.construct_pairs(node, deep=True):
            if not isinstance(key, str) or key in result:
                raise ValueError("non-string or duplicate stream metadata key")
            result[key] = value
        return result

    UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)
    values = yaml.load(text, Loader=UniqueLoader)
    if values is None and not re.sub(r"#[^\n]*|\s+", "", text):
        values = {}
    if not isinstance(values, dict):
        raise ValueError("stream metadata must be a mapping")
    fields = {"bufferBaseAddress", "bufferCapacity", "bufferWidthBytes"}
    for name, allocation in values.items():
        if (not re.fullmatch(r"[A-Za-z_]\w*", name) or not isinstance(allocation, dict)
                or set(allocation) != fields
                or any(type(v) is not int for v in allocation.values())
                or allocation["bufferBaseAddress"] < 0
                or allocation["bufferCapacity"] <= 0
                or allocation["bufferWidthBytes"] <= 0):
            raise ValueError(f"unsupported stream allocation: {name}")
    return values


def partition_contract(text):
    # The selected four recipes are unpartitioned. Any non-comment C++ token
    # needs a supported partition contract, rather than a spelling normalizer.
    if re.sub(r"//[^\n]*|/\*.*?\*/|\s+", "", text, flags=re.S):
        raise ValueError("selected F2 recipe requires an unpartitioned header")
    return "unpartitioned"


def driver_contract(text, helper):
    # The existing ABI normalizer removes whitespace from C++ source. Preserve
    # decoded string values separately so whitespace within literals cannot hide
    # a changed register, bridge or argument string.
    escaped = re.sub(r'R"ESC\((.*?)\)ESC"', lambda m: json.dumps(m[1]), text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\])*"',
                  lambda m: json.dumps(json.loads(m[0]), ensure_ascii=True).replace(" ", r"\u0020"), escaped)
    configs = re.findall(r'static constexpr TargetConfig conf_target\{(.*?)\};', text, re.S)
    if len(configs) != 1:
        raise ValueError("expected one complete TargetConfig")
    constructors = helper.constructors(text)
    asserts = sorted(helper.normalize(s) for s in re.findall(r'static_assert\(.*?;', text, re.S))
    if not constructors or not asserts:
        raise ValueError("missing constructor or static ABI assertions")
    return {"constructors": constructors, "assertions": asserts,
            "target_config": helper.normalize(configs[0])}


def compare_driver_contracts(expected, actual, protocol=None):
    """Compare the base ABI and explicitly report the native F2 ACK extension.

    SFC's five-argument StreamParameters keeps its legacy default. The native
    F2 source vector may add exactly a sixth `true` to every source, selecting
    non-destructive PCI reads and explicit OCL dequeue acknowledgement. This is
    a platform protocol change, not C++ spelling equivalence or a formal waiver.
    All original arguments, sink entries, widgets, assertions and config must
    still compare exactly. The paired syntax check compiles both full headers.
    """
    result = compare_values(expected, actual)
    result['raw_match'] = result['match']
    if result['match']:
        if protocol is not None:
            result.update(match=False, protocol_error='Unselected native F2 ACK protocol metadata')
        return result
    constructors = actual.get('constructors', {})
    reference = expected.get('constructors', {})
    key = 'CPUManagedStreamWidget'
    if (key not in constructors or key not in reference or
            any(name.startswith(key + '[') for name in constructors)):
        return result
    guard, arguments = constructors[key]
    vector = 'std::vector<CPUManagedStreams::StreamParameters>{'
    parts = arguments.split(vector)
    if len(parts) != 3 or not parts[1].endswith('},') or not parts[2].endswith('}'):
        return result
    sinks, sources = parts[1][:-2], parts[2][:-1]
    parameter = re.compile(r'CPUManagedStreams::StreamParameters\(("(?:\\.|[^"\\])*"),'
                           r'([0-9]+),([0-9]+),([0-9]+),([0-9]+)(?:,(true|false))?\)')
    def entries(text):
        if not text:
            return []
        rows, position = [], 0
        while position < len(text):
            match = parameter.match(text, position)
            if match is None:
                return None
            rows.append(match)
            position = match.end()
            if position == len(text):
                break
            if text[position] != ',':
                return None
            position += 1
            if position == len(text):
                return None
        return rows
    sink_rows, source_rows = entries(sinks), entries(sources)
    if (sink_rows is None or not source_rows or
            any(row[6] is not None for row in sink_rows) or
            any(row[6] != 'true' for row in source_rows)):
        return result
    # Preserve the exact first five arguments; do not normalize any other true
    # literal or a flag on a sink. Unknown/partial profiles remain mismatches.
    legacy_sources = ','.join('CPUManagedStreams::StreamParameters(' +
                              ','.join(row.group(i) for i in range(1, 6)) + ')'
                              for row in source_rows)
    legacy_arguments = parts[0] + vector + parts[1] + vector + legacy_sources + '}'
    base = {**actual, 'constructors': {**constructors, key: (guard, legacy_arguments)}}
    result['base_abi_match'] = base == expected
    protocol_sources = [dict(name=json.loads(row[1]), index=index, countAddress=int(row[3]),
                             ackWriteValue=1, beatBytes=64)
                        for index, row in enumerate(source_rows)]
    expected_protocol = dict(version=2, platform='f2', protocol='source-count-write1-ack',
        liveHierarchyAndPredicatesValidated=True, controlWriteStrobesValidated=True,
        pciReadsNonDestructive=True, sources=protocol_sources)
    strict_types = (isinstance(protocol, dict) and type(protocol.get('version')) is int and
        type(protocol.get('liveHierarchyAndPredicatesValidated')) is bool and
        type(protocol.get('controlWriteStrobesValidated')) is bool and
        type(protocol.get('pciReadsNonDestructive')) is bool and
        isinstance(protocol.get('sources'), list) and all(isinstance(row, dict) and
            all(type(row.get(key)) is int for key in ('index', 'countAddress', 'ackWriteValue', 'beatBytes'))
            for row in protocol['sources']))
    result['protocol_metadata_match'] = bool(strict_types and protocol == expected_protocol)
    result['match'] = result['base_abi_match'] and result['protocol_metadata_match']
    result['platform_protocol_extension'] = {
        'profile': 'native_f2_explicit_source_ack',
        'source_streams': [dict(name=json.loads(row[1]), count_address=int(row[3]))
                           for row in source_rows],
        'dequeue': 'One explicit OCL acknowledgement after each complete 64-byte host read; PCI reads do not dequeue',
        'formal_equivalence_claim': False,
        'runtime_verified': False,
        'scope': 'Declared source protocol extension only; all five original arguments and the remaining ABI are strict. Behavioral verification remains mandatory.'}
    return result


def verify_pair(gm, cm, binding, recipe, recipe_name):
    for manifest in (gm, cm):
        if manifest["recipe"] != recipe or manifest["recipe_name"] != recipe_name:
            raise ValueError("archive recipe differs from exact selected F2 recipe")
    provenance = cm["provenance"]["golden"]
    if provenance["manifest"] != binding:
        raise ValueError("candidate uses a different immutable golden manifest")
    for key in ("firrtl", "annotations"):
        bound_file(gm["preMIDAS"][key])
        if provenance["preMIDAS"][key] != gm["preMIDAS"][key]:
            raise ValueError(f"candidate pre-MIDAS {key} identity differs")


def file_record(path):
    path = path.resolve()
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"missing/empty syntax dependency: {path}")
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def define_arguments(text):
    """Accept only the emitted literal define subset; use identical arguments."""
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    defines = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"\s*`define\s+([A-Za-z_]\w*)(?:\s+([A-Za-z_0-9']+))?\s*", line)
        if not match or match[1] in defines:
            raise ValueError(f"unsupported/duplicate emitted define: {line}")
        defines[match[1]] = match[2]
    return ["+define+" + name + ("=" + value if value is not None else "")
            for name, value in sorted(defines.items())]


def finalized_resources(output, manifest, kind):
    """Verify that all actual archived resource bodies reach the linted RTL."""
    mappings = manifest["sfc_resources" if kind == "sfc" else "resources"]
    if not isinstance(mappings, list):
        raise ValueError("malformed archived RTL resource mappings")
    expected = (output / (BASE + ".raw.sv")).read_bytes()
    resources = []
    for mapping in mappings:
        archived = bound_file(mapping["archived"])
        if not archived.is_relative_to(output):
            raise ValueError("RTL resource outside checked output inventory")
        if {k: mapping["source"][k] for k in ("bytes", "sha256")} != {
                k: mapping["archived"][k] for k in ("bytes", "sha256")}:
            raise ValueError("archived RTL resource differs from original source")
        expected += archived.read_bytes() + b"\n"
        resources.append(mapping["archived"])
    if expected != (output / (BASE + ".sv")).read_bytes():
        raise ValueError("final RTL does not contain exactly the archived resource append")
    return resources


def logged_command(argv, cwd, directory, timeout, env=None):
    directory.mkdir(parents=True, exist_ok=False)
    record = {"argv": [str(a) for a in argv], "cwd": str(cwd),
              "timeout_seconds": timeout, "stdout": str(directory / "stdout.log"),
              "stderr": str(directory / "stderr.log")}
    start = time.monotonic()
    with (directory / "stdout.log").open("w") as out, (directory / "stderr.log").open("w") as err:
        try:
            run = subprocess.Popen(record["argv"], cwd=cwd, env=env, stdout=out,
                                   stderr=err, start_new_session=True)
            try:
                run.wait(timeout=timeout)
                record.update(returncode=run.returncode, timed_out=False)
            except subprocess.TimeoutExpired:
                os.killpg(run.pid, signal.SIGKILL)
                run.wait()
                record.update(returncode=run.returncode, timed_out=True)
        except OSError as exc:
            record.update(returncode=None, timed_out=False, error=f"{type(exc).__name__}: {exc}")
    record["seconds"] = time.monotonic() - start
    record["passed"] = record["returncode"] == 0 and not record["timed_out"]
    (directory / "command.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return record


def driver_source(header):
    include = '#include ' + json.dumps(str(header.resolve())) + '\n'
    return ('#include "core/config.h"\n#include "core/widget_registry.h"\n'
            '#define GET_INCLUDES\n' + include + '#undef GET_INCLUDES\n'
            '#include "bridges/loadmem.h"\n'
            '#define GET_SUBSTRUCT_CHECKS\n' + include + '#undef GET_SUBSTRUCT_CHECKS\n'
            '#define GET_METASIM_INTERFACE_CONFIG\n' + include +
            '#undef GET_METASIM_INTERFACE_CONFIG\n'
            'void build(widget_registry_t &registry, simif_t &simif, const std::vector<std::string> &args) {\n' +
            ''.join('#define ' + guard + '\n' + include + '#undef ' + guard + '\n'
                    for guard in ('GET_CORE_CONSTRUCTOR', 'GET_MANAGED_STREAM_CONSTRUCTOR',
                                  'GET_BRIDGE_CONSTRUCTOR')) + '}\n')


def syntax_commands(verilator, cxx, rtl, library, defines, source, includes, mdir):
    # Warnings stay visible and are not acceptance of any behavioral difference.
    # Missing modules/unsupported constructs remain fatal; no blackbox or stub flags.
    return ([str(verilator), '--lint-only', '--timing', '-Wno-fatal',
             '--error-limit', '25', '--top-module', 'F1Shim', '--Mdir', str(mdir),
             *defines, str(rtl), str(library)],
            [str(cxx), '-std=c++20', '-fsyntax-only',
             *[arg for path in includes for arg in ('-I', str(path))], str(source)])


def paired_syntax(repo, golden, candidate, gm, cm, evidence):
    evidence.mkdir(parents=True, exist_ok=False)
    verilator, cxx = repo / '.conda-env/bin/verilator', repo / '.conda-env/bin/c++'
    backend = repo / '.conda-env/bin/verilator_bin'
    library = repo / 'sims/firesim/sim/midas/src/main/verilog/BUFGCE.v'
    includes = [repo / 'sims/firesim/sim/midas/src/main/cc',
                repo / 'generators/firechip/bridgestubs/src/main/cc',
                repo / '.conda-env/riscv-tools/include']
    if any(not path.is_dir() for path in includes):
        raise ValueError("missing real driver include directory")
    dependencies = [file_record(p) for directory in includes
                    for p in sorted(directory.rglob('*')) if p.is_file() and p.suffix in ('.h', '.hpp')]
    tools = {name: file_record(path) for name, path in
             [('verilator', verilator), ('verilator_backend', backend), ('cxx', cxx)]}
    env = dict(os.environ)
    # Pin the launcher to its own archived backend; an inherited override cannot
    # silently substitute another Verilator installation or model search tree.
    for override in ('VERILATOR_ROOT', 'VERILATOR_BIN', 'CPATH', 'CPLUS_INCLUDE_PATH',
                     'C_INCLUDE_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        env.pop(override, None)
    versions = {name: logged_command([path, '--version'], repo, evidence / ('version-' + name), 30, env)
                for name, path in [('verilator', verilator), ('cxx', cxx)]}
    define_texts = [(path / (BASE + '.defines.vh')).read_text() for path in (golden, candidate)]
    if define_texts[0] != define_texts[1]:
        raise ValueError("paired syntax requires identical emitted platform defines")
    defines = define_arguments(define_texts[0])
    result = {"passed": False, "tools": tools, "versions": versions,
              "primitive_library": file_record(library), "driver_header_dependencies": dependencies,
              "defines": defines, "top": "F1Shim", "pairs": {},
              "scope": "Both complete finalized RTL designs in Verilator simulation mode, including every receipted appended blackbox body and the same functional BUFGCE model. Both complete emitted driver headers compiled against real driver declarations in entry.cc include/constructor phases. No behavioral equivalence or synthesis primitive proof."}
    for kind, output, manifest in [('sfc', golden, gm), ('circt', candidate, cm)]:
        resources = finalized_resources(output, manifest, kind)
        source = evidence / (kind + '-driver.cpp')
        source.write_text(driver_source(output / (BASE + '.const.h')))
        rtl, driver = syntax_commands(verilator, cxx, output / (BASE + '.sv'), library,
                                      defines, source, includes, evidence / (kind + '-obj'))
        result['pairs'][kind] = {
            'rtl': logged_command(rtl, repo, evidence / (kind + '-rtl'), 1200, env),
            'driver': logged_command(driver, repo, evidence / (kind + '-driver'), 180, env),
            'rtl_input': file_record(output / (BASE + '.sv')),
            'driver_header': file_record(output / (BASE + '.const.h')),
            'appended_resources': resources, 'driver_source': file_record(source)}
    result['passed'] = (all(v['passed'] for v in versions.values()) and
                        all(pair[key]['passed'] for pair in result['pairs'].values()
                            for key in ('rtl', 'driver')))
    (evidence / 'result.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


def compare(repo, target, golden_root, candidate_root, syntax_evidence=None):
    import yaml

    sfc = load_module("f2_semantic_recipes", Path(__file__).with_name("GenerateF2SFCGoldens.py"))
    recipes = sfc.validate_recipes(yaml.safe_load((repo / "automation/chia_firesim_circt/selected_config_build_recipes.yaml").read_text()),
                                  yaml.safe_load((repo / "sims/firesim-staging/sample_config_build_recipes.yaml").read_text()))
    if target not in recipes:
        raise ValueError(f"unsupported target: {target}")
    golden, gm, binding = receipt(golden_root, target, "sfc")
    candidate, cm, candidate_binding = receipt(candidate_root, target, "circt")
    verify_pair(gm, cm, binding, recipes[target], sfc.TARGETS[target][0])
    details = {"input_pair": gm["preMIDAS"], "golden_manifest": binding,
               "candidate_manifest": candidate_binding, "candidate_preparation_identity": cm["input_identity"],
               "bridge_extraction": boundary(golden / "post-bridge-extraction.json", candidate / "post-bridge-extraction-all.json", BRIDGES),
               "infer_model_ports": boundary(golden / "post-infer-model-ports.json", candidate / "post-infer-model-ports.json", MODELS)}
    rtl = [(p / (BASE + ".sv")).read_text() for p in (golden, candidate)]
    details["public_interfaces"] = {name: compare_values(*(ports(t, name) for t in rtl)) for name in ("F1Shim", "FPGATop")}
    helper = load_module("f2_semantic_driver_contract", Path(__file__).with_name("NICDriverHeaderCompare.py"))
    protocol_path = candidate / (BASE + '.f2-cpu-stream-protocol.json')
    protocol = json.loads(protocol_path.read_text()) if protocol_path.exists() else None
    details["driver_abi"] = compare_driver_contracts(*(driver_contract((p / (BASE + ".const.h")).read_text(), helper) for p in (golden, candidate)), protocol=protocol)
    if protocol is not None:
        details['driver_abi']['protocol_artifact'] = file_record(protocol_path)
    for ext in ("defines.vh", "synthesis.xdc"):
        details[ext] = compare_values(*((p / (BASE + "." + ext)).read_text() for p in (golden, candidate)))
    details["implementation.xdc"] = compare_implementation_clocks(*(
        implementation_clocks((p / (BASE + ".implementation.xdc")).read_text(), text)
        for p, text in zip((golden, candidate), rtl)))
    for ext, contract in (("peer2peer.const.yaml", stream_metadata),
                          ("partition.const.h", partition_contract)):
        details[ext] = compare_values(*(contract((p / (BASE + "." + ext)).read_text())
                                        for p in (golden, candidate)))
    if syntax_evidence is not None:
        details["paired_syntax"] = paired_syntax(repo, golden, candidate, gm, cm, syntax_evidence)
    return details


def report(target, identity, details=None, error=None):
    details = details or {}
    failed = []
    for path in STATIC_CHECKS:
        value = details
        for key in path:
            value = value.get(key) if isinstance(value, dict) else None
        if not isinstance(value, dict) or value.get("match") is not True:
            failed.append("Static contract missing or mismatched: " + ".".join(path))
    static_semantics = not error and not failed
    paired = details.get("paired_syntax", {})
    syntax = isinstance(paired, dict) and paired.get("passed") is True
    # A coarse success flag cannot hide an absent/failed compiler or RTL arm.
    pairs = paired.get("pairs", {}) if isinstance(paired, dict) else {}
    for kind in ("sfc", "circt"):
        arm = pairs.get(kind, {}) if isinstance(pairs, dict) else {}
        arm = arm if isinstance(arm, dict) else {}
        for check in ("rtl", "driver"):
            if not isinstance(arm.get(check), dict) or arm[check].get("passed") is not True:
                syntax = False
    syntax = not error and syntax
    if not syntax:
        failed.append("Syntax: paired complete RTL lint and real driver-header checks must all pass for SFC and CIRCT; inspect paired_syntax command logs.")
    result = {"passed": bool(static_semantics and syntax), "syntax": bool(syntax),
              "semantic": bool(static_semantics), "static_semantics": bool(static_semantics),
              "verification_stage": "prebuild", "full_equivalence": False, "target": target,
              "input_identity": identity,
              "scope": "Pre-build syntax and selected static-semantic contracts of the exact F2 archived input pair: bridge/model annotations, public interfaces, driver ABI, platform defines, emitted-RTL XDC resolution, stream and partition metadata. Includes paired complete RTL lint and real driver-header syntax checks. Full behavioral equivalence, post-synthesis timing and FPGA runtime are separate mandatory Jasper/FireSim gates.",
              "details": details, "unresolved_obligations": failed,
              "deferred_obligations": DEFERRED_OBLIGATIONS}
    if error:
        result["error"] = error
        result["unresolved_obligations"].insert(0, error)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--golden-root", type=Path, required=True)
    parser.add_argument("--candidate-root", type=Path, required=True)
    args = parser.parse_args()
    identity, path = os.environ["CHIA_INPUT_IDENTITY"], Path(os.environ["CHIA_REPORT_PATH"])
    try:
        result = report(args.target, identity, compare(args.repo.resolve(), args.target, args.golden_root.resolve(), args.candidate_root.resolve(), path.parent / "paired-syntax"))
    except Exception as exc:
        result = report(args.target, identity, error=f"{type(exc).__name__}: {exc}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"passed": result["passed"], "verification_stage": "prebuild",
                      "target": args.target, "report": str(path), "error": result.get("error")}))


if __name__ == "__main__":
    main()
