"""Grid of SA: export the data store's plots in the website's national-plots format (natp bundles).

    python export_web.py OUT_DIR [--dsn "dbname=gsa"] [--skip CPT] [--muni WC053,EC101]
    python <site>/scripts/natp_manifest.py OUT_DIR/manifest_local.json data/natp/manifest.json --urls urls.json

One store, two front ends: export_tiles.py / export_pack.py feed the desktop app, this script feeds the
website. It writes the same files as the website's scripts/nat_tile.py, which read the parcel and roll
files directly:

    OUT_DIR/bundles/<z>_<x>_<y>.js   JSON string of base64(gzip(json {tiles: {"x_y": tile}})), named .js
                                     because the artifact asset store takes .js in batch uploads
    OUT_DIR/manifest_local.json      {bundles, tiles, rolls, n, valued, source}; natp_manifest.py turns it
                                     into data/natp/manifest.json once the bundles are uploaded

    tile = {o: [lon, lat] of the z12 tile's south-west corner, T: strings,
            a: [[town, type, parcel, portion, sg21, area_m2, value, cat, roll, farm, address, zoning, src, sale, sale_date]],
            g: [[a index, dx, dy, ...]]}   outer rings, 1e-6 degree integers, delta-encoded from o

Same rules as nat_tile.py: municipalities in --skip (Cape Town, which has its own plot layer) are left out;
plots replaced by newer metro stands are left out (core.parcel_live); values with area_check 'mismatch',
'zero' placeholders and superseded rows (core.valuation_live) are not shown; one value per plot. The
roll 'unit' column is never loaded into the store, and owner names and ID numbers do not exist there.
"""
import argparse, base64, collections, glob, gzip, hashlib, json, math, os, sys
import psycopg

TY = {"erf": 0, "farm": 1, "farm_portion": 2, "holding": 3, "public_place": 4, "unsurveyed_stand": 5}
MAXB = 4_000_000   # target max gz bytes per bundle
METRO_SRC = ("City of Johannesburg", "City of Ekurhuleni")

def txy(lon, lat, z):
    n = 2 ** z; r = math.radians(lat)
    return int((lon + 180) / 360 * n), int((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n)

def torig(x, y, z=12):
    n = 2 ** z; return x / n * 360 - 180, math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y + 1) / n))))

def rdp(pts, eps):
    if len(pts) < 4: return pts
    keep = [False] * len(pts); keep[0] = keep[-1] = True; st = [(0, len(pts) - 1)]
    while st:
        a, b = st.pop(); (x1, y1), (x2, y2) = pts[a], pts[b]; dx, dy = x2 - x1, y2 - y1; L = math.hypot(dx, dy) or 1e-12; dm, im = 0, -1
        for i in range(a + 1, b):
            d = abs(dy * pts[i][0] - dx * pts[i][1] + x2 * y1 - y2 * x1) / L
            if d > dm: dm, im = d, i
        if dm > eps and im > 0: keep[im] = True; st += [(a, im), (im, b)]
    return [p for p, k in zip(pts, keep) if k]

def simp(r):
    """Outer ring simplified to ~0.2 m, as nat_tile.py does (split at the far point so closed rings keep their shape)."""
    ring = r[:-1] if r[0] == r[-1] else r
    if len(ring) < 3: return []
    k = max(range(len(ring)), key=lambda i: (ring[i][0] - ring[0][0]) ** 2 + (ring[i][1] - ring[0][1]) ** 2)
    return rdp(ring[:k + 1], 2e-6)[:-1] + rdp(ring[k:] + [ring[0]], 2e-6)[:-1]

def title(s):
    return " ".join(w.capitalize() for w in str(s).split()) if s else ""

