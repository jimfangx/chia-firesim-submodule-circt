#!/usr/bin/env python3
"""Exercise the production make rules without Scala, FPGA tools, or a rebuild."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


RULES = Path(__file__).resolve().parents[3] / "make/goldengate.mk"
NATIVE = r'''
import json
import os
from pathlib import Path
import sys

script_at_start = Path(__file__).read_bytes()
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
               '.partition.const.h', '.peer2peer.const.yaml', '.f2-cpu-stream-protocol.json'):
    if os.environ.get('PARTIAL_NATIVE') == '1' and suffix == '.const.h':
        continue
    data = json.dumps({'source': 'CIRCT' + suffix}) if suffix.endswith('.json') else 'CIRCT' + suffix
    (output / (base + suffix)).write_text(data + '\n')
(output / 'post-bridge-extraction-all.json').write_text('[]\n')
blackbox = output / 'blackbox.v'
blackbox.write_text('BLACKBOX\n')
(output / 'firrtl_black_box_resource_files.f').write_text(str(blackbox) + '\n')
if '--enable-autoila' in args:
    (output / (base + '.ila.ipgen.tcl')).write_text('ILA\n')
if os.environ.get('NATIVE_NINJA'):
    import subprocess
    subprocess.run([os.environ['NINJA_EXECUTABLE'], '-f', os.environ['NATIVE_NINJA']],
                   cwd=Path(os.environ['NATIVE_NINJA']).parent, check=True)
if os.environ.get('NATIVE_DELAY'):
    import time
    time.sleep(float(os.environ['NATIVE_DELAY']))
if Path(__file__).read_bytes() != script_at_start:
    sys.exit('compiler executable changed during invocation')
'''

CMAKE = r'''
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

args = sys.argv[1:]
assert not any(name in os.environ for name in ('CXXFLAGS', 'CFLAGS', 'LDFLAGS'))
assert 'FRONTEND_ENV_READY' not in os.environ

def wait_for_frontend():
    path = os.environ.get('WAIT_FOR_FRONTEND')
    deadline = time.monotonic() + 8
    while path and not Path(path).exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if path:
        assert Path(path).exists(), 'frontend did not overlap bootstrap'

def interrupted(signum, frame):
    raise SystemExit(128 + signum)

signal.signal(signal.SIGTERM, interrupted)
with open(os.environ['CMAKE_CALLS'], 'a') as log:
    log.write(json.dumps(args) + '\n')
if os.environ.get('FAIL_BUILD') == '1':
    wait_for_frontend()
    sys.exit('native build failed')
if args[0] == '--build':
    root = Path(args[1])
    active = root / '.fake-active'
    fd = os.open(active, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    try:
        if os.environ.get('BUILD_STARTED'):
            Path(os.environ['BUILD_STARTED']).write_text(str(os.getpid()))
        wait_for_frontend()
        if os.environ.get('BOOTSTRAP_NINJA'):
            subprocess.run([os.environ['NINJA_EXECUTABLE'], '-f', os.environ['BOOTSTRAP_NINJA']],
                           cwd=Path(os.environ['BOOTSTRAP_NINJA']).parent, check=True)
        time.sleep(float(os.environ.get('BUILD_DELAY', '0')))
        src = Path(os.environ['FAKE_COMPILER'])
        dest = root / 'goldengate-circt'
        if not dest.exists() or dest.read_bytes() != src.read_bytes():
            shutil.copy2(src, dest)
    finally:
        os.close(fd)
        active.unlink()
else:
    root = Path(args[args.index('-B') + 1])
    root.mkdir(parents=True, exist_ok=True)
    (root / '.fake-active').unlink(missing_ok=True)
    (root / 'CMakeCache.txt').write_text('configured\n')
'''


class FireSimCompilerSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="firesim-compiler-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.generated = self.root / "generated"
        self.generated.mkdir()
        self.compiler = self.root / 'fake-compiler'
        self.compiler.write_text("#!" + sys.executable + "\n" + NATIVE)
        self.compiler.chmod(0o755)
        tools = self.root / "bin"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text("#!" + sys.executable + "\n" + CMAKE)
        cmake.chmod(0o755)
        self.calls = self.root / "calls.jsonl"
        self.cmake_calls = self.root / 'cmake-calls.jsonl'
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                        COMPILER_CALLS=str(self.calls), CMAKE_CALLS=str(self.cmake_calls),
                        FAKE_COMPILER=str(self.compiler))
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
            "PLATFORM := f2\n"
            f"GOLDENGATE_CIRCT_PREFIX := {self.root}\n"
            f"GOLDENGATE_CIRCT_BUILD_ROOT := {self.root}/native-build\n"
            f"GOLDENGATE_CIRCT_PYTHON := {sys.executable}\n"
            "run_jar_scala_main = $(error Unexpected Scala compiler invocation)\n"
            f"include {RULES}\n"
            f"driver_binary := {self.root}/driver\n"
            "$(driver_binary): $(header)\n"
            "\t@printf 'driver\\n' >> $(driver_binary)\n"
            "driver: $(driver_binary)\n"
            "$(driver_binary): export CXXFLAGS := -Idriver-sdk -O2\n"
            "$(driver_binary): export LDFLAGS := -ldriver-only\n"
            "replace-rtl: compile driver\n")

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
        self.assertNotIn("--enable-multicycle-ram-models", args)
        self.assertEqual(args[args.index('--platform-config') + 1], "BaseF2Config")
        for suffix in (".const.h", ".const.vh", ".partition.const.h",
                       ".peer2peer.const.yaml", ".memory-origins.json",
                       ".synthesis.xdc", ".implementation.xdc", ".defines.vh"):
            expected = json.dumps({'source': 'CIRCT' + suffix}) if suffix.endswith('.json') else 'CIRCT' + suffix
            self.assertEqual((self.generated / ("FireSim-generated" + suffix)).read_text(), expected + '\n')
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

    def test_diagnostics_separate_bootstrap_compilation_and_reuse(self):
        result = self.make('replace-rtl')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for phase in ('native-configure', 'native-build', 'design-compile'):
            self.assertEqual(result.stdout.count('[goldengate-circt] start phase=' + phase), 1)
            self.assertRegex(result.stdout, 'finish phase=' + phase + r' exit=0 wall_seconds=\d+\.\d+')
        self.assertRegex(result.stdout, r'argv phase=design-compile .*goldengate-circt-[a-f0-9]{64} ')
        reused = self.make('driver')
        self.assertEqual(reused.returncode, 0, reused.stdout + reused.stderr)
        self.assertIn('Reusing complete CIRCT RTL and driver collateral:', reused.stdout)
        self.assertNotIn('start phase=design-compile', reused.stdout)
        self.assertNotIn('start phase=native-configure', reused.stdout)
        self.assertIn('start phase=native-build', reused.stdout)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
    def test_native_failure_stops_build(self):
        result = self.make("compile", env=dict(self.env, FAIL_NATIVE="1"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native compiler failed", result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        self.assertFalse((self.generated / "FireSim-generated.sv").exists())

    def test_replace_rtl_then_driver_reuses_one_complete_compile(self):
        for target in ('replace-rtl', 'driver'):
            result = self.make(target)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        self.assertEqual((self.root / 'driver').read_text(), 'driver\n')
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertEqual(sum(command[0] == '-S' for command in commands), 1)
        header = self.generated / 'FireSim-generated.const.h'
        before = header.stat().st_mtime_ns
        result = self.make('driver')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(header.stat().st_mtime_ns, before)

    def test_real_fpga_delivery_rule_then_driver_compiles_once(self):
        platforms = self.root / 'platforms'
        board = platforms / 'f2/aws-fpga-firesim-f2/hdk/cl/developer_designs'
        template = board / 'cl_firesim'
        (template / 'design').mkdir(parents=True)
        (template / 'driver').mkdir()
        (template / 'build/scripts').mkdir(parents=True)
        (template / 'build/scripts/synth_cl_firesim.tcl').write_text('# fixture\n')
        with self.makefile.open('a') as stream:
            stream.write('DESIGN := Model\nname_quintuplet := fixture\nf2 := $(driver_binary)\n'
                         'include ' + str(RULES.with_name('fpga.mk')) + '\n')
        for goal in ('replace-rtl', 'driver'):
            result = self.make(goal, 'platforms_dir=' + str(platforms))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        self.assertEqual((self.root / 'driver').read_text(), 'driver\n')
        design = board / 'cl_fixture/design'
        for suffix in ('.sv', '.defines.vh', '.synthesis.xdc', '.implementation.xdc'):
            self.assertEqual((design / ('FireSim-generated' + suffix)).read_bytes(),
                             (self.generated / ('FireSim-generated' + suffix)).read_bytes())
        self.assertEqual((board / 'cl_fixture/driver/Model-f2').read_text(), 'driver\n')

    def test_content_changes_invalidate_even_with_preserved_timestamps(self):
        result = self.make('compile')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in ('input.fir', 'input.json'):
            path = self.root / name
            stamp = path.stat()
            path.write_text('changed ' + name)
            os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            result = self.make('compile')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)

    def test_compiler_binary_and_options_invalidate(self):
        for option in ('', 'GOLDENGATE_CIRCT_SYNTH_ASSERTS=enabled'):
            result = self.make('compile', *([option] if option else []))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with self.compiler.open('a') as stream:
            stream.write('\n# rebuilt native compiler\n')
        result = self.make('compile', 'GOLDENGATE_CIRCT_SYNTH_ASSERTS=enabled')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)

    def test_missing_or_corrupt_output_recompiles(self):
        self.assertEqual(self.make('compile').returncode, 0)
        (self.generated / 'FireSim-generated.const.h').unlink()
        self.assertEqual(self.make('driver').returncode, 0)
        path = self.generated / 'FireSim-generated.sv'
        stamp = path.stat()
        path.write_text('stale SFC RTL\n')
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertEqual(self.make('compile').returncode, 0)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)

    def test_partial_and_failed_rebuilds_have_no_complete_receipt(self):
        for variable in ('PARTIAL_NATIVE', 'FAIL_NATIVE', 'FAIL_BUILD'):
            with self.subTest(variable=variable):
                self.assertEqual(self.make('compile').returncode, 0)
                result = self.make('compile', 'GOLDENGATE_CIRCT_SYNTH_ASSERTS=enabled',
                                   env=dict(self.env, **{variable: '1'}))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.generated / '.goldengate-circt-complete.json').exists())

    def test_disabled_autoila_removes_previous_ip(self):
        self.assertEqual(self.make('compile', 'GOLDENGATE_CIRCT_AUTOILA=enabled').returncode, 0)
        self.assertTrue(list(self.generated.glob('*.ipgen.tcl')))
        self.assertEqual(self.make('compile', 'GOLDENGATE_CIRCT_AUTOILA=disabled').returncode, 0)
        self.assertFalse(list(self.generated.glob('*.ipgen.tcl')))

    def test_concurrent_recipes_share_one_locked_build_and_pin_binary(self):
        env = dict(self.env, BUILD_DELAY='0.15', NATIVE_DELAY='0.15')
        commands = [["make", "--no-print-directory", "-f", str(self.makefile), 'compile',
                     'GENERATED_DIR=' + str(self.root / ('recipe-' + str(index)))]
                    for index in range(3)]
        children = [subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True) for command in commands]
        for child in children:
            stdout, stderr = child.communicate(timeout=30)
            self.assertEqual(child.returncode, 0, stdout + stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertEqual(sum(command[0] == '-S' for command in commands), 1)
        for index in range(3):
            receipt = json.loads((self.root / ('recipe-' + str(index)) /
                                  '.goldengate-circt-complete.json').read_text())
            self.assertTrue(Path(receipt['request']['compiler']['binary']['path']).name.startswith(
                'goldengate-circt-'))

    def test_concurrent_requests_for_same_recipe_compile_once(self):
        env = dict(self.env, NATIVE_DELAY='0.15')
        command = ['make', '--no-print-directory', '-f', str(self.makefile), 'compile']
        children = [subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True) for _ in range(3)]
        for child in children:
            stdout, stderr = child.communicate(timeout=30)
            self.assertEqual(child.returncode, 0, stdout + stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)

    def test_rebuild_does_not_mutate_an_active_compiler(self):
        command = ['make', '--no-print-directory', '-f', str(self.makefile), 'compile']
        child = subprocess.Popen(command + ['GENERATED_DIR=' + str(self.root / 'first')],
                                 env=dict(self.env, NATIVE_DELAY='0.4'),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 10
        while not self.calls.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(self.calls.exists())
        with self.compiler.open('a') as stream:
            stream.write('\n# new compiler while first recipe is running\n')
        other = self.make('compile', 'GENERATED_DIR=' + str(self.root / 'second'))
        self.assertEqual(other.returncode, 0, other.stdout + other.stderr)
        stdout, stderr = child.communicate(timeout=30)
        self.assertEqual(child.returncode, 0, stdout + stderr)
        identities = [json.loads((self.root / name / '.goldengate-circt-complete.json').read_text())
                      ['request']['compiler']['binary'] for name in ('first', 'second')]
        self.assertNotEqual(identities[0]['sha256'], identities[1]['sha256'])
        self.assertNotEqual(identities[0]['path'], identities[1]['path'])

    def test_input_change_during_compile_is_not_published(self):
        child = subprocess.Popen(['make', '--no-print-directory', '-f', str(self.makefile), 'compile'],
                                 env=dict(self.env, NATIVE_DELAY='0.2'),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 10
        while not self.calls.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(self.calls.exists())
        (self.root / 'input.fir').write_text('changed while compiling\n')
        stdout, stderr = child.communicate(timeout=30)
        self.assertNotEqual(child.returncode, 0, stdout + stderr)
        self.assertIn('inputs changed during compilation', stderr)
        self.assertFalse((self.generated / '.goldengate-circt-complete.json').exists())
        self.assertFalse((self.generated / 'FireSim-generated.sv').exists())

    def test_external_resource_and_backend_receipt_changes_invalidate(self):
        self.assertEqual(self.make('compile').returncode, 0)
        (self.generated / 'circt-ingestion/blackbox.v').write_text('changed resource\n')
        self.assertEqual(self.make('compile').returncode, 0)
        receipt = self.generated / '.goldengate-circt-complete.json'
        data = json.loads(receipt.read_text())
        data['backend'] = 'sfc'
        receipt.write_text(json.dumps(data))
        self.assertEqual(self.make('compile').returncode, 0)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)

    def test_native_build_configuration_and_root_overrides_are_isolated(self):
        for args in ([], ['GOLDENGATE_CIRCT_CXXFLAGS=-fno-omit-frame-pointer'],
                     ['GOLDENGATE_CIRCT_BUILD_ROOT=' + str(self.root / 'isolated-build')]):
            result = self.make('compile', *args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 3)
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertEqual(sum(command[0] == '-S' for command in commands), 3)

    def test_same_prefix_toolchain_change_with_preserved_mtime_rebuilds(self):
        header = self.root / 'include/circt/Installed.h'
        header.parent.mkdir(parents=True)
        header.write_text('old SDK\n')
        self.assertEqual(self.make('compile').returncode, 0)
        stamp = header.stat()
        header.write_text('new SDK\n')
        os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        result = self.make('compile')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertEqual(sum(command[0] == '-S' for command in commands), 2)

    def test_receipt_cannot_substitute_another_destination(self):
        self.assertEqual(self.make('compile').returncode, 0)
        path = self.generated / '.goldengate-circt-complete.json'
        receipt = json.loads(path.read_text())
        header = self.generated / 'FireSim-generated.const.h'
        other = self.root / 'other-header'
        other.write_bytes(header.read_bytes())
        receipt['outputs'][header.name]['path'] = str(other)
        header.write_text('stale SFC header\n')
        path.write_text(json.dumps(receipt))
        self.assertEqual(self.make('driver').returncode, 0)
        self.assertEqual(header.read_text(), 'CIRCT.const.h\n')
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)

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


    def test_multicycle_ram_configuration_aliases_reach_native_compiler(self):
        for fragment in ('WithMultiCycleRamModels', 'MCRams'):
            with self.subTest(fragment=fragment):
                self.calls.unlink(missing_ok=True)
                result = self.make('compile', 'PLATFORM_CONFIG=' + fragment + '_BaseF2Config')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads(self.calls.read_text()).count('--enable-multicycle-ram-models'), 1)

    def test_multicycle_ram_explicit_option_reaches_native_compiler(self):
        result = self.make('compile', 'GOLDENGATE_CIRCT_MULTICYCLE_RAM_MODELS=enabled')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--enable-multicycle-ram-models', json.loads(self.calls.read_text()))

    def test_multicycle_ram_explicit_disabled_remains_disabled(self):
        result = self.make('compile', 'GOLDENGATE_CIRCT_MULTICYCLE_RAM_MODELS=disabled')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('--enable-multicycle-ram-models', json.loads(self.calls.read_text()))

    def test_invalid_multicycle_ram_option_stops_build(self):
        result = self.make('compile', 'GOLDENGATE_CIRCT_MULTICYCLE_RAM_MODELS=yes')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('GOLDENGATE_CIRCT_MULTICYCLE_RAM_MODELS must be enabled or disabled', result.stderr)
        self.assertFalse(self.calls.exists())


if __name__ == "__main__":
    unittest.main()
