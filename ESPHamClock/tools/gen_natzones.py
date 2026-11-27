#!/usr/bin/env python3
"""
gen_natzones.py -- build natzones_data.cpp for HamClock from the NorthAmericanTimeZones project data.

source: https://github.com/EtherHamRadio/NorthAmericanTimeZones-standalone (CC-1.0 license)

Inputs (from NorthAmericanTimeZones-standalone/source/vendor/):
    admin-can-usa-mex.geojson   Natural Earth admin-1 (provinces/states) for Canada, USA, Mexico
    great-lakes.geojson         the five big lakes (removed from the filled areas)

Output:
    natzones_data.cpp           compact tables: fill rings, zone-to-zone border lines, label anchors

The zone grouping and the sub-province "variance" areas below mirror tz-map.template.html
(tables US, CA, MXS, V and VM).  If that project changes, edit the tables here and re-run.

Usage:
    python3 gen_natzones.py  <vendor-dir>  <out-natzones_data.cpp>   [--tol 0.04]

Needs: shapely >= 2.1 (pip install shapely)
"""
import json, sys, math, argparse, pathlib
import shapely
from shapely.geometry import shape, Polygon, MultiPolygon, box, LineString, MultiLineString
from shapely.ops import unary_union, linemerge, polylabel

# ----------------------------------------------------------------------------------------------
# zone ids -- order MUST match enum NATZoneId in natzones.h
ZONES = ['BCT', 'YKT', 'NWT', 'ABT', 'SKT', 'MBT', 'EAST', 'ATL', 'NLT',
         'PAC', 'MTN', 'ARIZ', 'CEN', 'AK', 'HI', 'PR', 'MXN', 'MXP', 'MXC', 'MXS']
# project group key -> zone id
G2Z = dict(pct='BCT', yk='YKT', nwt='NWT', abt='ABT', sk='SKT', mb='MBT', E='EAST', atl='ATL', nl='NLT',
           P='PAC', M='MTN', Z='ARIZ', C='CEN', AK='AK', HI='HI', PR='PR',
           mxN='MXN', mxP='MXP', mxC='MXC', mxS='MXS')

# province/territory -> group   (tz-map.template.html: CA)
CA = dict(BC='pct', YT='yk', NT='nwt', AB='abt', SK='sk', MB='mb', ON='E', QC='E', NU='E',
          NB='atl', NS='atl', PE='atl', NL='nl')
# US state -> group             (tz-map.template.html: US)
US = dict(P='WA OR CA NV', M='ID MT WY UT CO NM', Z='AZ',
          C='ND SD NE KS OK TX MN IA MO AR LA MS AL TN IL WI',
          E='MI IN KY OH WV VA NC SC GA FL PA NY VT NH ME MA CT RI NJ MD DE DC',
          AK='AK', HI='HI')
# Mexico state -> group         (tz-map.template.html: MXS; anything else is Centro)
MXS = dict(mxN='BC', mxP='BS SI NA SO', mxS='QR')

def R(w, e, s, n):
    return [(w, s), (e, s), (e, n), (w, n)]

