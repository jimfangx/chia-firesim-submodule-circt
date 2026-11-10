#!/usr/bin/env python3
"""Spelling-only CIRCT FIRRTL4 to immutable SFC1.2 parser adapter.

Only version, source locators, multiline types/expressions, connect and regreset
spellings change. The adapter rejects unsupported new statements. It neither
runs compiler passes nor alters clocks, widths, channel order or expressions.
"""
import re
import sys
from pathlib import Path


def split_commas(text):
    result, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in '({[<':
            depth += 1
        elif ch in ')}]>':
            depth -= 1
        elif ch == ',' and depth == 0:
            result.append(text[start:i].strip())
            start = i + 1
    result.append(text[start:].strip())
    return result


def normalize(text):
    records, pending = [], ''
    for raw in text.splitlines():
        line = re.sub(r' @\[.*\]$', '', raw)
        if not line.strip():
            continue
        pending = pending + ' ' + line.strip() if pending else line
        # CIRCT wraps bundle declarations and connect operands. Their braces,
        # parentheses and terminal comma make continuation explicit.
        if pending.count('{') != pending.count('}') or pending.count('(') != pending.count(')') or pending.rstrip().endswith(','):
            continue
        records.append(pending)
        pending = ''
    assert not pending, 'unterminated FIRRTL record'
    result = []
    for line in records:
        indent = line[:len(line)-len(line.lstrip())]
        body = line.strip()
        if body.startswith('public module ') or body.startswith('private module '):
            # Visibility controls CIRCT emission only; SFC syntax has no token.
            body = body.split(' ', 1)[1]
            line = indent + body
        if body.startswith('FIRRTL version '):
            result.append('FIRRTL version 1.2.0')
        elif body.startswith('connect '):
            fields = split_commas(body[len('connect '):])
            assert len(fields) == 2, body
            result.append(indent + fields[0] + ' <= ' + fields[1])
        elif body.startswith('regreset '):
            fields = split_commas(body[len('regreset '):])
            assert len(fields) == 4, body
            result.append(indent + 'reg ' + fields[0] + ', ' + fields[1] + ' with :')
            result.append(indent + '  reset => (' + fields[2] + ', ' + fields[3] + ')')
        else:
            assert not body.startswith(('propassign ', 'define ', 'contract ', 'layer ', 'layerblock ', 'option ')), body
            result.append(line)
    return '\n'.join(result) + '\n'


if __name__ == '__main__':
    assert len(sys.argv) == 3, 'usage: legacy-syntax native4.fir normalized1.fir'
    Path(sys.argv[2]).write_text(normalize(Path(sys.argv[1]).read_text()))
