#!/usr/bin/env python3
"""Prints the README's "What changed in <version>" section: the release's notes on GitHub, which
the app also shows on its Update button."""
import sys

ver = sys.argv[1]
out, on = [], False
with open('README.md', encoding='utf-8') as f:
    for line in f.read().splitlines():
        if line.startswith('### ') or line.startswith('## '):
            if on:
                break
            on = line.strip() == '### What changed in ' + ver
            continue
        if on:
            out.append(line)
text = '\n'.join(out).strip()
print(text if text else 'vrupscale ' + ver)