# sub-province areas that keep a different time from their parent (tz-map.template.html: V + VM)
# (name, parents 'CTRY:postal|...', target group, [rings of (lng,lat)])
VARIANCE = [
    ('East Kootenay',           'CAN:BC', 'abt', [[(-116.1, 48.9), (-113.5, 48.9), (-113.5, 50.9), (-116.3, 50.9)]]),
    ('Northwest (Kenora)',      'CAN:ON', 'mb',  [R(-96, -90, 48, 57)]),
    ('Lower North Shore',       'CAN:QC', 'atl', [R(-63, -56, 50, 53.2)]),
    ('Goose Bay / Labrador',    'CAN:NL', 'atl', [R(-67, -55.4, 52, 60.6)]),
    ('SE Labrador',             'CAN:NL', 'nl',  [R(-58, -55.4, 52, 53.6)]),
    ('Kivalliq',                'CAN:NU', 'C',   [R(-102, -86, 60, 69.5)]),
    ('Kitikmeot',               'CAN:NU', 'M',   [R(-125, -102, 60, 80)]),
    ('Navajo Nation',           'USA:AZ', 'M',   [R(-111.6, -109, 35, 37)]),
    ('Florida panhandle',       'USA:FL', 'C',   [R(-88, -85, 29, 31.1)]),
    ('Indiana NW/SW',           'USA:IN', 'C',   [R(-88.2, -86.4, 40.7, 41.8), R(-88.2, -86.4, 37.7, 38.5)]),
    ('Kansas west',             'USA:KS', 'M',   [R(-102.1, -101.35, 37.7, 40)]),
    ('Kentucky west',           'USA:KY', 'C',   [R(-89.7, -86.2, 36.4, 38.1)]),
    ('Michigan UP',             'USA:MI', 'C',   [[(-90.5, 46.1), (-90.3, 46.6), (-89.2, 46.6), (-89, 46.45), (-88, 46.5),
                                                   (-87.6, 46.2), (-87.3, 45.9), (-87.3, 45.1), (-88.2, 45.1), (-90.5, 45.9)]]),
    ('Nebraska panhandle',      'USA:NE', 'M',   [R(-104.1, -101.6, 40, 43)]),
    ('North Dakota SW',         'USA:ND', 'M',   [R(-104.1, -102.3, 45.9, 48.1), R(-102.3, -101, 45.9, 46.9)]),
    ('Malheur County',          'USA:OR', 'M',   [R(-118.4, -116.9, 42, 44)]),
    ('South Dakota west',       'USA:SD', 'M',   [R(-104.1, -100.65, 43, 45.95)]),
    ('Tennessee east',          'USA:TN', 'E',   [[(-85.4, 34.9), (-85, 35.5), (-84.9, 35.95), (-84.7, 36.4), (-84.6, 36.7),
                                                   (-81.5, 36.7), (-81.5, 34.9)]]),
    ('Texas El Paso',           'USA:TX', 'M',   [R(-106.7, -104.8, 30.65, 32)]),
    ('Idaho panhandle',         'USA:ID', 'P',   [R(-117.3, -114.7, 45.5, 49)]),
    ('Chihuahua (Juarez..)',    'MEX:CH', 'M',   [R(-108.7, -105.75, 30.2, 31.9)]),
    ('Chihuahua (Ojinaga..)',   'MEX:CH', 'C',   [R(-105.75, -103.9, 28.6, 30.4)]),
    ('Coahuila/NL/Tamps',       'MEX:CO|MEX:NL|MEX:TM', 'C',
                                [R(-102.9, -100.2, 27.8, 29.9), R(-100.5, -99.6, 27.0, 27.9),
                                 R(-99.4, -99.0, 25.9, 26.3), R(-100.0, -97.1, 25.4, 27.9)]),
    ('Bahia de Banderas',       'MEX:NA', 'mxC', [R(-105.6, -105.05, 20.6, 21.0)]),
]

# Puerto Rico / USVI are not in the admin file; a rough outline of Puerto Rico stands in (zone PR)
PR_RING = [(-67.27, 18.37), (-66.5, 18.49), (-66.0, 18.46), (-65.62, 18.38), (-65.6, 18.2), (-65.75, 18.0),
           (-66.5, 17.93), (-67.2, 17.95), (-67.2, 18.15)]

# extra label anchors that the automatic rule can't know about: (zone, lat, lng)
EXTRA_LABELS = [('HI', 20.6, -157.0), ('PR', 18.25, -66.4),
                ('ATL', 46.3, -65.8)]       # New Brunswick / Nova Scotia (Labrador is labelled automatically)

MIN_PIECE_AREA = 0.10       # sq deg; smaller islands are dropped from the fill rings
LABEL_MIN_AREA = 5.0        # sq deg; a piece this big gets its own label
LABEL_SPREAD   = 9.0        # deg; no second label for the same zone closer than this to the first