# One row per plot, with its one shown value (highest-priority roll, then the newest period).
SQL = """
SELECT p.sg21, p.kind, p.town, p.farm_name, p.scheme, p.parcel_no, p.portion_no, p.area_m2, p.muni_id, p.src,
       v.market_value, v.category, v.usage, v.muni_code, v.roll, v.roll_period, v.source_url, v.address, v.zoning,
       v.sale_price, v.sale_date::text, ST_AsGeoJSON(p.geom, 7)
FROM core.parcel_live p
LEFT JOIN LATERAL (
  SELECT sum(market_value) OVER () AS market_value, category, usage, muni_code, roll, roll_period, source_url, address, zoning,
         max(sale_price) OVER () AS sale_price, max(sale_date) OVER () AS sale_date
  FROM core.valuation_live
  WHERE sg21 = p.sg21 AND market_value IS NOT NULL AND coalesce(area_check, '') <> 'mismatch'
  ORDER BY roll_period DESC NULLS LAST LIMIT 1) v ON true
{where}"""

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out"); ap.add_argument("--dsn", default=os.environ.get("GSA_DSN", "dbname=gsa"))
    ap.add_argument("--skip", default="CPT", help="municipalities left out (default CPT: the site has its own Cape Town plots)")
    ap.add_argument("--muni", default="", help="only these municipalities (for a test)")
    a = ap.parse_args()
    skip = [m for m in a.skip.split(",") if m]; only = [m for m in a.muni.split(",") if m]
    spool_dir, bun = os.path.join(a.out, "spool"), os.path.join(a.out, "bundles")
    os.makedirs(spool_dir, exist_ok=True); os.makedirs(bun, exist_ok=True)
    for f in glob.glob(spool_dir + "/*") + glob.glob(bun + "/*"): os.remove(f)

    rolls, rix = [], {}
    def roll_of(muni, roll, period, url):
        if roll and roll.startswith("KZN COGTA"): per = period or "roll year not stated"
        elif roll == "JHB GV2023": per = "valued " + (period or "")[:10]
        else: per = period or ""
        k = (muni or "", per, url or "")
        if k not in rix: rix[k] = len(rolls); rolls.append(list(k))
        return rix[k]

    spool = {}
    def sp(k):
        if k not in spool:
            if len(spool) > 400:
                for f in spool.values(): f.close()
                spool.clear()
            spool[k] = open(f"{spool_dir}/{k}.jl", "a")
        return spool[k]

    conds, params = ["coalesce(p.muni_id, '') <> ALL(%(skip)s)"], {"skip": skip}
    if only: conds.append("p.muni_id = ANY(%(only)s)"); params["only"] = only
    pg = psycopg.connect(a.dsn)
    n = nv = 0; munis = collections.Counter()
    with pg.cursor(name="web_plots") as c:
        c.itersize = 20000
        c.execute(SQL.format(where="WHERE " + " AND ".join(conds)), params)
        for (sg, kind, town, farm, scheme, parcel_no, portion, area, muni, src, mv, cat, use, vmuni, roll, period, url, addr, zoning,
             sale, sale_date, gj) in c:
            g = json.loads(gj)
            polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]] if g["type"] == "Polygon" else []
            rings = [simp(pl[0]) for pl in polys if pl and len(pl[0]) >= 4]
            rings = [r for r in rings if len(r) >= 3]
            if not rings: continue
            big = max(rings, key=len); cx = sum(q[0] for q in big) / len(big); cy = sum(q[1] for q in big) / len(big)
            x12, y12 = txy(cx, cy, 12)
            metro = bool(src) and src.startswith(METRO_SRC)
            v = None
            if mv is not None:
                c_ = title((cat or "").split("-", 1)[-1].strip())[:40] if roll != "JHB GV2023" else title(cat or "")
                sp_ = int(sale) if sale else None
                v = (int(mv), c_, roll_of(vmuni, roll, period, url), (addr or "").strip(), sp_, (sale_date or "")[:10] if sp_ else None)
                nv += 1
            row = [title(town), TY.get(kind, 0), parcel_no or "", portion or 0, sg, area, v[0] if v else None, v[1] if v else None,
                   v[2] if v else None, title(farm or scheme), title(v[3] if v else ""), (zoning or "").strip() if v else "",
                   src if metro else None, v[4] if v else None, v[5] if v else None]
            q = [[[round(X * 1e6), round(Y * 1e6)] for X, Y in r] for r in rings]
            sp(f"{x12 >> 4}_{y12 >> 4}").write(json.dumps([x12, y12, row, q], separators=(",", ":")) + "\n")
            n += 1; munis[muni] += 1
            if n % 500000 == 0: print(f"  {n} plots", flush=True)
    for f in spool.values(): f.close()
    print(f"plots {n}, valued {nv}, municipalities {len(munis)}", flush=True)

    def tile_body(x, y, rows):
        ox, oy = torig(x, y); oxi, oyi = round(ox * 1e6), round(oy * 1e6)
        S = {}; A = []; G = []
        def s(v):
            if v not in S: S[v] = len(S)
            return S[v]
        for _, _, r, q in rows:
            ai = len(A); row = [s(r[0])] + r[1:9] + [s(x) if x else None for x in r[9:13]] + r[13:15]
            while len(row) > 5 and row[-1] in (None, ""): row.pop()
            A.append(row)
            for ring in q:
                flat = [ai]; px = py = 0
                for X, Y in ring:
                    qx, qy = X - oxi, Y - oyi; flat += [qx - px, qy - py]; px, py = qx, qy
                G.append(flat)
        return {"o": [round(ox, 6), round(oy, 6)], "T": list(S), "a": A, "g": G}

    man = {"bundles": [], "tiles": [], "rolls": rolls, "n": n, "valued": nv,
           "source": "Grid of SA data store: Chief Surveyor-General cadastre (via DFFE, 2022; Western Cape SG, 2026; KZN COGTA, 2026), "
                     "City of Johannesburg and City of Ekurhuleni stands; values from municipal valuation rolls"}
    for fn in sorted(glob.glob(spool_dir + "/*.jl")):
        x8, y8 = map(int, os.path.basename(fn)[:-3].split("_"))
        by = collections.defaultdict(list)
        for line in open(fn):
            r = json.loads(line); by[(r[0], r[1])].append(r)
        def emit(z, x, y, keys):
            tiles = {f"{k[0]}_{k[1]}": tile_body(k[0], k[1], by[k]) for k in keys}
            gz = gzip.compress(json.dumps({"tiles": tiles}, separators=(",", ":"), ensure_ascii=False).encode(), 9)
            if len(gz) > MAXB and z < 12:
                for dx in (0, 1):
                    for dy in (0, 1):
                        sx, sy = x * 2 + dx, y * 2 + dy; sh = 12 - (z + 1)
                        sub = [k for k in keys if (k[0] >> sh) == sx and (k[1] >> sh) == sy]
                        if sub: emit(z + 1, sx, sy, sub)
                return
            name = f"{z}_{x}_{y}.js"
            body = '"' + base64.b64encode(gz).decode() + '"'
            with open(f"{bun}/{name}", "w") as f: f.write(body)
            man["bundles"].append([z, x, y, name, hashlib.sha1(body.encode()).hexdigest()[:12], len(body)])
            for k in keys: man["tiles"].append([k[0], k[1], len(by[k]), name])
        emit(8, x8, y8, list(by))
        os.remove(fn)
    with open(os.path.join(a.out, "manifest_local.json"), "w") as f: json.dump(man, f, separators=(",", ":"))
    print(f"bundles {len(man['bundles'])}, tiles {len(man['tiles'])}, {sum(b[5] for b in man['bundles']) / 1e6:.1f} MB")

if __name__ == "__main__":
    main()
