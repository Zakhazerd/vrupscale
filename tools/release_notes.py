#!/usr/bin/env python3
"""Prints CHANGELOG.md's section for a version ("## <version>" up to the next "## "): the release's
notes on GitHub, which the app also shows on its Update button."""
import sys

ver = sys.argv[1]
out, on = [], False
with open('CHANGELOG.md', encoding='utf-8') as f:
    for line in f.read().splitlines():
        if line.startswith('## ') or line.startswith('# '):
            if on:
                break
            on = line.strip() == '## ' + ver
            continue
        if on:
            out.append(line)
text = '\n'.join(out).strip()
print(text if text else 'vrupscale ' + ver)
