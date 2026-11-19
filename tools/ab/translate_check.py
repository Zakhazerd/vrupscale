#!/usr/bin/env python3
# tools/ab/translate_check.py <dir> <key> [x y z yaw]: what tools/ab/translate.sh's run left behind,
# against the scene of test/text_scene.inc and the eyes of test/stub/openvr_stub.c, with the head
# where the stub had it (STUB_HEAD; default 0 1.6 0 0). Prints ok / FAIL lines and "RESULT: N passed,
# M failed"; exits 1 on a failure.
import json
import math
import os
import re
import struct
import sys

D, KEY = sys.argv[1], sys.argv[2]
HEAD = [float(v) for v in sys.argv[3:7]] if len(sys.argv) >= 7 else [0.0, 1.6, 0.0, 0.0]
passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
    print('  %s %s' % ('ok  ' if ok else 'FAIL', what))


def read(name):
    p = os.path.join(D, name)
    return open(p, encoding='utf-8', errors='replace').read().splitlines() if os.path.exists(p) else []


# ---- the scene (test/text_scene.inc) and the stub's eyes and head
W = H = 1400.0
L0, R0, TOP, BOTTOM, EYE_X = -1.3, 1.1, 1.2, -1.25, -0.032   # the left eye; raw "bottom" 1.2 is the top edge
COS, SIN = math.cos(math.radians(HEAD[3])), math.sin(math.radians(HEAD[3]))   # the head turned to the left by that
WELCOME, FEED, EXIT = 'Welcome to the quiet world', 'Please do not feed the sleeping avatars', 'Exit this way'
SIGNS = {WELCOME: (620, 480, 2.0, (28, 60, 110)), FEED: (800, 860, 4.0, (236, 232, 220)), EXIT: (800, 860, 4.0, (140, 30, 30))}


def pixel(pos):   # a point of the standing space in the left eye's picture, and its depth
    d = [pos[i] - HEAD[i] for i in range(3)]
    h = (COS * d[0] - SIN * d[2], d[1], SIN * d[0] + COS * d[2])   # in the head's space
    depth = -h[2]
    tx, ty = (h[0] - EYE_X) / depth, h[1] / depth
    return (tx - L0) / (R0 - L0) * W, (TOP - ty) / (TOP - BOTTOM) * H, depth


# ---- what went to "DeepL"
reqs = [json.loads(l) for l in read('deepl.log') if l.strip()]
posts = [r for r in reqs if r['method'] == 'POST']
texts = [t for r in posts for t in (r['body'] or {}).get('text', [])]
check(sorted(texts) == sorted(SIGNS), 'each sign\'s text went to DeepL once (also when a sign came back), and nothing else: %s' % texts)
check(bool(reqs) and all(r['auth'] == 'DeepL-Auth-Key ' + KEY for r in reqs), 'every request carries the key in its header (%d requests)' % len(reqs))
check(bool(posts) and all(r['path'] == '/v2/translate' and r['body'].get('source_lang') == 'EN' and r['body'].get('target_lang') == 'JA' and
                          set(r['body']) == {'text', 'source_lang', 'target_lang'} for r in posts),
      'the requests hold the texts and the two languages only')
check(all(r['path'] in ('/v2/translate', '/v2/usage') for r in reqs) and any(r['path'] == '/v2/usage' for r in reqs),
      'the month\'s usage is asked for; no other address is called')

# ---- what the app told "SteamVR"
out = read('out.txt')
phase = {int(m.group(1)): int(m.group(2)) for m in (re.match(r'phase (\d) at (\d+)', l) for l in out) if m}
stub = read('stub.log')
events = []   # (tick, call, label, rest)
for l in stub:
    m = re.match(r'(\d+) (\w+) label (\d+)\s*(.*)', l)
    if m:
        events.append((int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)))
