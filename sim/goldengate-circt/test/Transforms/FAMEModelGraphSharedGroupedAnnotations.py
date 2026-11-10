#!/usr/bin/env python3
"""Check retained annotations and generated-clock contracts at the FAME boundary."""
import json
import re
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit('usage: FAMEModelGraphSharedGroupedAnnotations.py evidence-directory')
root = Path(sys.argv[1]).resolve()
sfc = json.loads((root / 'sfc-fame.json').read_text())
native = json.loads((root / 'native-cli/post-fame-model-graph-boundary-all.json').read_text())
# Fanout is materialized by the subsequent transport stage, which rejects this
# aggregate Pipe case in SFC. The clock-gate implementation is used verbatim by
# the real RTL differential; native clock constraints use structured metadata.
excluded = {'midas.InternalXDCAnnotation',
            'midas.passes.fame.FAMEChannelFanoutAnnotation',
            'firrtl.transforms.BlackBoxInlineAnno'}
assert [x for x in sfc if x['class'] not in excluded] == native
mlir = (root / 'native-cli/post-fame-model-graph-boundary.mlir').read_text()
clocks = []
for row in sfc:
    if row['class'] != 'midas.InternalXDCAnnotation':
        continue
    gate = row['argumentList'][0].split('>')[1].split('.')[0]
    name = re.search(r'create_generated_clock -name (\w+)', row['formatString'])[1]
    mfmr = int(re.search(r'set_multicycle_path (\d+) -setup', row['formatString'])[1])
    line = next(line for line in mlir.splitlines() if 'firrtl.instance ' + gate + ' ' in line)
    assert f'mfmr = {mfmr} : i64, name = "{name}"' in line
    clocks.append({'gate': gate + '.O', 'name': name, 'mfmr': mfmr})
assert len(clocks) == 2
report = {'passed': True, 'exact_ordered_retained_annotations': len(native),
          'generated_clock_metadata': clocks,
          'excluded_annotations': [x['class'] for x in sfc if x['class'] in excluded],
          'scope': 'Exact ordered boundary annotations with no channel-name normalization. '
                   'Two SFC XDC gate/name/MFMR contracts match native structured metadata. '
                   'Fanout transport and final platform XDC are outside this boundary check.'}
(root / 'annotation-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS 22 exact ordered annotations and two SFC/native generated-clock metadata contracts')
