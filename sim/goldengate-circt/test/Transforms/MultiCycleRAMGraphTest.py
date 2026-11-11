#!/usr/bin/env python3
"""Exercise the production graph/materializer on a portable selected-RAM graph."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile

compiler, graph_test, fixtures = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='multicycle-ram-graph-') as temp:
    root = Path(temp)
    source = (fixtures / 'SRAMModelChannels.fir').read_text()
    # The SFC timing model supports separate read and write ports; the common
    # extraction fixture also tests readwrite ports as an unsupported case.
    source = source.replace('      readwriter => rw\n', '')
    source = re.sub(r'    ram\.rw[^\n]*\n', '', source)
    (root / 'input.fir').write_text(source)
    annotations = json.loads((fixtures / 'SRAMModelChannels.json').read_text())
    annotations += [{"class": "firrtl.transforms.DontTouchAnnotation",
                     "target": "~FAMETop|Parent/ram:ram>r.addr",
                     "test.context": "selected-ram-port"},
                    {"class": "test.NoTargetPayload",
                     "description": "~FAMETop|Parent/ram:ram>r.addr",
                     "constructor": {"target": "~FAMETop|ram>r_addr"}}]
    (root / 'input.json').write_text(json.dumps(annotations))
    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result

    run([compiler, root / 'input.fir', '--annotation-file', root / 'input.json',
         '--output-dir', root / 'prepared', '--analyze-sram-channels'])
    # The standalone handoff is FIRRTL 1.2; drop the exporter's public-module
    # visibility keyword before reimporting (the hierarchy is unchanged).
    exported = (root / 'prepared/post-sram-channels.fir').read_text()
    (root / 'prepared.fir').write_text(exported.replace('public module ', 'module '))
    run([compiler, root / 'prepared.fir', '--annotation-file',
         root / 'prepared/post-sram-channels-all.json', '--output-dir', root / 'imported'])
    result = run([graph_test, '--ram-model-input', root / 'imported/input.mlir'])
    print(result.stdout, end='')
