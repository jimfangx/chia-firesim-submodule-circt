#!/usr/bin/env python3
"""Candidate receipt tests using small synthetic archives and a fake compiler.

No target elaboration, real compiler transformation, proof, FPGA or workload is run.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import yaml

SPEC = importlib.util.spec_from_file_location("candidate", Path(__file__).with_name("GenerateF2CIRCTCandidates.py"))
G = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(G)
REPO = Path(__file__).resolve().parents[6]


class CandidateContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        self.goldens = Path(self.temp.name) / "goldens"
        self.output = Path(self.temp.name) / "candidates"
        for relative in ("automation/chia_firesim_circt/verification_nodes.py",
                         "automation/chia_firesim_circt/verification.yaml",
                         "automation/chia_firesim_circt/selected_config_build_recipes.yaml",
                         "sims/firesim-staging/sample_config_build_recipes.yaml"):
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / relative, destination)
        for relative in ("sims/firesim/sim/goldengate-circt/CMakeLists.txt",
                         "sims/firesim/sim/goldengate-circt/lib/Native.cpp",
                         "sims/firesim/sim/make/goldengate.mk"):
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("fixture source\n")
        self.recipes = yaml.safe_load((self.repo / "automation/chia_firesim_circt/selected_config_build_recipes.yaml").read_text())
        self.compiler = self.repo / "fake-native-compiler"
        self.compiler.write_text(f"#!{sys.executable}\n" + '''import json, sys
from pathlib import Path
args = sys.argv[1:]
out = Path(args[args.index("--output-dir")+1])
assert "--compile-baseline" in args
assert args[args.index("--output-filename-base")+1] == "FireSim-generated"
'''+ "required = " + repr(G.REQUIRED) + '''
for name in required:
    (out / name).write_text("[]" if name.endswith(".json") else "module native; endmodule\\n")
(out / "FireSim-generated.defines.vh").write_text("")
(out / "FireSim-generated.implementation.xdc").write_text("")
(out / "resource.v").write_text("module blackbox; endmodule\\n")
(out / "firrtl_black_box_resource_files.f").write_text(str(out / "resource.v") + "\\n")
(out / "compiler-observed-argv.json").write_text(json.dumps(args))
print("fixture compiler only")
''')
        self.compiler.chmod(0o755)
        self.bootroms = []
        for index in range(3):
            resource = self.repo / f"bootrom-{index}.img"
            resource.write_bytes(b"fixture bootrom")
            self.bootroms.append(resource)
        jars = self.goldens / "toolchain"
        jars.mkdir(parents=True)
        for name in ("firechip.jar", "firesim-main.jar"):
            (jars / name).write_bytes(b"fixture jar")
        for target in G.TARGETS:
            attempt = self.goldens / target / "attempt-fixture"
            for directory in ("inputs", "sfc", "preMIDAS-resources"):
                (attempt / directory).mkdir(parents=True)
            recipe_name = G.SFC.TARGETS[target][0]
            recipe = self.recipes[recipe_name]
            stem = "firechip.chip.FireSim." + recipe["TARGET_CONFIG"]
            fir = attempt / "inputs" / (stem + ".sfc.fir")
            fir.write_text("circuit FireSim :\n")
            annos = attempt / "inputs" / (stem + ".anno.json")
            annos.write_text("[]")
            for name in G.SFC.REQUIRED_SFC:
                (attempt / "sfc" / name).write_text("[]" if name.endswith(".json") else "fixture output")
            (attempt / "sfc/FireSim-generated.peer2peer.const.yaml").write_text("")
            mapped = []
            for index, resource in enumerate(self.bootroms):
                archived = attempt / "preMIDAS-resources" / f"bootrom-{index}.img"
                shutil.copy2(resource, archived)
                mapped.append({"source": G.record(resource), "archived": G.record(archived)})
            manifest = {"status": "complete", "target": target, "recipe_name": recipe_name, "recipe": recipe,
                        "artifacts": G.inventory(attempt / "inputs"), "sfc_artifacts": G.inventory(attempt / "sfc"),
                        "resource_artifacts": G.inventory(attempt / "preMIDAS-resources"),
                        "preMIDAS": {"firrtl": G.record(fir), "annotations": G.record(annos),
                                     "annotation_resources": [], "bootrom_resources": mapped},
                        "sfc_resources": [], "provenance": {"execution_jars": {p.name: G.record(p) for p in jars.iterdir()}}}
            path = attempt / "manifest.json"
            G.write_json(path, manifest)
            G.write_json(attempt.parent / "complete.json", {"status": "complete", "target": target, "manifest": G.record(path)})

    def generate(self, target="rocket"):
        return G.generate_target(self.repo, self.goldens, self.output, self.compiler, target, 1)

    def validate(self, target="rocket"):
        return G.validate_receipt(self.repo, self.goldens, self.output, self.compiler, target, 1)

    def test_exact_four_commands_preserve_archived_inputs_platform_and_resources(self):
        before = G.inventory(self.goldens)
        for target in G.TARGETS:
            with self.subTest(target=target):
                receipt = self.generate(target)
                self.assertEqual(receipt["verification_claim"], G.CLAIM)
                manifest = json.loads(Path(receipt["manifest"]).read_text())
                args = manifest["command"]["argv"]
                recipe = self.recipes[G.SFC.TARGETS[target][0]]
                self.assertEqual(args[args.index("--platform-config") + 1], recipe["PLATFORM_CONFIG"])
                self.assertEqual(args[1], manifest["provenance"]["golden"]["preMIDAS"]["firrtl"]["path"])
                self.assertEqual(args[args.index("--annotation-file") + 1],
                                 manifest["provenance"]["golden"]["preMIDAS"]["annotations"]["path"])
                final = Path(receipt["manifest"]).parent / "circt/FireSim-generated.sv"
                self.assertIn("module blackbox", final.read_text())
                self.assertNotIn("module blackbox", final.with_name("FireSim-generated.raw.sv").read_text())
                self.assertEqual(self.validate(target), receipt)
                self.assertIn(str(self.compiler), receipt["circt_artifacts"])
                self.assertTrue(any(path.endswith("resource.v") for path in receipt["circt_artifacts"]))
                self.assertTrue(any(path.endswith(".implementation.xdc") for path in receipt["allowed_empty"]))
        self.assertEqual(G.inventory(self.goldens), before)

    def test_source_membership_content_binary_and_thread_profile_make_receipt_stale(self):
        self.generate()
        source = self.repo / "sims/firesim/sim/goldengate-circt/lib/Native.cpp"
        old = source.read_bytes()
        source.write_bytes(b"changed")
        self.assertIsNone(self.validate())
        source.write_bytes(old)
        added = source.with_name("Added.cpp")
        added.write_bytes(b"new source")
        self.assertIsNone(self.validate())
        added.unlink()
        self.assertIsNotNone(self.validate())
        self.compiler.write_text(self.compiler.read_text() + "\n# changed binary\n")
        self.assertIsNone(self.validate())
        if len(os.sched_getaffinity(0)) > 1:
            self.generate()
            self.assertIsNone(G.validate_receipt(self.repo, self.goldens, self.output, self.compiler, "rocket", 2))

    def test_changed_or_extra_or_missing_outputs_are_corrupt_not_cache_hits(self):
        receipt = self.generate()
        output = Path(receipt["manifest"]).parent / "circt"
        rtl = output / "FireSim-generated.sv"
        old = rtl.read_bytes()
        rtl.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "membership/hash"):
            self.validate()
        rtl.write_bytes(old)
        extra = output / "unexpected.txt"
        extra.write_text("extra")
        with self.assertRaisesRegex(ValueError, "membership/hash"):
            self.validate()
        extra.unlink()
        rtl.unlink()
        with self.assertRaisesRegex(ValueError, "membership/hash"):
            self.validate()

    def test_failed_and_missing_sidecar_attempts_never_publish_complete(self):
        self.compiler.write_text(self.compiler.read_text() + "\nraise SystemExit(9)\n")
        with self.assertRaisesRegex(RuntimeError, "exited 9"):
            self.generate()
        self.assertIsNone(self.validate())
        failed = json.loads(next((self.output / "rocket").glob("attempt-*/manifest.json")).read_text())
        self.assertEqual(failed["status"], "failed")
        self.assertEqual(failed["compiler_result"]["exit_code"], 9)
        self.assertTrue(failed["partial_circt_inventory"])
        self.compiler.write_text(self.compiler.read_text().replace("raise SystemExit(9)",
                                '(out / "FireSim-generated.const.h").unlink()'))
        with self.assertRaisesRegex(ValueError, "missing/empty"):
            self.generate()
        self.assertIsNone(self.validate())

    def test_new_identity_new_attempt_preserves_old_completed_attempt_and_failed_pointer(self):
        first = self.generate()
        old_path = Path(first["manifest"])
        old_tree = G.inventory(old_path.parent)
        marker = self.output / "rocket/complete.json"
        old_marker = marker.read_bytes()
        self.compiler.write_text(self.compiler.read_text() + "\nraise SystemExit(9)\n")
        with self.assertRaises(RuntimeError):
            self.generate()
        self.assertEqual(marker.read_bytes(), old_marker)
        self.assertIsNone(self.validate())
        self.compiler.write_text(self.compiler.read_text().replace("raise SystemExit(9)", "# corrected"))
        second = self.generate()
        self.assertNotEqual(first["manifest"], second["manifest"])
        self.assertEqual(G.inventory(old_path.parent), old_tree)
        self.assertNotEqual(marker.read_bytes(), old_marker)

    def test_original_resource_drift_and_platform_substitution_block_before_compile(self):
        self.bootroms[0].write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "hash/size mismatch"):
            self.generate()
        self.assertFalse(self.output.exists())
        self.bootroms[0].write_bytes(b"fixture bootrom")
        selected = self.repo / "automation/chia_firesim_circt/selected_config_build_recipes.yaml"
        recipes = copy.deepcopy(self.recipes)
        recipes[G.SFC.TARGETS["megaboomv3"][0]]["PLATFORM_CONFIG"] = "BaseXilinxAlveoU250Config"
        selected.write_text(yaml.safe_dump(recipes))
        with self.assertRaisesRegex(ValueError, "semantics"):
            self.generate()
        self.assertFalse(self.output.exists())

    def test_main_attempts_every_target_after_one_failure_and_reuses_valid_receipts(self):
        source = self.compiler.read_text()
        self.compiler.write_text(source + '\nif "MegaBoomV3Config" in args[0]: raise SystemExit(7)\n')
        argv = ["--repo", str(self.repo), "--golden-root", str(self.goldens), "--output-root", str(self.output),
                "--compiler", str(self.compiler), "--threads", "1"]
        self.assertEqual(G.main(argv), 1)
        result = json.loads((self.output / "generation-result.json").read_text())["targets"]
        self.assertEqual(set(result), set(G.TARGETS))
        self.assertEqual(result["megaboomv3"]["status"], "failed")
        for target in ("rocket", "shuttle", "saturn_shuttle"):
            self.assertEqual(result[target]["status"], "complete")
        marker = (self.output / "rocket/complete.json").read_bytes()
        self.assertEqual(G.main(argv + ["--target", "rocket"]), 0)
        self.assertTrue(json.loads((self.output / "generation-result.json").read_text())["targets"]["rocket"]["reused"])
        self.assertEqual((self.output / "rocket/complete.json").read_bytes(), marker)

    def test_main_reports_each_target_when_one_golden_is_missing(self):
        (self.goldens / "megaboomv3/complete.json").unlink()
        argv = ["--repo", str(self.repo), "--golden-root", str(self.goldens), "--output-root", str(self.output),
                "--compiler", str(self.compiler), "--threads", "1"]
        self.assertEqual(G.main(argv), 1)
        results = json.loads((self.output / "generation-result.json").read_text())["targets"]
        self.assertEqual(results["megaboomv3"]["status"], "failed")
        for target in ("rocket", "shuttle", "saturn_shuttle"):
            self.assertEqual(results[target]["status"], "complete")

    def test_output_symlink_failure_retains_failed_manifest(self):
        self.compiler.write_text(self.compiler.read_text() + '\n(out / "escape").symlink_to("/tmp")\n')
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.generate()
        failed = json.loads(next((self.output / "rocket").glob("attempt-*/manifest.json")).read_text())
        self.assertEqual(failed["status"], "failed")
        self.assertIn("symlink", failed["partial_inventory_error"])
        self.assertIsNone(self.validate())

    def test_missing_sfc_marker_threads_and_overlapping_roots_reject(self):
        (self.goldens / "rocket/complete.json").unlink()
        with self.assertRaisesRegex(ValueError, "no complete immutable SFC"):
            self.generate()
        with self.assertRaisesRegex(ValueError, "threads"):
            G.prerequisites(self.repo, self.goldens, self.compiler, 0, "shuttle")
        with self.assertRaisesRegex(ValueError, "separate"):
            G.generate_target(self.repo, self.goldens, self.goldens / "candidates", self.compiler, "shuttle", 1)


if __name__ == "__main__":
    unittest.main()
