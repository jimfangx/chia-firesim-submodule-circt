#!/usr/bin/env python3
"""Fast archive contract checks; never elaborate targets or invoke Golden Gate."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import yaml

SPEC = importlib.util.spec_from_file_location("f2golden", Path(__file__).with_name("GenerateF2SFCGoldens.py"))
G = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(G)
REPO = Path(__file__).resolve().parents[6]


class GoldenArchiveContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.selected = yaml.safe_load((REPO / "automation/chia_firesim_circt/selected_config_build_recipes.yaml").read_text())
        self.sample = yaml.safe_load((REPO / "sims/firesim-staging/sample_config_build_recipes.yaml").read_text())
        self.recipes = G.validate_recipes(self.selected, self.sample)

    def test_fixed_recipe_derivation_and_platform_not_substituted(self):
        self.assertEqual(set(self.recipes), set(G.TARGETS))
        for target, field, value in [
            ("rocket", "PLATFORM", "xilinx_alveo_u250"),
            ("megaboomv3", "TARGET_CONFIG", "FireSimLargeBoomConfig"),
            ("shuttle", "TARGET_CONFIG", "FireSimGENV256D128ShuttleConfig"),
            ("saturn_shuttle", "platform_config_args", {"fpga_frequency": 50, "build_strategy": "TIMING"}),
        ]:
            invalid = copy.deepcopy(self.selected)
            invalid[G.TARGETS[target][0]][field] = value
            with self.assertRaises(ValueError):
                G.validate_recipes(invalid, self.sample)
        invalid = dict(self.selected, extra=self.selected[G.TARGETS["rocket"][0]])
        with self.assertRaises(ValueError):
            G.validate_recipes(invalid, self.sample)

    def test_commands_use_identical_input_and_exact_f2_platform(self):
        jars = {name: self.root / name for name in G.JAR_HASHES}
        for target, recipe in self.recipes.items():
            phases = G.commands(REPO, Path("/bin/java"), jars, recipe, self.root, 24, 1)
            first, second = (phase["argv"] for phase in phases)
            self.assertIn("firechip.chip:" + recipe["TARGET_CONFIG"], first)
            self.assertEqual(second[second.index("-ggcs") + 1], recipe["PLATFORM_CONFIG"])
            self.assertEqual(second[second.index("-i") + 1], str(self.root / "inputs" /
                             ("firechip.chip.FireSim." + recipe["TARGET_CONFIG"] + ".sfc.fir")))
            self.assertEqual(phases[1]["timing_scope"], "firesim_compiler")
            self.assertNotIn("vivado", " ".join(first + second).lower())

    def test_partial_attempt_is_never_completion_and_completed_target_is_immutable(self):
        partial = G.reserve_attempt(self.root, "rocket")
        (partial / "manifest.json").write_text('{"status":"failed"}')
        retry = G.reserve_attempt(self.root, "rocket")
        self.assertNotEqual(partial, retry)
        marker = partial.parent / "complete.json"
        marker.write_text('{"status":"complete"}')
        before = G.inventory(self.root)
        with self.assertRaisesRegex(ValueError, "immutable"):
            G.reserve_attempt(self.root, "rocket")
        self.assertEqual(before, G.inventory(self.root))

    def test_missing_sidecar_and_missing_resource_fail_closed(self):
        output = self.root / "sfc"
        output.mkdir()
        for name in G.REQUIRED_SFC:
            (output / name).write_text("[]" if name.endswith(".json") else "generated\n")
        missing = output / "FireSim-generated.const.vh"
        missing.unlink()
        with self.assertRaisesRegex(ValueError, "required artifact"):
            G.finalize_sfc(output, self.root)
        missing.write_text("header\n")
        (output / "firrtl_black_box_resource_files.f").write_text(str(self.root / "missing.v") + "\n")
        with self.assertRaisesRegex(ValueError, "required artifact"):
            G.finalize_sfc(output, self.root)

    def test_blackbox_append_retains_raw_rtl_and_archives_external_source(self):
        output = self.root / "sfc"
        output.mkdir()
        for name in G.REQUIRED_SFC:
            (output / name).write_text("[]" if name.endswith(".json") else "generated\n")
        # Scala legitimately emits an empty peer-to-peer map when no routes
        # exist; empty required headers/RTL remain invalid.
        (output / "FireSim-generated.peer2peer.const.yaml").write_text("")
        source = self.root / "blackbox.v"
        source.write_text("module blackbox; endmodule\n")
        (output / "firrtl_black_box_resource_files.f").write_text(str(source) + "\n")
        resources = G.finalize_sfc(output, self.root)
        self.assertEqual((output / "FireSim-generated.raw.sv").read_text(), "generated\n")
        self.assertIn(source.read_text(), (output / "FireSim-generated.sv").read_text())
        self.assertEqual(resources[0]["archived"]["sha256"], G.digest(source))
        self.assertTrue(Path(resources[0]["archived"]["path"]).is_relative_to(output))

    def test_recorded_tool_or_source_resource_drift_is_rejected(self):
        resource = self.root / "bootrom.img"
        resource.write_bytes(b"original")
        record = {"provenance": {"resources": [G.file_record(resource)]}}
        G.recheck_recorded_files(record)
        resource.write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "changed during generation"):
            G.recheck_recorded_files(record)

    def test_conda_java_without_release_still_binds_required_runtime(self):
        java = self.root / "bin/java"
        java.parent.mkdir()
        java.write_bytes(b"launcher")
        modules = self.root / "lib/modules"
        modules.parent.mkdir()
        modules.write_bytes(b"runtime image")
        library = self.root / "lib/server/libjvm.so"
        library.parent.mkdir()
        library.write_bytes(b"VM")
        with patch.object(G.subprocess, "run") as run:
            run.return_value.stdout, run.return_value.stderr = "", "test Java"
            record = G.java_runtime_record(java)
            self.assertIsNone(record["release_metadata"])
            self.assertEqual(len(record["runtime_files"]), 2)
            G.recheck_recorded_files(record)
            library.unlink()
            with self.assertRaises(OSError):
                G.java_runtime_record(java)

    def test_failed_compiler_keeps_commands_and_never_publishes_marker(self):
        jars = {name: self.root / name for name in G.JAR_HASHES}
        with patch.object(G, "run_phase", side_effect=RuntimeError("elaboration failed")):
            with self.assertRaisesRegex(RuntimeError, "elaboration failed"):
                G.generate_target(REPO, Path("/bin/java"), jars, self.root, "rocket",
                                  self.recipes["rocket"], {}, 24, 1)
        self.assertFalse((self.root / "rocket/complete.json").exists())
        manifest = json.loads(next((self.root / "rocket").glob("attempt-*/manifest.json")).read_text())
        self.assertEqual(manifest["status"], "failed")
        self.assertEqual(len(manifest["planned_commands"]), 2)


if __name__ == "__main__":
    unittest.main()
