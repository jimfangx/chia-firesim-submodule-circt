#!/usr/bin/env python3
"""Check the native CHIRRTL/sidecar annotation handoff without losing repeats."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

COMPILER = Path(sys.argv.pop(1)).resolve()


class NativeAnnotationsTest(unittest.TestCase):
    def check_import(self, embedded, sidecar, count, mirrored):
        with tempfile.TemporaryDirectory(prefix='native-chirrtl-') as temp:
            root = Path(temp)
            header = '' if embedded is None else '%[' + json.dumps(embedded) + ']'
            (root / 'input.fir').write_text(
                'FIRRTL version 3.3.0\ncircuit Top :' + header +
                '\n  module Top :\n    input clock : Clock\n')
            (root / 'input.json').write_text(json.dumps(sidecar))
            result = subprocess.run([str(COMPILER), str(root / 'input.fir'),
                                     '--annotation-file', str(root / 'input.json'),
                                     '--output-dir', str(root / 'output')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / 'output/analysis.json').read_text())
            self.assertEqual(report['annotation_count'], count)
            self.assertEqual(report['deduplicated_embedded_annotations'], mirrored)
            self.assertEqual(report['annotation_classes']['example.Keep'], count)

    def test_matching_embedded_and_sidecar_consumed_once(self):
        a = {'class': 'example.Keep', 'payload': [1, 'two']}
        self.check_import([a], [a], 1, True)

    def test_intentional_repeats_in_mirrored_arrays_preserved(self):
        a = {'class': 'example.Keep', 'payload': 'repeat'}
        self.check_import([a, a], [a, a], 2, True)

    def test_distinct_sidecar_payload_preserved(self):
        self.check_import([{'class': 'example.Keep', 'payload': 1}],
                          [{'class': 'example.Keep', 'payload': 2}], 2, False)

    def test_embedded_only_repeats_preserved(self):
        a = {'class': 'example.Keep'}
        self.check_import([a, a], [], 2, False)

    def test_sidecar_only_repeats_preserved(self):
        a = {'class': 'example.Keep'}
        self.check_import(None, [a, a], 2, False)


if __name__ == '__main__':
    unittest.main()