created = [int(m.group(1)) for m in (re.search(r'CreateOverlay key=zakhazerd\.vrupscale\.label\.(\d+) ', l) for l in stub) if m]
tlog = read('translate.log')
news = []   # (text, label, depth, depth told from the two eyes), in order
for l in tlog:
    m = re.search(r'  new (\d+) "(.*)" at ([\d.]+) m( \(guessed\))?, ', l)
    if m:
        news.append((m.group(2).replace('/', ' '), int(m.group(1)), float(m.group(3)), m.group(4) is None))
check(len(phase) == 5, 'the scene went through its five phases')
check(len(created) >= 2 and len(set(created)) == len(created), 'an overlay was made for each label, once (%s)' % created)
if len(phase) != 5:
    print('RESULT: %d passed, %d failed' % (passed, failed + 1))
    sys.exit(1)

captures = [l for l in tlog if re.search(r' capture \d+: \d+ x \d+', l)]
both = [l for l in captures if 'both eyes' in l]
lost = [l for l in tlog if 'overwritten' in l or 'not read' in l or 'no head pose' in l]
on_seconds = (phase[4] - phase[0]) / 1000.0
check(len(captures) >= 2.4 * (on_seconds - 3) and len(both) == len(captures) and len(lost) <= 2,
      'about three captures a second are read, both eyes each time (%d in %.0f s, %d lost)' % (len(captures), on_seconds, len(lost)))
