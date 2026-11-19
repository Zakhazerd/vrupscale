#!/usr/bin/env python3
"""tools/ab/cmp.py <dirA>[:tag] <dirB>[:tag] [-v]: compares two tools/ab/run.sh outputs.

Without tags, every <tag>_e<eye>_<frame>.raw in dirA is compared with the same file in dirB and the
result is summed per tag (variant). With tags, dirA's <tagA>_* files are compared with dirB's <tagB>_*
(e.g. cmp.py out:v1 out:w1 -- one run, the default flow against FSR 3's). -v adds a line per frame.
Prints, per variant: differing bytes, the largest difference and the PSNR (99.0 = identical).
Exit status 0 when everything compared is identical, 1 otherwise. Uses numpy when present.
"""
import glob, math, os, sys

try:
    import numpy as np
except ImportError:
    np = None


def stats(fa, fb):
    """(differing values, max difference, count, sum of squared differences)"""
    a = open(fa, 'rb').read()
    b = open(fb, 'rb').read()
    if len(a) != len(b):
        raise SystemExit('%s and %s differ in size (%d vs %d bytes): not the same output size' % (fa, fb, len(a), len(b)))
    if a == b:
        return 0, 0, len(a), 0.0
    if np is not None:
        d = np.abs(np.frombuffer(a, np.uint8).astype(np.int16) - np.frombuffer(b, np.uint8).astype(np.int16))
        return int((d > 0).sum()), int(d.max()), d.size, float((d.astype(np.float64) ** 2).sum())
    n = m = 0
    sq = 0.0
    for x, y in zip(a, b):
        if x != y:
            e = abs(x - y)
            n += 1
            m = max(m, e)
            sq += e * e
    return n, m, len(a), sq


def split_tag(arg):
    """'dir' or 'dir:tag' (a Windows drive letter's colon is part of the folder)"""
    if os.path.isdir(arg):
        return arg, ''
    d, _, t = arg.rpartition(':')
    return (d, t) if d and os.path.isdir(d) else (arg, '')


def order(v):
    """v2 before v10"""
    head = v.rstrip('0123456789')
    return head, int(v[len(head):] or 0)


def psnr(sq, count):
    return 99.0 if sq == 0 else 10 * math.log10(255.0 ** 2 / (sq / count))


def main():
    args = [a for a in sys.argv[1:] if a != '-v']
    verbose = '-v' in sys.argv[1:]
    if len(args) != 2:
        print(__doc__)
        return 2
    (da, ta), (db, tb) = (split_tag(x) for x in args)
    if bool(ta) != bool(tb):
        print('give a tag on both sides or on neither')
        return 2
    pattern = (ta + '_*.raw') if ta else '*.raw'
    tot = {}
    missing = 0
    files = sorted(glob.glob(os.path.join(da, pattern)))
    if not files:
        print('no %s in %s' % (pattern, da))
        return 2
    for fa in files:
        name = os.path.basename(fa)
        fb = os.path.join(db, (tb + name[len(ta):]) if ta else name)
        if not os.path.exists(fb):
            print('missing in %s: %s' % (db, os.path.basename(fb)))
            missing += 1
            continue
        n, m, c, sq = stats(fa, fb)
        v = ta + ':' + tb if ta else name.split('_')[0]
        t = tot.setdefault(v, [0, 0, 0, 0.0, 0])
        t[0] += n; t[1] = max(t[1], m); t[2] += c; t[3] += sq; t[4] += 1
        if verbose:
            print('  %s: %s' % (name, 'identical' if n == 0 else '%d differ (max %d), %.1f dB' % (n, m, psnr(sq, c))))
    same = missing == 0
    for v, t in sorted(tot.items(), key=lambda kv: order(kv[0])):
        same = same and t[0] == 0
        print('%s: %d files, %s' % (v, t[4], 'IDENTICAL' if t[0] == 0 else
              '%d of %d values differ (max %d), PSNR %.1f dB' % (t[0], t[2], t[1], psnr(t[3], t[2]))))
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
