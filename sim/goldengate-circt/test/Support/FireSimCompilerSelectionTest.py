#!/usr/bin/env python3
"""Exercise the production make rules without Scala, FPGA tools, or a rebuild."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


RULES = Path(__file__).resolve().parents[3] / "make/goldengate.mk"
NATIVE = r'''
import json
import os
from pathlib import Path
import sys

with open(os.environ['COMPILER_CALLS'], 'a') as calls:
    calls.write(json.dumps(sys.argv[1:]) + '\n')
if os.environ.get('FAIL_NATIVE') == '1':
    sys.exit('native compiler failed')
args = sys.argv[1:]
output = Path(args[args.index('--output-dir') + 1])
base = args[args.index('--output-filename-base') + 1]
output.mkdir(parents=True, exist_ok=True)
for suffix in ('.sv', '.memory-origins.json', '.synthesis.xdc',
               '.implementation.xdc', '.defines.vh', '.const.h', '.const.vh',
               '.partition.const.h', '.peer2peer.const.yaml'):
    (output / (base + suffix)).write_text('CIRCT' + suffix + '\n')
(output / 'post-bridge-extraction-all.json').write_text('[]\n')
blackbox = output / 'blackbox.v'
blackbox.write_text('BLACKBOX\n')
(output / 'firrtl_black_box_resource_files.f').write_text(str(blackbox) + '\n')
'''


class FireSimCompilerSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="firesim-compiler-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.generated = self.root / "generated"
        build = self.generated / "goldengate-circt-build"
        build.mkdir(parents=True)
        compiler = build / "goldengate-circt"
        compiler.write_text("#!" + sys.executable + "\n" + NATIVE)
        compiler.chmod(0o755)
        tools = self.root / "bin"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text("#!/bin/sh\nexit 0\n")
        cmake.chmod(0o755)
        self.calls = self.root / "calls.jsonl"
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        COMPILER_CALLS=str(self.calls))
        self.env.pop("GOLDENGATE_COMPILER", None)
        self.env.pop("MAKEFLAGS", None)
        self.env.pop("MFLAGS", None)
        for name in ("input.fir", "input.json"):
            (self.root / name).write_text("fixture\n")
        self.makefile = self.root / "Makefile"
        self.makefile.write_text(
            f"firesim_base_dir := {RULES.parent.parent}\n"
            f"GENERATED_DIR := {self.generated}\n"
            f"FIRRTL_FILE := {self.root}/input.fir\n"
            f"ANNO_FILE := {self.root}/input.json\n"
            "BASE_FILE_NAME := FireSim-generated\n"
            "PLATFORM_CONFIG := BaseF2Config\n"
            f"GOLDENGATE_CIRCT_PREFIX := {self.root}\n"
            "run_jar_scala_main = $(error Unexpected Scala compiler invocation)\n"
            f"include {RULES}\n")

    def make(self, *args, env=None):
        return subprocess.run(["make", "--no-print-directory", "-f", str(self.makefile),
                               *args], env=env or self.env, capture_output=True,
                              text=True, timeout=30)

    def test_default_replaces_stale_sfc_outputs_and_delivers_collateral(self):
        rtl = self.generated / "FireSim-generated.sv"
        rtl.write_text("stale SFC RTL\n")
        result = self.make("compile")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(rtl.read_text(), "CIRCT.sv\nBLACKBOX\n\n")
        args = json.loads(self.calls.read_text())
        self.assertIn("--compile-baseline", args)
        self.assertNotIn("--enable-synth-asserts", args)
        self.assertEqual(args[args.index('--platform-config') + 1], "BaseF2Config")
        for suffix in (".const.h", ".const.vh", ".partition.const.h",
                       ".peer2peer.const.yaml", ".memory-origins.json",
                       ".synthesis.xdc", ".implementation.xdc", ".defines.vh"):
            self.assertEqual((self.generated / ("FireSim-generated" + suffix)).read_text(),
                             "CIRCT" + suffix + "\n")
        self.assertEqual((self.generated / "post-bridge-extraction.json").read_text(), "[]\n")

    def test_sfc_and_unknown_compilers_are_rejected_before_building(self):
        for compiler in ("sfc", "unknown", ""):
            with self.subTest(compiler=compiler):
                result = self.make("compile", "GOLDENGATE_COMPILER=" + compiler)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("SFC is disabled", result.stderr)
                self.assertFalse(self.calls.exists())
        env = dict(self.env, GOLDENGATE_COMPILER="sfc")
        result = self.make("compile", env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SFC is disabled", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_native_failure_stops_build(self):
        result = self.make("compile", env=dict(self.env, FAIL_NATIVE="1"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native compiler failed", result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        self.assertFalse((self.generated / "FireSim-generated.sv").exists())

    def test_synthesized_assertions_configuration_reaches_native_compiler(self):
        result = self.make("compile", "PLATFORM_CONFIG=WithSynthAsserts_BaseF2Config")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--enable-synth-asserts", json.loads(self.calls.read_text()))

    def test_synthesized_assertions_explicit_option_reaches_native_compiler(self):
        result = self.make("compile", "GOLDENGATE_CIRCT_SYNTH_ASSERTS=enabled")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--enable-synth-asserts", json.loads(self.calls.read_text()))

    def test_invalid_synthesized_assertions_option_stops_build(self):
        result = self.make("compile", "GOLDENGATE_CIRCT_SYNTH_ASSERTS=yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GOLDENGATE_CIRCT_SYNTH_ASSERTS must be enabled or disabled", result.stderr)
        self.assertFalse(self.calls.exists())

    def test_standard_platform_fragments_reach_native_compiler(self):
        result = self.make("compile", "PLATFORM_CONFIG=WithAutoCounter_WithAutoILA_ILADepth2048_BaseF2Config")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(self.calls.read_text())
        self.assertIn("--enable-autocounter-csv", args)
        self.assertIn("--enable-autoila", args)
        self.assertEqual(args[args.index('--ila-depth') + 1], "2048")


if __name__ == "__main__":
    unittest.main()
