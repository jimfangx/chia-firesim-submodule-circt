#!/usr/bin/env python3
"""Exercise production recursive driver Make rules with real C++ fixtures.

The small sources and empty SDK/GMP link archives isolate build scheduling;
this is not an FPGA driver behavior test. A compiler barrier records overlap
directly instead of relying on elapsed-time speed assertions.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SIM = Path(__file__).resolve().parents[3]
REAL_CXX = shutil.which("c++")
REAL_AR = shutil.which("ar")
REAL_MAKE = shutil.which("make")

COMPILER_WRAPPER = r'''
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

state_path = Path(os.environ["DRIVER_TEST_STATE"])

def state_change(event=None, code=None):
    with state_path.open("r+") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        state = json.load(stream)
        if event:
            state["active"] += 1 if event == "start" else -1
            state["maximum"] = max(state["maximum"], state["active"])
            state["events"].append({"event": event, "pid": os.getpid(),
                                     "argv": sys.argv[1:], "returncode": code})
            stream.seek(0)
            stream.truncate()
            json.dump(state, stream)
            stream.flush()
        return state

state_change("start")
code = 124
try:
    if os.environ.get("DRIVER_TEST_REQUIRE_OVERLAP") == "1":
        deadline = time.monotonic() + 10
        while state_change()["maximum"] < 2:
            if time.monotonic() >= deadline:
                raise RuntimeError("recursive driver make did not schedule concurrent compilers")
            time.sleep(0.005)
    code = subprocess.run([os.environ["DRIVER_TEST_REAL_CXX"], *sys.argv[1:]]).returncode
finally:
    state_change("end", code)
raise SystemExit(code)
'''


@unittest.skipUnless(REAL_CXX and REAL_AR and REAL_MAKE, "requires C++, ar and GNU Make")
class DriverParallelismTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="firesim-driver-parallel-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sim = self.root / "sim"
        self.cc = self.sim / "midas/src/main/cc"
        self.cc.mkdir(parents=True)
        shutil.copy2(SIM / "midas/src/main/cc/Makefile", self.cc / "Makefile")
        (self.cc / "rtlsim").mkdir()
        for name in ("verilator", "vcs", "xcelium"):
            source = SIM / "midas/src/main/cc/rtlsim" / ("Makefrag-" + name)
            shutil.copy2(source, self.cc / "rtlsim" / source.name)
        for directory in ("core", "bridges"):
            (self.cc / directory).mkdir()
        functions = {"bridges/alpha.cc": ("fixture_alpha", 1),
                     "bridges/beta.cc": ("fixture_beta", 2),
                     "core/gamma.cc": ("fixture_gamma", 3),
                     "simif_f2.cc": ("fixture_platform", 4),
                     "entry.cc": ("fixture_entry", 5)}
        for name, (function, value) in functions.items():
            (self.cc / name).write_text("int " + function + "() { return " + str(value) + "; }\n")
        names = [value[0] for value in functions.values()] + ["fixture_driver"]
        (self.cc / "core/api.h").write_text("\n".join("int " + name + "();" for name in names) + "\n")
        (self.cc / "main.cc").write_text(
            '#include "FireSim-generated.const.h"\n#include "core/api.h"\n'
            'int main() { return (' + " + ".join(name + "()" for name in names) +
            ' == EXPECTED_TOTAL) ? 0 : 1; }\n')
        driver = self.root / "driver.cc"
        driver.write_text("int fixture_driver() { return 6; }\n")
        generated = self.root / "generated"
        generated.mkdir()
        header = generated / "FireSim-generated.const.h"
        header.write_text("#define EXPECTED_TOTAL 21\n")
        self.binary = self.root / "output/Fixture-f2"
        build = self.root / "output/build"
        build.mkdir(parents=True)
        sdk = self.root / "platforms/f2/aws-fpga-firesim-f2/sdk/userspace/lib"
        sdk.mkdir(parents=True)
        for archive in (sdk / "libfpga_mgmt.a", build / "libgmp.a"):
            subprocess.run([REAL_AR, "rcs", str(archive)], check=True, capture_output=True)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        wrapper = self.tools / "cxx"
        wrapper.write_text("#!" + sys.executable + "\n" + COMPILER_WRAPPER)
        wrapper.chmod(0o755)
        # Exercise the default CPU discovery reproducibly, even on single-CPU CI.
        nproc = self.tools / "nproc"
        nproc.write_text("#!/bin/sh\nprintf '4\\n'\n")
        nproc.chmod(0o755)
        self.state = self.root / "state.json"
        self.state.write_text(json.dumps({"active": 0, "maximum": 0, "events": []}))
        self.environment = dict(os.environ, CXX=str(wrapper),
            PATH=str(self.tools) + os.pathsep + os.environ["PATH"],
            DRIVER_TEST_STATE=str(self.state), DRIVER_TEST_REAL_CXX=REAL_CXX)
        for variable in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "DRIVER_BUILD_JOBS"):
            self.environment.pop(variable, None)
        makefile = self.root / "Makefile"
        makefile.write_text("\n".join([
            ".NOTPARALLEL:", "PLATFORM := f2", "DESIGN := Fixture",
            "BASE_FILE_NAME := FireSim-generated", "firesim_base_dir := " + str(self.sim),
            "OUTPUT_DIR := " + str(self.root / "output"), "header := " + str(header),
            "simif_dir := " + str(self.cc), "DRIVER_CC := " + str(driver),
            "midas_cc := $(wildcard $(simif_dir)/*.cc $(simif_dir)/core/*.cc $(simif_dir)/bridges/*.cc)",
            "midas_h := $(wildcard $(simif_dir)/core/*.h $(simif_dir)/bridges/*.h)",
            "CXX := " + str(wrapper), "include " + str(SIM / "make/driver.mk"), ""]))

    def build(self, jobs=None, require_overlap=False):
        environment = dict(self.environment, DRIVER_TEST_REQUIRE_OVERLAP="1" if require_overlap else "0")
        argv = [REAL_MAKE, "--no-print-directory", "-f", str(self.root / "Makefile"), "driver"]
        if jobs is not None:
            argv.append("DRIVER_BUILD_JOBS=" + str(jobs))
        return subprocess.run(argv, cwd=self.root, env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)

    def statistics(self):
        return json.loads(self.state.read_text())

    def test_default_parallel_and_incremental_noop(self):
        result = self.build(require_overlap=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("--jobs=4", result.stdout)
        state = self.statistics()
        self.assertGreaterEqual(state["maximum"], 2)
        self.assertLessEqual(state["maximum"], 4)
        self.assertEqual(state["active"], 0)
        subprocess.run([str(self.binary)], check=True)
        timestamp = self.binary.stat().st_mtime_ns
        result = self.build(require_overlap=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.statistics()["events"], state["events"])
        self.assertEqual(self.binary.stat().st_mtime_ns, timestamp)

    def test_serial_override(self):
        result = self.build(jobs=1)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.statistics()["maximum"], 1)
        subprocess.run([str(self.binary)], check=True)

    def test_parallel_override(self):
        result = self.build(jobs=2, require_overlap=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.statistics()["maximum"], 2)

    def test_alveo_macro_parallel_override(self):
        makefile = self.root / "Makefile"
        makefile.write_text(makefile.read_text().replace(
            "PLATFORM := f2", "PLATFORM := xilinx_alveo_u250"))
        shutil.copy2(self.cc / "simif_f2.cc", self.cc / "simif_xilinx_alveo_u250.cc")
        self.binary = self.root / "output/Fixture-xilinx_alveo_u250"
        result = self.build(jobs=2, require_overlap=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.statistics()["maximum"], 2)
        subprocess.run([str(self.binary)], check=True)

    def test_compile_failure_propagates(self):
        (self.cc / "bridges/beta.cc").write_text("#error driver_parallel_fixture_failure\n")
        result = self.build(require_overlap=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("driver_parallel_fixture_failure", result.stdout)
        self.assertFalse(self.binary.exists())
        self.assertEqual(self.statistics()["active"], 0)


if __name__ == "__main__":
    unittest.main()