ms = sorted(float(m.group(1)) for m in (re.search(r'read in (\d+) ms', l) for l in captures) if m)
seen = sorted(float(m.group(1)) for m in (re.search(r'seen (\d+) ms after it was taken, in memory (\d+) ms later', l) for l in tlog) if m)
mem = sorted(float(m.group(2)) for m in (re.search(r'seen (\d+) ms after it was taken, in memory (\d+) ms later', l) for l in tlog) if m)
if ms and seen:
    print('       a capture is seen %.0f ms after it was taken, in memory %.0f ms later, read in %.0f ms (medians; at most %.0f, %.0f, %.0f)' %
          (seen[len(seen) // 2], mem[len(mem) // 2], ms[len(ms) // 2], seen[-1], mem[-1], ms[-1]))


def during(call, label, a, b):
    return [e for e in events if e[1] == call and e[2] == label and a <= e[0] < b]


def placed(text, label, a, b):
    """The label's last place in [a, b), checked against the sign."""
    cx, cy, depth, bg = SIGNS[text]
    tr = during('SetOverlayTransformAbsolute', label, 0, b)
    if not tr or not during('ShowOverlay', label, a, b):
        return False, 'not shown'
    v = [float(x) for x in re.findall(r'-?\d+\.\d+', tr[-1][3])]
    pos, right, up, back = v[0:3], v[3:6], v[6:9], v[9:12]
    px, py, d = pixel(pos)
    ok = abs(px - cx) < 6 and abs(py - cy) < 6 and abs(d - depth) / depth < 0.03
    want = ((COS, 0, -SIN), (0, 1, 0), (SIN, 0, COS))   # the head's right, up and back
    ok = ok and all(abs(got[i] - axis[i]) < 0.01 for got, axis in zip((right, up, back), want) for i in range(3)) and 'origin 1' in tr[-1][3]
    return ok, 'at pixel %.1f, %.1f and %.2f m; the sign at %d, %d and %.1f m' % (px, py, d, cx, cy, depth)


def picture(text, label, a, b):
    """The label's picture sent in [a, b): the sign's own background, with writing on it, under 1000 pixels a side."""
    bg = SIGNS[text][3]
    n = len(during('SetOverlayTexture', label, 0, a))
    pics = sorted(p for p in os.listdir(os.path.join(D, 'pics')) if p.startswith('label_%d_' % label))[n:n + len(during('SetOverlayTexture', label, a, b))]
    for p in pics:
        raw = open(os.path.join(D, 'pics', p), 'rb').read()
        w, h = struct.unpack_from('<ii', raw, 18)
        px = raw[54:]
        b0, g0, r0 = px[0], px[1], px[2]
        same = all(abs(x - y) <= 6 for x, y in zip((r0, g0, b0), bg))
        ink = sum(1 for i in range(0, len(px) - 3, 4 * 7) if abs(px[i] - b0) + abs(px[i + 1] - g0) + abs(px[i + 2] - r0) > 120)
        if same and ink > 30 and w < 1000 and abs(h) < 1000:
            return True, '%s, %d x %d' % (p, w, abs(h))
    return False, 'no such picture among %s' % pics


def first_label(text, index=0):
    found = [n for n in news if n[0] == text]
    return found[index] if len(found) > index else (text, None, 0, False)


# phase 0 and 1: the signs are read, their distance told, their labels placed and drawn
for text, a, b in ((WELCOME, phase[0], phase[2]), (FEED, phase[0], phase[1]), (EXIT, phase[1], phase[2])):
    _, label, got, known = first_label(text)
    depth = SIGNS[text][2]
    check(label is not None and known and abs(got - depth) / depth < 0.03, '"%s": its distance is told from the two eyes (%.2f m, really %.1f m)' % (text, got, depth))
    if label is None:
        continue
    ok, info = placed(text, label, a, b)
    check(ok, '"%s": its label is over the sign, facing the viewer (%s)' % (text, info))
    ok, info = picture(text, label, a, b)
    check(ok, '"%s": its label is drawn in the sign\'s colours (%s)' % (text, info))

# over time
shows = [e for e in events if e[1] == 'ShowOverlay']
t_first = min([e[0] for e in shows] or [0])
_, exit_label, _, _ = first_label(EXIT)
_, welcome_label, _, _ = first_label(WELCOME)
t_exit = min([e[0] for e in shows if e[2] == exit_label and e[0] >= phase[1]] or [0])
check(t_exit > 0 and t_exit - phase[1] < 1500, 'the changed sign has its new label %.2f s after it changed' % ((t_exit - phase[1]) / 1000.0))
hides = [e for e in events if e[1] == 'HideOverlay' and phase[2] <= e[0] < phase[3]]
t_gone = max([e[0] for e in hides] or [0])
check({welcome_label, exit_label} <= set(e[2] for e in hides) and t_gone - phase[2] < 1500, 'with the signs gone their labels go, %.2f s after' % ((t_gone - phase[2]) / 1000.0))
check(not [e for e in shows if t_gone < e[0] < phase[3]], 'nothing is shown while there is nothing to read')
# phase 3: the signs again: read again, their translations still known
back = [e for e in shows if phase[3] <= e[0] < phase[4]]
t_back = max([e[0] for e in back] or [0])
check(len(set(e[2] for e in back)) == 2 and t_back - phase[3] < 1200, 'the signs come back: both have their labels again %.2f s later, without asking DeepL' % ((t_back - phase[3]) / 1000.0))
for text in (WELCOME, EXIT):
    again = [n for n in news if n[0] == text]
    if len(again) >= 2:
        ok, info = placed(text, again[-1][1], phase[3], phase[4])
        check(ok, '"%s": its label is over the sign again (%s)' % (text, info))
    else:
        check(False, '"%s": read again when it came back' % text)
# phase 4: Ctrl+F11
destroyed = [e for e in events if e[1] == 'DestroyOverlay' and e[0] >= phase[4]]
t_off = max([e[0] for e in destroyed] or [0])
check(set(created) <= set(e[2] for e in destroyed) and t_off - phase[4] < 4000, 'Ctrl+F11 in VR: the app follows and takes its labels away, %.1f s later' % ((t_off - phase[4]) / 1000.0))
check(not [e for e in events if e[0] > t_off and e[1] in ('ShowOverlay', 'CreateOverlay')], 'nothing is shown once it is off')
ini = read('vrupscale.ini')
section = ''
enabled = None
for l in ini:
    if l.startswith('['):
        section = l.strip()
    elif section == '[translate]' and l.startswith('enabled='):
        enabled = l.strip()
check(enabled == 'enabled=0', 'the switch is saved as off in the settings file (%s)' % enabled)
check(any('app exit 0' in l for l in out), 'the app closes when SteamVR asks')

print('RESULT: %d passed, %d failed' % (passed, failed))
sys.exit(1 if failed else 0)