# ----------------------------------------------------------------------------------------------
def pieces(g):
    if g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    return [p for p in getattr(g, 'geoms', []) if isinstance(p, Polygon)]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('vendor')
    ap.add_argument('out')
    ap.add_argument('--tol', type=float, default=0.04, help='simplify tolerance, degrees')
    a = ap.parse_args()
    vend = pathlib.Path(a.vendor)

    admin = json.loads((vend / 'admin-can-usa-mex.geojson').read_text(encoding='utf-8'))
    lakes = json.loads((vend / 'great-lakes.geojson').read_text(encoding='utf-8'))

    west = box(-180, -90, 0, 90)             # all of North America is west of Greenwich; drops far Aleutians
    feat = {}
    for f in admin['features']:
        p = f['properties']
        feat[p['adm0_a3'] + ':' + p['postal']] = shape(f['geometry']).buffer(0).intersection(west)
    feat['USA:PR'] = Polygon(PR_RING)

    us_grp = {st: g for g, sts in US.items() for st in sts.split()}
    mx_grp = {st: g for g, sts in MXS.items() for st in sts.split()}

    def group_of(fid):
        c, pc = fid.split(':')
        if c == 'CAN': return CA[pc]
        if c == 'USA': return 'PR' if pc == 'PR' else us_grp[pc]
        return mx_grp.get(pc, 'mxC')

    def country_of(fid):
        return 'CAN' if fid.startswith('CAN') else ('MEX' if fid.startswith('MEX') else 'USA')

    # geometry store: (group, country) -> geometry
    store = {}
    def add(key, g):
        if g.is_empty: return
        store[key] = store[key].union(g) if key in store else g
    for fid, g in feat.items():
        add((group_of(fid), country_of(fid)), g)

    # apply the sub-province variances in order: take the area from whoever holds it, give it to the target group
    for name, parents, target, rings in VARIANCE:
        par = [feat[p] for p in parents.split('|')]
        region = unary_union(par).intersection(unary_union([Polygon(r) for r in rings]))
        if region.is_empty:
            print('warning: empty variance', name); continue
        ctry = country_of(parents.split('|')[0])
        for key in list(store):
            store[key] = store[key].difference(region)
            if store[key].is_empty: del store[key]
        add((target, ctry), region)

    # lakes come out of the filled areas
    lake_u = unary_union([shape(f['geometry']).buffer(0) for f in lakes['features']])
    for key in list(store):
        store[key] = store[key].difference(lake_u)

    # ---- explode into polygons, drop dust, coverage-simplify so shared borders stay shared
    polys = []          # (zone, country, Polygon)
    for (g, c), geom in sorted(store.items()):
        for p in pieces(geom):
            if p.area >= MIN_PIECE_AREA:
                polys.append((G2Z[g], c, p))
    geoms = [p for _, _, p in polys]
    try:
        simp = list(shapely.coverage_simplify(geoms, a.tol))
    except Exception as e:                                          # older shapely/GEOS
        print('coverage_simplify unavailable (%s); simplifying per polygon' % e)
        simp = [p.simplify(a.tol, preserve_topology=True) for p in geoms]
    polys = [(z, c, s if isinstance(s, Polygon) else p) for (z, c, p), s in zip(polys, simp)]
    polys = [(z, c, p) for z, c, p in polys if (not p.is_empty) and p.area >= MIN_PIECE_AREA * 0.5]

    # ---- zone-to-zone borders (only where DIFFERENT time zones touch; coasts are not drawn)
    by_zone = {}
    for z, c, p in polys:
        by_zone.setdefault(z, []).append(p)
    zone_geom = {z: unary_union(ps) for z, ps in by_zone.items()}
    zlist = sorted(zone_geom)
    borders = []
    for i, za in enumerate(zlist):
        for zb in zlist[i + 1:]:
            ga, gb = zone_geom[za], zone_geom[zb]
            if not ga.buffer(0.003).intersects(gb):
                continue
            shared = ga.boundary.intersection(gb.buffer(0.003))
            if shared.is_empty:
                continue
            lines = []
            for part in getattr(shared, 'geoms', [shared]):
                if isinstance(part, LineString) and part.length > 0.05:
                    lines.append(part)
                elif isinstance(part, MultiLineString):
                    lines += [l for l in part.geoms if l.length > 0.05]
            if not lines:
                continue
            merged = linemerge(lines)
            for ln in getattr(merged, 'geoms', [merged]):
                if isinstance(ln, LineString) and ln.length > 0.05:
                    borders.append(ln)

    # ---- labels
    # rule: the biggest piece of each zone in each country gets a label.  A second label for the same
    # zone+country only if that piece is large, well away from the first, and not in the high Arctic.
    # Mexico labels only for the four Mexican zones (not for the US-border municipalities).
    labels = []         # (zone, lat, lng)
    for z in zlist:
        if z in ('HI', 'PR'):
            continue
        for c in ('CAN', 'USA', 'MEX'):
            if c == 'MEX' and not z.startswith('MX'):
                continue
            ps = sorted([p for zz, cc, p in polys if zz == z and cc == c], key=lambda p: -p.area)
            placed = []
            for p in ps:
                pt = polylabel(p, tolerance=0.05)
                if not placed:
                    if p.area < 1.0 or pt.y > 72:
                        continue
                else:
                    if len(placed) >= 2 or p.area < 20 or pt.y > 70:
                        continue
                    if any(math.hypot(pt.x - x, pt.y - y) < LABEL_SPREAD for x, y in placed):
                        continue
                placed.append((pt.x, pt.y))
                labels.append((z, pt.y, pt.x, c))
    for z, la, lo in EXTRA_LABELS:
        labels.append((z, la, lo, 'USA' if z in ('HI', 'PR') else 'CAN'))
    prio = ['EAST', 'CEN', 'MTN', 'PAC', 'AK', 'ATL', 'NLT', 'HI', 'ARIZ', 'BCT', 'ABT', 'SKT', 'MBT', 'NWT', 'YKT',
            'MXC', 'MXN', 'MXP', 'MXS', 'PR']
    # zone priority first, then south before north
    # tier 1: the big US zones, Alaska, Hawaii, Atlantic Canada.  tier 2: the other named zones.  tier 3: Mexico,
    # the Arctic, Labrador and other second labels.  The runtime draws tier by tier and drops what doesn't fit.
    def tier(z, la, lo, c):
        if c == 'USA' and z in ('EAST', 'CEN', 'MTN', 'PAC', 'AK', 'HI'): return 1
        if z == 'ATL' and la < 50: return 1
        if z in ('NWT', 'YKT'): return 2
        if c == 'MEX' or la > 60 or (z == 'ATL'): return 3
        return 2
    labels = [(z, la, lo, tier(z, la, lo, c)) for z, la, lo, c in labels]
    labels.sort(key=lambda t: (t[3], prio.index(t[0]), t[1]))

    # ---- emit C++
    def c100(v): return int(round(v * 100))
    pts, ring_rec = [], []
    for z, c, p in polys:
        for ring in [p.exterior] + list(p.interiors):
            xy = list(ring.coords)[:-1]                       # drop closing duplicate
            if len(xy) < 3: continue
            ring_rec.append((z, len(pts) // 2, len(xy)))
            for lng, lat in xy:
                pts += [c100(lat), c100(lng)]
    bpts, bord_rec = [], []
    for ln in borders:
        xy = list(ln.coords)
        if len(xy) < 2: continue
        # light extra simplification of border lines
        bord_rec.append((len(bpts) // 2, len(xy)))
        for lng, lat in xy:
            bpts += [c100(lat), c100(lng)]

    o = []
    o.append('// GENERATED by tools/gen_natzones.py -- do not edit by hand.\n'
             '// Source: NorthAmericanTimeZones-standalone (Natural Earth admin-1, public domain; CC0 tz rules).\n'
             '// Coordinates are degrees*100 (lat, lng pairs), same scale as zones.cpp.\n\n'
             '#include "natzones.h"\n\n')
    def arr(name, vals, per=16):
        s = 'const int16_t %s[] = {\n' % name
        for i in range(0, len(vals), per):
            s += '    ' + ','.join(str(v) for v in vals[i:i + per]) + ',\n'
        return s + '};\n\n'
    o.append(arr('natz_fill_pts', pts, 16))
    o.append('const NATZRing natz_rings[] = {\n')
    for z, st, n in ring_rec:
        o.append('    { NZ_%s, %d, %d },\n' % (z, st, n))
    o.append('};\nconst int natz_n_rings = %d;\n\n' % len(ring_rec))
    o.append(arr('natz_border_pts', bpts, 16))
    o.append('const NATZLine natz_borders[] = {\n')
    for st, n in bord_rec:
        o.append('    { %d, %d },\n' % (st, n))
    o.append('};\nconst int natz_n_borders = %d;\n\n' % len(bord_rec))
    o.append('const NATZLabel natz_labels[] = {\n')
    for z, la, lo, tr in labels:
        o.append('    { NZ_%s, %d, %d, %d },\n' % (z, c100(la), c100(lo), tr))
    o.append('};\nconst int natz_n_labels = %d;\n\n' % len(labels))
    pathlib.Path(a.out).write_text(''.join(o), encoding='utf-8')

    print('rings %d  fill vertices %d  border lines %d  border vertices %d  labels %d'
          % (len(ring_rec), len(pts) // 2, len(bord_rec), len(bpts) // 2, len(labels)))
    print('zones present:', ' '.join(zlist))
    print('output: %s (%.0f kB)' % (a.out, pathlib.Path(a.out).stat().st_size / 1024))
    # label list for eyeballing
    for z, la, lo, tr in labels:
        print('  label %-5s %7.2f %8.2f  tier %d' % (z, la, lo, tr))

if __name__ == '__main__':
    main()
