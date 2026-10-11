#!/usr/bin/env python3
"""Builds the app's embedded fonts from Rubik (SIL OFL 1.1, third_party/rubik).

For each weight: a static instance of the variable font, an added right arrow (Rubik has none),
a subset to Latin-1 and a few punctuation marks, GPOS kerning flattened into a legacy 'kern'
table (GDI only reads that one), a unique family name so an installed Rubik can't get in the way,
and ttfautohint hinting for GDI at small sizes. Output: app/fonts/rubik-<weight>.ttf.

Needs: pip install fonttools ttfautohint-py
"""
import io
import os
import sys

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.varLib import instancer
from fontTools.subset import Subsetter, Options
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
import ttfautohint

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'third_party', 'rubik', 'Rubik[wght].ttf')
OUT = os.path.join(ROOT, 'app', 'fonts')
WEIGHTS = (400, 500, 700, 800)
UNICODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2192, 0x2212, 0x20AC]


def add_arrow(font):
    """A right arrow drawn with the weight's own stroke (the hyphen's thickness, at its height)."""
    gs = font.getGlyphSet()
    hy = font.getBestCmap()[ord('-')]
    bp = BoundsPen(gs)
    gs[hy].draw(bp)
    x0, y0, x1, y1 = bp.bounds
    t = y1 - y0
    c = (y0 + y1) / 2
    adv = round(720 + t)
    left, tip = 70, adv - 70
    head_h = t * 1.7 + 260
    head_l = head_h * 0.66
    xh = tip - head_l
    pen = TTGlyphPen(None)
    # one clockwise contour: shaft, then the head; the head's back edge is notched for a lighter look
    pts = [(left, c + t / 2), (xh + t * 0.4, c + t / 2), (xh, c + head_h / 2), (xh + t * 1.05, c + head_h / 2),
           (tip, c + t * 0.15), (tip, c - t * 0.15), (xh + t * 1.05, c - head_h / 2), (xh, c - head_h / 2),
           (xh + t * 0.4, c - t / 2), (left, c - t / 2)]
    pen.moveTo(tuple(round(v) for v in pts[0]))
    for p in pts[1:]:
        pen.lineTo(tuple(round(v) for v in p))
    pen.closePath()
    name = 'arrowright'
    glyf = font['glyf']
    order = list(font.getGlyphOrder())
    glyf[name] = pen.glyph()
    if name not in order:
        order.append(name)
    glyf.glyphOrder = order
    font.setGlyphOrder(order)
    font['hmtx'][name] = (adv, left)
    for st in font['cmap'].tables:
        if st.isUnicode():
            st.cmap[0x2192] = name
    font['maxp'].numGlyphs = len(font.getGlyphOrder())


def pair_value(sub, a, b, classes1):
    """XAdvance of pair (a, b) in a PairPos subtable, or None if the subtable does not cover a."""
    if a not in sub.Coverage.glyphs:
        return None
    if sub.Format == 1:
        ps = sub.PairSet[sub.Coverage.glyphs.index(a)]
        for r in ps.PairValueRecord:
            if r.SecondGlyph == b:
                return getattr(r.Value1, 'XAdvance', 0) or 0
        return None   # first glyph covered, pair absent: the next subtable may have it
    c1 = sub.ClassDef1.classDefs.get(a, 0)
    c2 = sub.ClassDef2.classDefs.get(b, 0)
    v = sub.Class1Record[c1].Class2Record[c2].Value1
    return (getattr(v, 'XAdvance', 0) or 0) if v else 0


