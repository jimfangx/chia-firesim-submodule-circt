#!/usr/bin/env python3
"""Exercise production frontend/bootstrap overlap, joining and error cleanup."""
import json
import os
from pathlib import Path
import signal
import shlex
import shutil
import subprocess
import sys
import time
import unittest

import FireSimCompilerSelectionTest as fixtures


HELPER = fixtures.RULES.parent.parent / 'goldengate-circt/tools/firesim_compile.py'
FIRECHIP = fixtures.RULES.parents[4] / 'generators/firechip/chip/src/main/makefrag/firesim/build.mk'
FRONTEND = r'''
from pathlib import Path
import os
import subprocess
import sys
import time

if os.environ.get('EXPECT_ENV_READY') == '1':
    assert os.environ.get('FRONTEND_ENV_READY') == '1'
started = Path(os.environ['FRONTEND_STARTED'])
started.write_text(str(os.getpid()))
if os.environ.get('FRONTEND_CHILD_PID'):
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
    Path(os.environ['FRONTEND_CHILD_PID']).write_text(str(child.pid))
if os.environ.get('EXPECT_BOOTSTRAP') == '1':
    deadline = time.monotonic() + 8
    while not Path(os.environ['BUILD_STARTED']).exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert Path(os.environ['BUILD_STARTED']).exists(), 'bootstrap did not overlap frontend'
if os.environ.get('WAIT_FOR_WORKER'):
    deadline = time.monotonic() + 8
    while not Path(os.environ['WAIT_FOR_WORKER']).exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert Path(os.environ['WAIT_FOR_WORKER']).exists()
if os.environ.get('FAIL_FRONTEND'):
    sys.exit(19)
time.sleep(float(os.environ.get('FRONTEND_DELAY', '0.05')))
if os.environ.get('STAGING_OUTPUT'):
    name = '.'.join(os.environ[key] for key in ('MODEL_PACKAGE', 'MODEL', 'CONFIG'))
    output = Path(os.environ['STAGING_OUTPUT']) / name
    output.mkdir(parents=True, exist_ok=True)
    assert os.environ['EXTRA_CHISEL_OPTIONS'] == ''
    for suffix in ('.fir', '.anno.json', '.d'):
        (output / (name + suffix)).write_text('frontend fixture\n')
'''


