#!/usr/bin/env python3
"""Reject profile/input/replay substitutions before comparing any RTL."""
import copy
import json
from pathlib import Path
import tempfile

import CompileContractProvenance as provenance
import CompareF2Semantics as semantics


def main():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        inputs = root / "inputs"
        inputs.mkdir()

        def file(path, value):
            path.write_text(value)
            return dict(path=str(path), bytes=path.stat().st_size, sha256=semantics.digest(path))

        native_input = file(inputs / "design.fir", "native input")
        sfc_input = file(inputs / "design.sfc.fir", "SFC input")
        annotations = file(inputs / "design.anno.json", "[]")
        compiler = file(root / "native-compiler", "compiler")
        jar = file(root / "compiler.jar", "jar")
        archive = file(root / "manifest.json", json.dumps(dict(
            status="complete", target="target",
            recipe={"PLATFORM_CONFIG": "WithFeature_BaseF2Config"},
            provenance={"execution_jars": {"compiler.jar": jar}},
            preMIDAS=dict(firrtl=sfc_input, annotations=annotations),
            artifacts={"design.fir": provenance.identity(native_input)},
            commands=[dict(phase="target_elaboration", exit_code=0,
                           argv=["generator", "--target-dir", str(inputs)])])))
        pair = {"name": "target"}
        receipts = {}
        profile = "WithMultiCycleRamModels_WithFeature_BaseF2Config"
        for label in ("golden", "candidate", "candidate_routes"):
            output = root / label
            output.mkdir()
            pair[label] = str(output)
            artifact = file(output / "generated.sv", "RTL")
            receipt = dict(target="target", returncode=0,
                           input=sfc_input if label == "golden" else native_input,
                           annotations=annotations, artifacts={"generated.sv": provenance.identity(artifact)})
            if label == "golden":
                receipt.update(original_manifest=archive, enabled_profile=profile, compiler_classpath=jar,
                               argv=["java", "-cp", jar["path"], "GoldenGateMain", "-i", sfc_input["path"],
                                     "-faf", annotations["path"], "-td", str(output), "-ggcs", profile])
            else:
                receipt.update(profile=profile, compiler=compiler,
                               argv=[compiler["path"], native_input["path"], "--annotation-file", annotations["path"],
                                     "--output-dir", str(output), "--platform-config", profile])
            receipts[label] = receipt

        def save(data):
            for label, receipt in data.items():
                (Path(pair[label]) / "receipt.json").write_text(json.dumps(receipt))

        def run(data):
            save(data)
            return provenance.validate(pair, lambda path: Path(path))

        valid, _ = run(receipts)
        assert valid["match"] and valid["snapshot_compiler_identical"]
        for label in ("golden", "candidate", "candidate_routes"):
            bad = copy.deepcopy(receipts)
            key = "enabled_profile" if label == "golden" else "profile"
            bad[label][key] = "WithMultiCycleRamModels_BaseF2Config"
            bad[label]["argv"][-1] = bad[label][key]
            try:
                run(bad)
            except ValueError as error:
                assert "wrong enabled profile" in str(error)
            else:
                raise AssertionError(f"mismatched {label} profile was accepted")
        bad = copy.deepcopy(receipts)
        other_input = file(inputs / "another.fir", "native input")
        bad["candidate"]["input"] = other_input
        bad["candidate"]["argv"][1] = other_input["path"]
        try:
            run(bad)
        except ValueError as error:
            assert "hash-bound shared elaboration artifact" in str(error)
        else:
            raise AssertionError("same-content unbound input from another elaboration was accepted")
        bad = copy.deepcopy(receipts)
        other_annotations = file(inputs / "other.anno.json", "[1]")
        bad["candidate_routes"]["annotations"] = other_annotations
        bad["candidate_routes"]["argv"][3] = other_annotations["path"]
        try:
            run(bad)
        except ValueError as error:
            assert "annotation input differs" in str(error)
        else:
            raise AssertionError("stale route replay with different annotations was accepted")
    print("Compiler provenance rejects all wrong profiles, unbound input and stale route replay identity")


if __name__ == "__main__":
    main()
