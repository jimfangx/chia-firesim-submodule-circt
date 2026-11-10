#!/usr/bin/env python3
"""Focused tests for the fail-closed comparison adapter; no target compilation."""
import importlib.util
import contextlib
import copy
import io
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("compare_f2_semantics", HERE / "CompareF2Semantics.py")
M = importlib.util.module_from_spec(spec)
spec.loader.exec_module(M)


class ComparisonTests(unittest.TestCase):
    @staticmethod
    def matching_details():
        details = {}
        for path in M.STATIC_CHECKS:
            record = details
            for name in path:
                record = record.setdefault(name, {})
            record['match'] = True
        details['paired_syntax'] = {
            'passed': True,
            'pairs': {kind: {check: {'passed': True} for check in ('rtl', 'driver')}
                      for kind in ('sfc', 'circt')},
        }
        return details

    @staticmethod
    def clock_fixture(wrapper="target"):
        rtl = '''module F1Shim(input clock);
  Hub %s (.clock(clock));
endmodule
module Hub(input clock);
  BUFGCE buffer (.I(clock), .CE(1'b1), .O(gated));
endmodule
''' % wrapper
        xdc = '''# generated clock
create_generated_clock -name uart_clock,clock_500.0MHz -source [get_pins -of [get_clocks host_clock]] [get_pins -hierarchical -regexp .*WRAPPER/CL/firesim_top/%s/buffer/O.*] -divide_by 1
set_multicycle_path 2 -setup -from [get_clocks uart_clock,clock_500.0MHz] -to [get_clocks uart_clock,clock_500.0MHz]
set_multicycle_path 1 -hold  -from [get_clocks uart_clock,clock_500.0MHz] -to [get_clocks uart_clock,clock_500.0MHz]
''' % wrapper
        return xdc, rtl

    def test_implementation_clocks_resolve_each_actual_hierarchy(self):
        golden = M.implementation_clocks(*self.clock_fixture())
        candidate = M.implementation_clocks(*self.clock_fixture("target_FAMETop"))
        self.assertTrue(M.compare_implementation_clocks(golden, candidate)["match"])
        self.assertNotEqual(golden, candidate)
        record = next(iter(candidate.values()))
        self.assertEqual(record["resolved_hierarchy"], [
            {"parent": "F1Shim", "instance": "target_FAMETop", "module": "Hub"},
            {"parent": "Hub", "instance": "buffer", "module": "BUFGCE"}])
        self.assertIn("post-synthesis", M.compare_implementation_clocks(golden, candidate)["scope"])
        xdc, rtl = self.clock_fixture()
        second = xdc.replace("uart_clock,clock_500.0MHz", "reference").replace("/buffer/O", "/buffer2/O")
        rtl = rtl.replace("  BUFGCE buffer", "  BUFGCE buffer2 (.I(clock), .CE(1'b1), .O(other));\n  BUFGCE buffer")
        self.assertEqual(M.implementation_clocks(xdc + second, rtl),
                         M.implementation_clocks(second + xdc, rtl))

    def test_implementation_clocks_reject_unresolved_or_ambiguous_objects(self):
        xdc, rtl = self.clock_fixture()
        for bad_xdc, bad_rtl in (
                (xdc.replace("/target/", "/target_FAMETop/"), rtl),
                (xdc, rtl.replace("Hub target", "Missing target")),
                (xdc, rtl + "\nmodule Hub(input other); endmodule\n"),
                (xdc, rtl.replace("  Hub target", "  Hub target (.clock(clock));\n  Hub target")),
                (xdc, rtl.replace("BUFGCE buffer", "UnknownPrimitive buffer")),
                (xdc, rtl.replace(".O(gated)", ".Wrong(gated)")),
                (xdc, rtl.replace(".O(gated)", ".O(gated), .O(other)")),
                (xdc, rtl.replace("Hub target", "Hub #(.WIDTH(1)) target"))):
            with self.assertRaises(ValueError):
                M.implementation_clocks(bad_xdc, bad_rtl)

    def test_implementation_clocks_reject_unsupported_and_inconsistent_commands(self):
        xdc, rtl = self.clock_fixture()
        for bad in ("", xdc + "set_false_path -from [all_clocks]\n", xdc + xdc,
                    xdc.replace("-divide_by 1", "-divide_by 0"),
                    xdc.replace("-source [get_pins -of [get_clocks host_clock]]", "-source [get_ports clock]"),
                    xdc.replace("/buffer/O", "/buffer/I"),
                    xdc.replace("/buffer/O", "/buffer/O[0]"),
                    xdc.replace("set_multicycle_path 1 -hold", "set_multicycle_path 0 -hold"),
                    xdc.replace("-to [get_clocks uart_clock,clock_500.0MHz]", "-to [get_clocks other]")):
            with self.assertRaises(ValueError):
                M.implementation_clocks(bad, rtl)

    def test_implementation_clock_parameters_and_coverage_are_semantic(self):
        xdc, rtl = self.clock_fixture()
        golden = M.implementation_clocks(xdc, rtl)
        for changed in (xdc.replace("-divide_by 1", "-divide_by 2"),
                        xdc.replace("clock_500.0MHz", "clock_501.0MHz"),
                        xdc.replace("2 -setup", "3 -setup").replace("1 -hold", "2 -hold")):
            self.assertFalse(M.compare_implementation_clocks(golden, M.implementation_clocks(changed, rtl))["match"])
        self.assertFalse(M.compare_implementation_clocks(golden, {})["match"])
        result = M.report("rocket", "id", {"implementation.xdc": {"match": True},
                                           "paired_syntax": {"passed": True}})
        self.assertFalse(result["semantic"])
        self.assertFalse(result["passed"])

    def test_manager_stream_metadata_contract(self):
        a = 'TRACE_to_cpu_stream:\n  bufferBaseAddress: 0\n  bufferCapacity: 6144\n  bufferWidthBytes: 64\n'
        b = '# order and spacing only\nTRACE_to_cpu_stream: {bufferWidthBytes: 64, bufferCapacity: 6144, bufferBaseAddress: 0}\n'
        self.assertEqual(M.stream_metadata(a), M.stream_metadata(b))
        self.assertNotEqual(M.stream_metadata(a), M.stream_metadata(a.replace('6144', '6143')))
        self.assertEqual(M.stream_metadata('# no streams\n'), {})
        for text in (a + a, a.replace('bufferCapacity: 6144', 'bufferCapacity: 6144\n  bufferCapacity: 1'),
                     a.replace('6144', 'true'), a.replace('6144', '0'),
                     a.replace('bufferWidthBytes: 64', 'other: 64'), 'null', '[]'):
            with self.assertRaises(ValueError):
                M.stream_metadata(text)

    def test_selected_partition_header_is_unpartitioned(self):
        self.assertEqual(M.partition_contract('// widths\n/* no partition */\n'), 'unpartitioned')
        for text in ('#define PARTITION 1', 'struct Interface {};'):
            with self.assertRaisesRegex(ValueError, 'unpartitioned'):
                M.partition_contract(text)

    def test_incomplete_static_evidence_cannot_pass_prebuild(self):
        result = M.report("rocket", "identity", {"all_static_contracts_match": True})
        self.assertFalse(result["passed"])
        self.assertFalse(result["semantic"])
        self.assertFalse(result["syntax"])
        self.assertEqual(result["input_identity"], "identity")
        self.assertTrue(result["unresolved_obligations"])

    def test_complete_prebuild_checks_allow_independent_verification(self):
        result = M.report('shuttle', 'identity', self.matching_details())
        self.assertTrue(result['passed'])
        self.assertTrue(result['syntax'])
        self.assertTrue(result['semantic'])
        self.assertEqual(result['verification_stage'], 'prebuild')
        self.assertFalse(result['full_equivalence'])
        self.assertEqual(result['unresolved_obligations'], [])
        self.assertEqual(set(result['deferred_obligations']), {'jasper', 'firesim'})

    def test_each_static_mismatch_or_missing_check_blocks_prebuild(self):
        for path in M.STATIC_CHECKS:
            for missing in (False, True):
                with self.subTest(path=path, missing=missing):
                    details = self.matching_details()
                    record = details
                    for name in path[:-1]:
                        record = record[name]
                    if missing:
                        del record[path[-1]]
                    else:
                        record[path[-1]]['match'] = False
                    result = M.report('megaboomv3', 'identity', details)
                    self.assertTrue(result['syntax'])
                    self.assertFalse(result['semantic'])
                    self.assertFalse(result['passed'])
                    self.assertTrue(any('.'.join(path) in item for item in result['unresolved_obligations']))

    def test_both_complete_rtl_and_driver_syntax_checks_are_required(self):
        for kind in ('sfc', 'circt'):
            for check in ('rtl', 'driver'):
                with self.subTest(kind=kind, check=check):
                    details = self.matching_details()
                    details['paired_syntax']['pairs'][kind][check]['passed'] = False
                    result = M.report('saturn_shuttle', 'identity', details)
                    self.assertTrue(result['semantic'])
                    self.assertFalse(result['syntax'])
                    self.assertFalse(result['passed'])
        for pairs in (None, {}, {'sfc': None}):
            details = self.matching_details()
            details['paired_syntax']['pairs'] = pairs
            self.assertFalse(M.report('rocket', 'identity', details)['passed'])

    def test_cli_outputs_the_actual_prebuild_verdict(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'report.json'
            args = ['compare', '--repo', tmp, '--target', 'rocket',
                    '--golden-root', tmp, '--candidate-root', tmp]
            stdout = io.StringIO()
            with patch.object(sys, 'argv', args), \
                    patch.dict(os.environ, {'CHIA_INPUT_IDENTITY': 'identity', 'CHIA_REPORT_PATH': str(path)}), \
                    patch.object(M, 'compare', return_value=self.matching_details()), \
                    contextlib.redirect_stdout(stdout):
                M.main()
            self.assertTrue(json.loads(path.read_text())['passed'])
            self.assertTrue(json.loads(stdout.getvalue())['passed'])

    def test_error_never_signs_off(self):
        self.assertEqual(M.report("rocket", "identity", error="bad receipt")["error"], "bad receipt")
        self.assertFalse(M.report("rocket", "identity", error="bad receipt")["passed"])

    def test_port_spelling_and_order(self):
        a = M.ports("module T(input [7:0] x, y, output z); endmodule", "T")
        b = M.ports("module T(output z, input [7:0] y, input [7:0] x); endmodule", "T")
        self.assertEqual(a, b)
        self.assertNotEqual(a, M.ports("module T(input [6:0] x,y,output z); endmodule", "T"))

    def test_unrecognized_ports_reject(self):
        for text in ("module T(input signed [7:0] x); endmodule", "module T(input x,x); endmodule",
                     "module T(input x); endmodule module T(input y); endmodule"):
            with self.assertRaises(ValueError):
                M.ports(text, "T")

    def test_annotation_record_order_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / "a.json", Path(tmp) / "b.json"
            record = {"class": "C", "endpoints": ["a", "b"], "optional": None}
            a.write_text(json.dumps([record, {"class": "C", "x": 1}]))
            b.write_text(json.dumps([{"x": 1, "class": "C"}, record]))
            self.assertTrue(M.boundary(a, b, ("C",))["match"])
            b.write_text(json.dumps([dict(record, endpoints=["b", "a"]), {"class": "C", "x": 1}]))
            self.assertFalse(M.boundary(a, b, ("C",))["match"])
            b.write_text(json.dumps([record, record, {"class": "C", "x": 1}]))
            self.assertFalse(M.boundary(a, b, ("C",))["match"])
            b.write_text(json.dumps([{k: v for k, v in record.items() if k != "optional"}, {"class": "C", "x": 1}]))
            self.assertFalse(M.boundary(a, b, ("C",))["match"])

    def test_missing_and_malformed_annotations_reject(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "a.json"
            for value in ({}, [{}], ["C"]):
                p.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    M.annotations(p)
            p.write_text("[]")
            with self.assertRaises(ValueError):
                M.boundary(p, p, ("C",))

    def test_archive_inventory_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            attempt = root / "rocket/attempt"
            output = attempt / "circt"
            output.mkdir(parents=True)
            artifact = output / "a"
            artifact.write_text("hello")
            manifest = attempt / "manifest.json"
            value = {"status": "complete", "target": "rocket", "circt_inventory": {"a": {"bytes": 5, "sha256": M.digest(artifact)}}}
            manifest.write_text(json.dumps(value))
            marker = root / "rocket/complete.json"
            marker.write_text(json.dumps({"status": "complete", "target": "rocket", "manifest": {
                "path": str(manifest), "bytes": manifest.stat().st_size, "sha256": M.digest(manifest)}}))
            self.assertEqual(M.receipt(root, "rocket", "circt")[0], output)
            artifact.write_text("HELLO")
            with self.assertRaisesRegex(ValueError, "hash/size mismatch"):
                M.receipt(root, "rocket", "circt")
            artifact.write_text("hello")
            (output / "unexpected").write_text("extra")
            with self.assertRaisesRegex(ValueError, "membership"):
                M.receipt(root, "rocket", "circt")

    def test_recipe_and_input_pair_mismatch_reject(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "input"
            p.write_text("data")
            record = {"path": str(p), "bytes": 4, "sha256": M.digest(p)}
            pre = {"firrtl": record, "annotations": record}
            gm = {"recipe": {"PLATFORM": "f2"}, "recipe_name": "recipe", "preMIDAS": pre}
            cm = dict(gm, provenance={"golden": {"manifest": "bound-manifest", "preMIDAS": copy.deepcopy(pre)}})
            M.verify_pair(gm, cm, "bound-manifest", gm["recipe"], "recipe")
            bad = copy.deepcopy(cm)
            bad["recipe"]["PLATFORM"] = "u250"
            with self.assertRaisesRegex(ValueError, "exact selected F2 recipe"):
                M.verify_pair(gm, bad, "bound-manifest", gm["recipe"], "recipe")
            bad = copy.deepcopy(cm)
            bad["provenance"]["golden"]["manifest"] = "other-manifest"
            with self.assertRaisesRegex(ValueError, "different immutable golden manifest"):
                M.verify_pair(gm, bad, "bound-manifest", gm["recipe"], "recipe")
            bad = copy.deepcopy(cm)
            bad["provenance"]["golden"]["preMIDAS"]["firrtl"]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "pre-MIDAS firrtl identity differs"):
                M.verify_pair(gm, bad, "bound-manifest", gm["recipe"], "recipe")

    def test_cli_missing_archive_emits_fresh_failed_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.json"
            report.write_text('{"passed": true}')
            repo = HERE.parents[5]
            env = dict(os.environ, CHIA_REPORT_PATH=str(report), CHIA_INPUT_IDENTITY="fixture-id")
            run = subprocess.run([sys.executable, str(HERE / "CompareF2Semantics.py"),
                                  "--repo", str(repo), "--target", "rocket",
                                  "--golden-root", tmp, "--candidate-root", tmp],
                                 env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = json.loads(report.read_text())
            self.assertFalse(result["passed"])
            self.assertFalse(result["syntax"])
            self.assertFalse(result["semantic"])
            self.assertEqual(result["input_identity"], "fixture-id")
            self.assertIn("FileNotFoundError", result["error"])

    def test_driver_string_whitespace_is_semantic(self):
        helper = M.load_module("compare_f2_test_driver", HERE / "NICDriverHeaderCompare.py")
        header = '''#ifdef GET_CORE_CONSTRUCTOR
registry.add_widget(new core_t("a b", 1U));
#endif // GET_CORE_CONSTRUCTOR
static_assert(sizeof(A) == 8, "size");
static constexpr TargetConfig conf_target{true, "FireSim"};
'''
        a = M.driver_contract(header, helper)
        raw = header.replace('"a b"', 'R"ESC(a b)ESC"').replace("1U", "1ULL")
        self.assertEqual(a, M.driver_contract(raw, helper))
        self.assertNotEqual(a, M.driver_contract(header.replace('"a b"', '"ab"'), helper))
        self.assertNotEqual(a, M.driver_contract(header.replace('"a b"', '"b a"'), helper))

    def test_explicit_native_f2_ack_is_reported_separately_from_raw_abi(self):
        helper = M.load_module('f2_ack_driver', HERE / 'NICDriverHeaderCompare.py')
        header = '''#ifdef GET_MANAGED_STREAM_CONSTRUCTOR
registry.add_widget(new CPUManagedStreamWidget(simif, 0, args,
std::vector<CPUManagedStreams::StreamParameters>{CPUManagedStreams::StreamParameters("sink", 0, 8, 3072, 64)},
std::vector<CPUManagedStreams::StreamParameters>{CPUManagedStreams::StreamParameters("source", 0, 0, 6144, 64),CPUManagedStreams::StreamParameters("print", 524288, 4, 6144, 64)}));
#endif // GET_MANAGED_STREAM_CONSTRUCTOR
static_assert(sizeof(A) == 8, "size");
static constexpr TargetConfig conf_target{true, "FireSim"};
'''
        native = header.replace('6144, 64)', '6144, 64, true)')
        golden = M.driver_contract(header, helper)
        protocol = dict(version=2, platform='f2', protocol='source-count-write1-ack',
            liveHierarchyAndPredicatesValidated=True, controlWriteStrobesValidated=True,
            pciReadsNonDestructive=True,
            sources=[dict(name=name, index=index, countAddress=4*index, ackWriteValue=1, beatBytes=64)
                     for index, name in enumerate(('source', 'print'))])
        compare = lambda text: M.compare_driver_contracts(golden, M.driver_contract(text, helper),
                                                          protocol=protocol if text != header else None)
        result = compare(native)
        self.assertTrue(result['match'], result)
        self.assertFalse(result['raw_match'])
        self.assertTrue(result['base_abi_match'])
        extension = result['platform_protocol_extension']
        self.assertEqual(extension['profile'], 'native_f2_explicit_source_ack')
        self.assertEqual(extension['source_streams'], [dict(name='source', count_address=0), dict(name='print', count_address=4)])
        self.assertFalse(extension['formal_equivalence_claim'])
        self.assertFalse(extension['runtime_verified'])
        self.assertTrue(compare(header)['raw_match'])
        actual = M.driver_contract(native, helper)
        self.assertFalse(M.compare_driver_contracts(golden, actual)['match'])
        self.assertFalse(M.compare_driver_contracts(golden, golden, protocol=protocol)['match'])
        corrupt_protocols = [dict(protocol, version=True), dict(protocol, pciReadsNonDestructive=1),
                             dict(protocol, version=1), dict(protocol, controlWriteStrobesValidated=False),
                             dict(protocol, liveHierarchyAndPredicatesValidated=False),
                             dict(protocol, extra=True), dict(protocol, sources=list(reversed(protocol['sources'])))]
        for key, value in [('name', 'other'), ('countAddress', 12), ('ackWriteValue', 2),
                           ('index', True), ('beatBytes', 32)]:
            rows = copy.deepcopy(protocol['sources']); rows[0][key] = value
            corrupt_protocols.append(dict(protocol, sources=rows))
        for corrupt in corrupt_protocols:
            with self.subTest(protocol=corrupt):
                self.assertFalse(M.compare_driver_contracts(golden, actual, protocol=corrupt)['match'])
        for corrupt in [
                native.replace('3072, 64)', '3072, 64, true)'),
                native.replace('6144, 64, true)', '6144, 64, false)', 1),
                native.replace('6144, 64, true)', '6144, 64)', 1),
                native.replace('6144, 64, true)', '6144, 64, 1)'),
                native.replace('6144, 64, true)', '6144, 64, true, true)'),
                native.replace('"print", 524288, 4', '"print", 524288, 12'),
                native.replace('"source"', '"other"'),
                native.replace('3072, 64)', '3072, 32)'),
                native.replace('sizeof(A) == 8', 'sizeof(A) == 16'),
                native.replace('true, "FireSim"', 'false, "FireSim"'),
                native.replace('simif, 0, args', 'simif, 1, args')]:
            with self.subTest(corrupt=corrupt):
                self.assertFalse(compare(corrupt)['match'])


class SyntaxTests(unittest.TestCase):
    def test_paired_syntax_never_implies_semantic_signoff(self):
        details = {'paired_syntax': ComparisonTests.matching_details()['paired_syntax']}
        result = M.report("rocket", "identity", details)
        self.assertTrue(result["syntax"])
        self.assertFalse(result["semantic"])
        self.assertFalse(result["passed"])
        self.assertFalse(M.report("rocket", "identity", details, error="bad")["syntax"])

    def test_identical_define_arguments_and_rejection(self):
        self.assertEqual(M.define_arguments("// c\n`define B 0\n`define A\n"), ["+define+A", "+define+B=0"])
        for value in ("`define A 1\n`define A 1", "`include \"stub.v\"", "`define F(x) x", "`define A 1 + 2"):
            with self.assertRaises(ValueError):
                M.define_arguments(value)

    def test_resources_must_be_exact_final_append(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            raw = output / (M.BASE + '.raw.sv')
            raw.write_text('module F1Shim; endmodule\n')
            resource = output / 'resource.v'
            resource.write_text('module RealResource; endmodule\n')
            binding = M.file_record(resource)
            manifest = {'resources': [{'source': binding, 'archived': binding}]}
            rtl = output / (M.BASE + '.sv')
            rtl.write_bytes(raw.read_bytes() + resource.read_bytes() + b'\n')
            self.assertEqual(M.finalized_resources(output, manifest, 'circt'), [binding])
            rtl.write_bytes(raw.read_bytes())
            with self.assertRaisesRegex(ValueError, 'exactly the archived resource append'):
                M.finalized_resources(output, manifest, 'circt')

    def test_driver_probe_covers_real_entry_phases(self):
        source = M.driver_source(Path('/header.const.h'))
        self.assertIn('#include "core/widget_registry.h"', source)
        self.assertIn('#include "bridges/loadmem.h"', source)
        for phase in ('GET_INCLUDES', 'GET_SUBSTRUCT_CHECKS', 'GET_METASIM_INTERFACE_CONFIG',
                      'GET_CORE_CONSTRUCTOR', 'GET_MANAGED_STREAM_CONSTRUCTOR', 'GET_BRIDGE_CONSTRUCTOR'):
            self.assertIn('#define ' + phase + '\n#include "/header.const.h"', source)
            self.assertIn('#undef ' + phase, source)

    def test_real_small_syntax_fixtures_pass_and_fail_closed(self):
        repo = HERE.parents[5]
        verilator, cxx = repo / '.conda-env/bin/verilator', repo / '.conda-env/bin/c++'
        if not verilator.is_file() or not cxx.is_file():
            self.skipTest('local syntax tools unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            rtl, source = output / 'fixture.sv', output / 'fixture.cpp'
            rtl.write_text('module F1Shim(input clock, input enable, output gated); BUFGCE b(.I(clock),.CE(enable),.O(gated)); endmodule\n')
            source.write_text('static_assert(sizeof(unsigned char) == 1);\n')
            commands = M.syntax_commands(verilator, cxx, rtl,
                repo / 'sims/firesim/sim/midas/src/main/verilog/BUFGCE.v', ['+define+FIXTURE=1'],
                source, [], output / 'obj')
            for index, command in enumerate(commands):
                result = M.logged_command(command, repo, output / str(index), 30)
                self.assertTrue(result['passed'], Path(result['stderr']).read_text())
                self.assertEqual(json.loads((output / str(index) / 'command.json').read_text())['argv'], command)
            rtl.write_text('module F1Shim; MissingRealBlackbox missing(); endmodule\n')
            source.write_text('static_assert(false, "broken header contract");\n')
            for index, command in enumerate(commands):
                result = M.logged_command(command, repo, output / ('bad-' + str(index)), 30)
                self.assertFalse(result['passed'])
                self.assertNotEqual(result['returncode'], 0)
            self.assertFalse(any('bbox' in argument or 'blackbox' in argument for argument in commands[0]))

    def test_driver_probe_compiles_against_real_framework_declarations(self):
        repo = HERE.parents[5]
        cxx = repo / '.conda-env/bin/c++'
        if not cxx.is_file():
            self.skipTest('local C++ tool unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            header = output / 'fixture.const.h'
            header.write_text('#ifdef GET_METASIM_INTERFACE_CONFIG\nstatic constexpr TargetConfig conf_target{};\n#endif\n')
            source = output / 'driver.cpp'
            source.write_text(M.driver_source(header))
            includes = [repo / 'sims/firesim/sim/midas/src/main/cc',
                        repo / 'generators/firechip/bridgestubs/src/main/cc',
                        repo / '.conda-env/riscv-tools/include']
            _, command = M.syntax_commands('unused', cxx, 'unused', 'unused', [], source, includes, 'unused')
            result = M.logged_command(command, repo, output / 'driver', 30)
            self.assertTrue(result['passed'], Path(result['stderr']).read_text())

    def test_logged_timeout_and_missing_tool_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            result = M.logged_command([sys.executable, '-c', 'import time; time.sleep(10)'], output,
                                      output / 'timeout', 0.05)
            self.assertTrue(result['timed_out'])
            self.assertFalse(result['passed'])
            result = M.logged_command([str(output / 'missing')], output, output / 'missing-log', 1)
            self.assertFalse(result['passed'])
            self.assertIn('FileNotFoundError', result['error'])


if __name__ == "__main__":
    unittest.main()
