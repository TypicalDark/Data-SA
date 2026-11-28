"""Build the desktop base map pack (roads, rail, land use, landmarks, towns) from the website's base map.

    python tools/webpack/build_base_pack.py WEB_DATA_DIR OUT_DIR

WEB_DATA_DIR holds the website's data/ folder with bm/overview.json and the regional tiles
bm/r/X_Y.json (z8 tiles; each file is a JSON string of base64 gzip). The website draws the
overview below zoom 10 and the regional tiles from zoom 10; the pack does the same:

  zoom 5-9   overview: main roads (classes 0-3), rail, big land areas, towns
  zoom 10-14 regional tiles: every road class, rail, land use, landmarks, towns

Vector layers: `road` (r: class 0 motorway ... 7 track/service, name, ref, bridge, tunnel), `rail` (name),
`land` (c: res, ind, com, park, nat, wood, edu, hosp, air, cem, mil, beach, wet ...; name),
`poi` (k: hospital, school, police ...; name) and `place` (r: rank 0 city ... 4 hamlet; name).
Road classes appear from the zoom the website shows them (classes 5, 6, 7 from 11, 12 and 14).
Source: OpenStreetMap contributors (ODbL). Needs tippecanoe and tile-join.
"""
import argparse, base64, glob, gzip, json, os, subprocess, sys, tempfile

# the zoom each road class starts at in the regional tiles (website stiles minZ, rounded down)
ROAD_MINZ = {0: 10, 1: 10, 2: 10, 3: 10, 4: 10, 5: 11, 6: 12, 7: 14}


def load(p):
    j = json.load(open(p, encoding="utf-8"))
    if isinstance(j, str):
        j = json.loads(gzip.decompress(base64.b64decode(j)))
    return j


def dec(a, ox, oy):
    """Ints of 1e-5 degrees, delta encoded, relative to the origin."""
    out, x, y = [], 0, 0
    for i in range(0, len(a) - 1, 2):
        x += a[i]
        y += a[i + 1]
        out.append([round(ox + x / 1e5, 6), round(oy + y / 1e5, 6)])
    return out


def clean(p):
    return {k: v for k, v in p.items() if v is not None and v != ""}


def feat(props, geom):
    return json.dumps({"type": "Feature", "properties": clean(props), "geometry": geom}, ensure_ascii=False) + "\n"


def line(c):
    return {"type": "LineString", "coordinates": c} if len(c) >= 2 else None


def poly(rings):
    rs = [r + [r[0]] if r[0] != r[-1] else r for r in rings if len(r) >= 3]
    return {"type": "Polygon", "coordinates": rs} if rs else None


def pt(lon, lat):
    return {"type": "Point", "coordinates": [round(lon, 6), round(lat, 6)]}


class Out:
    def __init__(self, tmp):
        self.tmp, self.files, self.n = tmp, {}, {}

    def put(self, key, props, geom):
        if geom is None:
            return
        if key not in self.files:
            self.files[key] = open(os.path.join(self.tmp, key + ".geojsonl"), "w", encoding="utf-8")
        self.files[key].write(feat(props, geom))
        self.n[key] = self.n.get(key, 0) + 1

    def close(self):
        for f in self.files.values():
            f.close()

    def path(self, key):
        return os.path.join(self.tmp, key + ".geojsonl")


def overview(d, o):
    ov = load(os.path.join(d, "bm", "overview.json"))
    for r, n, g in ov["roads"]:
        o.put("ov-road", {"r": r, "name": n}, line(dec(g, 0, 0)))
    for g in ov["rail"]:
        o.put("ov-rail", {}, line(dec(g, 0, 0)))
    for c, n, rings in ov["land"]:
        o.put("ov-land", {"c": c, "name": n}, poly([dec(r, 0, 0) for r in rings]))
    for x, y, r, n in ov["places"]:
        o.put("ov-place", {"r": r, "name": n}, pt(x, y))


def regional(d, o):
    paths = sorted(glob.glob(os.path.join(d, "bm", "r", "*.json")))
    seen_poi, seen_place = set(), set()
    for p in paths:
        t = load(p)
        ox, oy = t["o"]
        for q in t["roads"]:
            r, n, ref, g = q[:4]
            br, tu = (q[4], q[5]) if len(q) >= 6 else (0, 0)
            o.put(f"road{ROAD_MINZ.get(r, 14)}", {"r": r, "name": n, "ref": ref, "bridge": br or None, "tunnel": tu or None},
                  line(dec(g, ox, oy)))
        for k, n, g in t["rail"]:
            o.put("rail", {"name": n}, line(dec(g, ox, oy)))
        for c, n, rings in t["land"]:
            o.put("land", {"c": c, "name": n}, poly([dec(r, ox, oy) for r in rings]))
        for x, y, k, n in t["pois"]:
            if (x, y, n) in seen_poi:
                continue
            seen_poi.add((x, y, n))
            o.put("poi", {"k": k, "name": n}, pt(x, y))
        for x, y, r, n in t["places"]:
            if (x, y, n) in seen_place:
                continue
            seen_place.add((x, y, n))
            o.put("place", {"r": r, "name": n}, pt(x, y))
    return len(paths)


def tippe(out, z0, z1, layers, extra=()):
    cmd = ["tippecanoe", "-o", out, "--force", "-q", "-Z", str(z0), "-z", str(z1), "--no-tile-size-limit",
           "--no-feature-limit", "--simplification=2", *extra]
    for layer, path in layers:
        cmd += ["-L", json.dumps({"file": path, "layer": layer})]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("web_data")
    ap.add_argument("out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="gsa_basepack_")
    o = Out(tmp)
    overview(a.web_data, o)
    ntiles = regional(a.web_data, o)
    o.close()
    parts = []
    # overview, zoom 5-9
    p = os.path.join(tmp, "ov.pmtiles")
    tippe(p, 5, 9, [(k[3:], o.path(k)) for k in ("ov-road", "ov-rail", "ov-land", "ov-place") if k in o.files])
    parts.append(p)
    # regional, zoom 10-14, road classes from their own zoom
    first = [(k, o.path(k)) for k in ("rail", "land", "poi", "place") if k in o.files]
    p = os.path.join(tmp, "r10.pmtiles")
    tippe(p, 10, 14, first + ([("road", o.path("road10"))] if "road10" in o.files else []))
    parts.append(p)
    for z in sorted({v for v in ROAD_MINZ.values() if v > 10}):
        if f"road{z}" not in o.files:
            continue
        p = os.path.join(tmp, f"r{z}.pmtiles")
        tippe(p, z, 14, [("road", o.path(f"road{z}"))])
        parts.append(p)
    out = os.path.join(a.out, "base.pmtiles")
    subprocess.run(["tile-join", "-o", out, "--force", "-q", "--no-tile-size-limit", "-n", "Grid of SA base map",
                    "-A", "© OpenStreetMap contributors (ODbL)", *parts], check=True)
    print(f"base: {ntiles} regional tiles; " + ", ".join(f"{k} {v}" for k, v in sorted(o.n.items())) +
          f"  ({os.path.getsize(out) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