class FireSimBootstrapOverlapTest(unittest.TestCase):
    setUp = fixtures.FireSimCompilerSelectionTest.setUp
    make = fixtures.FireSimCompilerSelectionTest.make

    def frontend(self):
        path = self.root / 'frontend.py'
        path.write_text(FRONTEND)
        return path

    def environment(self, **extra):
        return dict(self.env, BUILD_STARTED=str(self.root / 'bootstrap-started'),
                    FRONTEND_STARTED=str(self.root / 'frontend-started'),
                    WAIT_FOR_FRONTEND=str(self.root / 'frontend-started'),
                    **extra)

    def command(self):
        return [sys.executable, str(HELPER), '--source-dir',
                str(fixtures.RULES.parent.parent / 'goldengate-circt'),
                '--build-root', str(self.root / 'native-build'), '--circt-prefix',
                str(self.root), '--with-frontend', '--', sys.executable, str(self.frontend())]

    def wait_file(self, name, child):
        path = self.root / name
        deadline = time.monotonic() + 10
        while not path.exists() and child.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(path.exists(), str(path))
        return int(path.read_text())

    def assert_not_running(self, pid):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            stat = Path('/proc') / str(pid) / 'stat'
            if not stat.exists() or stat.read_text().split(') ', 1)[1].startswith('Z'):
                return
            time.sleep(0.01)
        self.fail('owned child still running: ' + str(pid))

    def test_independent_jobs_overlap_and_both_join(self):
        result = subprocess.run(self.command(), env=self.environment(EXPECT_BOOTSTRAP='1',
                                BUILD_DELAY='0.15'), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for phase in ('native-bootstrap', 'firechip-frontend'):
            self.assertIn('[goldengate-circt] argv phase=' + phase, result.stdout)
        self.assertRegex(result.stdout,
                         r'finish phase=bootstrap-frontend-overlap exit=0 wall_seconds=\d+\.\d+')
        self.assertTrue((self.root / 'bootstrap-started').exists())
        self.assertTrue((self.root / 'frontend-started').exists())
        self.assertTrue(list((self.root / 'native-build').glob('*/goldengate-circt-*')))
        self.assertFalse(self.calls.exists(), 'bootstrap must not compile a design')
        self.assertFalse(list((self.root / 'native-build').glob('*/.fake-active')))

    def test_frontend_failure_stops_bootstrap_and_its_own_descendants(self):
        env = self.environment(EXPECT_BOOTSTRAP='1', BUILD_DELAY='20', FAIL_FRONTEND='1',
                               FRONTEND_CHILD_PID=str(self.root / 'frontend-child'))
        result = subprocess.run(self.command(), env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
        for filename in ('bootstrap-started', 'frontend-started', 'frontend-child'):
            self.assert_not_running(int((self.root / filename).read_text()))
        self.assertFalse(self.calls.exists())
        self.assertFalse(list(self.generated.glob('.goldengate-circt-complete.json')))

    def test_bootstrap_failure_stops_frontend(self):
        env = self.environment(FAIL_BUILD='1', FRONTEND_DELAY='20')
        result = subprocess.run(self.command(), env=env, capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('native build failed', result.stderr)
        self.assert_not_running(int((self.root / 'frontend-started').read_text()))
        self.assertFalse(self.calls.exists())

    def test_interrupt_stops_and_reaps_both_owned_groups(self):
        child = subprocess.Popen(self.command(), env=self.environment(
                                 EXPECT_BOOTSTRAP='1', BUILD_DELAY='20', FRONTEND_DELAY='20'),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        bootstrap = self.wait_file('bootstrap-started', child)
        frontend = self.wait_file('frontend-started', child)
        child.send_signal(signal.SIGTERM)
        stdout, stderr = child.communicate(timeout=15)
        self.assertEqual(child.returncode, 128 + signal.SIGTERM, stdout + stderr)
        self.assert_not_running(bootstrap)
        self.assert_not_running(frontend)

    def staging_makefile(self, native):
        chipyard = self.root / 'chipyard'
        staging = chipyard / 'sims/firesim-staging'
        staging.mkdir(parents=True)
        (chipyard / 'env.sh').write_text('export FRONTEND_ENV_READY=1\n')
        script = self.frontend()
        (staging / 'Makefile').write_text('all:\n\t@' + sys.executable + ' ' + str(script) + '\n')
        content = self.makefile.read_text()
        include = 'include ' + str(fixtures.RULES)
        prefix = ('chipyard_dir := ' + str(chipyard) + '\n'
                  'long_name := firechip.chip.FireSim.FixtureConfig\n'
                  'DESIGN := FireSim\nDESIGN_PACKAGE := firechip.chip\n'
                  'TARGET_CONFIG := FixtureConfig\nTARGET_CONFIG_PACKAGE := firechip.chip\n'
                  'TARGET_SBT_PROJECT := fixture\n'
                  '.PHONY: firesim_target_symlink_hook\nfiresim_target_symlink_hook:\n'
                  'include ' + str(FIRECHIP) + '\n')
        if native:
            content = content.replace(include, prefix + include)
        else:
            content = content.replace(include, 'GOLDENGATE_COMPILER := sfc\n' + prefix)
        self.makefile.write_text(content + '\n.PHONY: frontend\nfrontend: $(FIRRTL_FILE)\n')
        return dict(STAGING_OUTPUT=str(staging / 'generated-src'), EXPECT_ENV_READY='1')

    def test_production_firechip_rule_overlaps_and_reuses_between_manager_steps(self):
        env = self.environment(EXPECT_BOOTSTRAP='1', BUILD_DELAY='0.1', **self.staging_makefile(True))
        for goal in ('replace-rtl', 'driver'):
            result = self.make(goal, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.calls.read_text().splitlines()), 1)
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertEqual(sum(command[0] == '-S' for command in commands), 1)
        self.assertEqual((self.root / 'driver').read_text(), 'driver\n')

    def test_non_native_frontend_keeps_ordinary_staging_behavior(self):
        result = self.make('frontend', env=self.environment(**self.staging_makefile(False)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.cmake_calls.exists())
        self.assertFalse(self.calls.exists())
        self.assertTrue((self.root / 'input.fir').is_symlink())

    def ninja_worker(self):
        worker = self.root / 'ninja-worker.py'
        marker = self.root / 'ninja-worker.json'
        worker.write_text(
            'import json,os,signal,time\nfrom pathlib import Path\n'
            'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
            'Path(' + repr(str(marker)) + ').write_text(json.dumps({"pid":os.getpid(),'
            '"pgid":os.getpgrp(),"parent":os.getppid()}))\n'
            'time.sleep(60)\n')
        build = self.root / 'worker.ninja'
        build.write_text('rule worker\n  command = ' + shlex.quote(sys.executable) + ' ' +
                         shlex.quote(str(worker)) + '\nbuild output: worker\ndefault output\n')
        return build, marker

    def wait_worker(self, marker, child):
        deadline = time.monotonic() + 10
        while not marker.exists() and child.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(marker.exists(), str(marker))
        return json.loads(marker.read_text())

    def assert_reaped(self, pid):
        self.assertFalse((Path('/proc') / str(pid)).exists(), 'owned worker was not reaped')

    @unittest.skipUnless(shutil.which('ninja'), 'requires Ninja')
    def test_actual_ninja_worker_group_is_killed_and_reaped_on_frontend_failure(self):
        build, marker = self.ninja_worker()
        env = self.environment(EXPECT_BOOTSTRAP='1', FAIL_FRONTEND='1',
                               BOOTSTRAP_NINJA=str(build), NINJA_EXECUTABLE=shutil.which('ninja'),
                               WAIT_FOR_WORKER=str(marker))
        result = subprocess.run(self.command(), env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
        worker = json.loads(marker.read_text())
        self.assertNotEqual(worker['pgid'], int((self.root / 'bootstrap-started').read_text()))
        self.assert_reaped(worker['pid'])
        self.assertFalse(self.calls.exists())

    @unittest.skipUnless(shutil.which('ninja'), 'requires Ninja')
    def test_direct_bootstrap_cancellation_reaps_ninja_and_recovers_cache(self):
        build, marker = self.ninja_worker()
        command = self.command()
        command = command[:command.index('--with-frontend')] + ['--bootstrap-only']
        env = dict(self.env, BOOTSTRAP_NINJA=str(build), NINJA_EXECUTABLE=shutil.which('ninja'))
        child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        worker = self.wait_worker(marker, child)
        child.terminate()
        stdout, stderr = child.communicate(timeout=15)
        self.assertNotEqual(child.returncode, 0, stdout + stderr)
        self.assert_reaped(worker['pid'])
        result = self.make('compile')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = [json.loads(line) for line in self.cmake_calls.read_text().splitlines()]
        self.assertTrue(any('--clean-first' in command for command in commands))

    @unittest.skipUnless(shutil.which('ninja'), 'requires Ninja')
    def test_direct_compilation_cancellation_reaps_workers_before_unlocking(self):
        build, marker = self.ninja_worker()
        command = self.command()
        command = command[:command.index('--with-frontend')] + [
            '--input', str(self.root / 'input.fir'), '--annotations', str(self.root / 'input.json'),
            '--output-dir', str(self.generated), '--base', 'FireSim-generated',
            '--platform', 'f2', '--platform-config', 'BaseF2Config']
        env = dict(self.env, NATIVE_NINJA=str(build), NINJA_EXECUTABLE=shutil.which('ninja'))
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()) if unrelated.poll() is None else None)
        child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        worker = self.wait_worker(marker, child)
        child.terminate()
        stdout, stderr = child.communicate(timeout=15)
        self.assertNotEqual(child.returncode, 0, stdout + stderr)
        self.assert_reaped(worker['pid'])
        self.assertIsNone(unrelated.poll(), 'unrelated process must not be signalled')
        self.assertFalse((self.generated / '.goldengate-circt-complete.json').exists())
        result = self.make('compile')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