def flatten_kerning(font):
    """GPOS 'kern' -> pairs {(left, right): value} for the glyphs of the cmap."""
    gpos = font['GPOS'].table
    idx = set()
    for fr in gpos.FeatureList.FeatureRecord:
        if fr.FeatureTag == 'kern':
            idx.update(fr.Feature.LookupListIndex)
    subs = []   # per lookup, its PairPos subtables
    for i in sorted(idx):
        lk = gpos.LookupList.Lookup[i]
        tables = []
        for st in lk.SubTable:
            if lk.LookupType == 9:
                st = st.ExtSubTable
            if getattr(st, 'LookupType', 2) == 2 or hasattr(st, 'Coverage'):
                tables.append(st)
        subs.append(tables)
    # the characters the app's text uses (accented letters kern well enough without)
    cmap = font.getBestCmap()
    glyphs = sorted({cmap[c] for c in list(range(0x20, 0x7F)) + [0xD7, 0x2192, 0x2026, 0x2013, 0x2014, 0x2019] if c in cmap})
    pairs = {}
    for a in glyphs:
        for b in glyphs:
            total = 0
            for tables in subs:
                for st in tables:
                    v = pair_value(st, a, b, None)
                    if v is not None:
                        total += v
                        break
            if abs(total) >= 5:
                pairs[(a, b)] = total
    return pairs


def build(weight):
    font = instancer.instantiateVariableFont(TTFont(SRC), {'wght': weight}, updateFontNames=False)
    add_arrow(font)
    opt = Options()
    opt.layout_features = ['kern']
    opt.name_IDs = ['*']
    opt.notdef_outline = True
    opt.hinting = False
    sub = Subsetter(opt)
    sub.populate(unicodes=UNICODES)
    sub.subset(font)
    pairs = flatten_kerning(font)
    kern = newTable('kern')
    kern.version = 0
    st = KernTable_format_0()
    st.version, st.coverage, st.format = 0, 1, 0
    st.tupleIndex = None
    st.kernTable = pairs
    kern.kernTables = [st]
    font['kern'] = kern
    for t in ('GPOS', 'GSUB', 'GDEF', 'STAT', 'fvar', 'gvar', 'avar', 'HVAR', 'MVAR'):
        if t in font:
            del font[t]
    # one family per weight, all "Regular" with weight 400, so GDI picks it by name and never
    # synthesizes bold
    family = 'vrupscale Rubik %d' % weight
    name = font['name']
    for rec in list(name.names):
        if rec.nameID in (16, 17, 21, 22, 25):
            name.names.remove(rec)
    name.setName(family, 1, 3, 1, 0x409)
    name.setName('Regular', 2, 3, 1, 0x409)
    name.setName(family.replace(' ', '') + '-Regular', 6, 3, 1, 0x409)
    name.setName(family, 4, 3, 1, 0x409)
    name.setName('%s; built by vrupscale tools/fonts.py from Rubik' % family, 3, 3, 1, 0x409)
    for rec in list(name.names):
        if rec.platformID == 1:
            name.names.remove(rec)
    os2 = font['OS/2']
    os2.usWeightClass = 400
    os2.fsSelection = (os2.fsSelection & ~0b1100001) | 0x40   # REGULAR, not bold/italic
    font['head'].macStyle = 0
    # line metrics from what is left: GDI's line height is winAscent + winDescent
    gs = font.getGlyphSet()
    ymax, ymin = 0, 0
    for g in font.getGlyphOrder():
        bp = BoundsPen(gs)
        gs[g].draw(bp)
        if bp.bounds:
            ymin, ymax = min(ymin, bp.bounds[1]), max(ymax, bp.bounds[3])
    os2.usWinAscent = int(ymax) + 10
    os2.usWinDescent = int(-ymin) + 10
    buf = io.BytesIO()
    font.save(buf)
    hinted = ttfautohint.ttfautohint(in_buffer=buf.getvalue(), hinting_range_min=8, hinting_range_max=50,
                                     increase_x_height=14, windows_compatibility=True)
    path = os.path.join(OUT, 'rubik-%d.ttf' % weight)
    with open(path, 'wb') as f:
        f.write(hinted)
    print('%s: %d bytes, %d glyphs, %d kerning pairs' % (path, len(hinted), len(font.getGlyphOrder()), len(pairs)))


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for w in (WEIGHTS if len(sys.argv) < 2 else [int(a) for a in sys.argv[1:]]):
        build(w)
