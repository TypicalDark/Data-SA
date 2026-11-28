"""Build the small development data pack in devdata/packs/ from the UI prototype's data bundle.

    python tools/devpack/make_devpack.py PROTOTYPE_DATA_JSON [--out devdata/packs]

PROTOTYPE_DATA_JSON is grid-of-sa/design/ui-research-2026/prototype/data.json from the project
files (213 municipalities with simplified outlines, Census 2022 population, median roll values,
2026 ward winners, the power grid graph, GCCA 2025 substation capacity, dams, airports, ports).

It writes the same formats the real pipeline (grid-of-sa/datastore/export/) produces, so the
back-end and app cannot tell the difference:
    admin.pmtiles   layers province, municipality, label   (tippecanoe)
    grid.pmtiles    layers line, facility                   (tippecanoe)
    pack.sqlite     tree, metric, source, search (FTS5), pack_info  (gsa-pack/1, export_pack.py schema)

Needs tippecanoe and shapely. The real packs come from PostGIS; this one exists so the app,
tests and CI run without the 30 GB database. No owner names or ID numbers exist in the input.
"""
import argparse, json, os, sqlite3, subprocess, sys, tempfile
from shapely.geometry import Polygon, mapping, shape
from shapely.ops import unary_union

PROV = {"EC": "Eastern Cape", "FS": "Free State", "GT": "Gauteng", "KZN": "KwaZulu-Natal", "LIM": "Limpopo",
        "MP": "Mpumalanga", "NC": "Northern Cape", "NW": "North West", "WC": "Western Cape"}
AS_OF = {"pop_est": "2022-10-01", "value_median": "2025-07-01", "parcels": "2025-07-01"}
SRC = {"pop_est": "statssa_census_2022", "value_median": "valuation_rolls", "parcels": "valuation_rolls"}
SOURCES = [
    ("statssa_census_2022", "Census 2022 population", "Stats SA", "https://census.statssa.gov.za", "Stats SA terms", 1, "10 years", "aggregate"),
    ("valuation_rolls", "Municipal valuation rolls", "213 municipalities", "", "Municipal copyright (MPRA s85)", 0, "4-5 years + supplementaries", "no owner names or ID numbers"),
    ("iec_lge_2026", "2026 local government election ward results", "IEC", "https://results.elections.org.za", "IEC terms", 1, "per election", "aggregate"),
    ("osm_power", "Power lines and substations", "OpenStreetMap contributors", "https://www.openstreetmap.org", "ODbL 1.0", 1, "daily", "none"),
    ("ntcsa_gcca_2025", "Generation connection capacity assessment 2025", "NTCSA", "https://www.ntcsa.co.za", "NTCSA publication", 1, "yearly", "none"),
    ("mdb_2021", "Municipal boundaries (simplified for development)", "Municipal Demarcation Board", "https://www.demarcation.org.za", "MDB terms", 1, "per delimitation", "none"),
]


def ring_poly(r):
    p = Polygon(r)
    return p if p.is_valid else p.buffer(0)


def tippecanoe(out, layers, zmin, zmax, name):
    cmd = ["tippecanoe", "-o", out, "--force", "-q", "-Z", str(zmin), "-z", str(zmax), "-n", name,
           "-A", "Development pack; see devdata/README.md for sources", "--no-tile-size-limit", "-r1"]
    for lname, path, extra in layers:
        cmd += ["-L", json.dumps({"file": path, "layer": lname})]
        cmd += extra
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_json")
    ap.add_argument("--out", default="devdata/packs")
    a = ap.parse_args()
    d = json.load(open(a.data_json, encoding="utf-8"))
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="gsa_devpack_")

    munis, by_prov = [], {}
    for m in d["munis"]:
        g = ring_poly(m["r"])
        dens = (m["pop"] / m["km2"]) if m.get("pop") and m.get("km2") else None
        munis.append((m, g, dens))
        by_prov.setdefault(m["p"], []).append((m, g))

    def wl(path, feats):
        with open(path, "w", encoding="utf-8") as f:
            for props, geom, minzoom in feats:
                o = {"type": "Feature", "properties": props, "geometry": geom}
                # The per-feature {"tippecanoe": {"minzoom": n}} key makes tippecanoe 2.49 (Ubuntu 24.04)
                # drop all but one feature per tile, so zoom gates are applied by the app instead
                # and minzoom is kept as a property.
                if minzoom is not None:
                    props = dict(props, minzoom=minzoom)
                    o["properties"] = props
                f.write(json.dumps(o, separators=(",", ":")) + "\n")
        return path

    # ---- admin.pmtiles
    prov_feats, muni_feats, label_feats = [], [], []
    tree, metric, search = [], [], []
    za_pop = 0
    tree_rows = {}
    for pc, items in sorted(by_prov.items()):
        u = unary_union([g for _, g in items]).buffer(0.002).buffer(-0.002)
        pop = sum(m.get("pop") or 0 for m, _ in items)
        za_pop += pop
        props = {"id": pc, "name": PROV[pc], "pop_est": pop}
        prov_feats.append((props, mapping(u), None))
        c = u.representative_point()
        label_feats.append(({"id": pc, "name": PROV[pc], "level": 1}, mapping(c), 3))
        x0, y0, x1, y1 = u.bounds
        tree_rows[pc] = (pc, 1, "ZA", f"ZA.{pc}", PROV[pc], x0, y0, x1, y1, c.x, c.y, round(sum(m["km2"] or 0 for m, _ in items), 1))
        metric.append((pc, "pop_est", AS_OF["pop_est"], pop, len(items), SRC["pop_est"]))
        search.append((f"area:{pc}", PROV[pc], "Province, South Africa", 9, c.x, c.y))
    allu = unary_union([g for _, g, _ in munis])
    x0, y0, x1, y1 = allu.bounds
    tree_rows["ZA"] = ("ZA", 0, None, "ZA", "South Africa", x0, y0, x1, y1, 25.2, -28.8, round(sum(m["km2"] or 0 for m, _, _ in munis), 1))
    metric.append(("ZA", "pop_est", AS_OF["pop_est"], za_pop, len(munis), SRC["pop_est"]))

    for m, g, dens in munis:
        props = {"id": m["c"], "name": m["n"], "parent": m["p"]}
        if m.get("pop"): props["pop_est"] = m["pop"]
        if dens is not None: props["density"] = round(dens, 1)
        if m.get("med"): props["value_median"] = m["med"]
        if m.get("nv"): props["parcels"] = m["nv"]
        if m.get("win"):
            props["win"], props["win_wards"], props["wards"] = m["win"][0], m["win"][1], m["win"][2]
        muni_feats.append((props, mapping(g), None))
        c = g.representative_point()
        label_feats.append(({"id": m["c"], "name": m["n"], "level": 3}, mapping(c), 6))
        bx0, by0, bx1, by1 = g.bounds
        tree_rows[m["c"]] = (m["c"], 3, m["p"], f"ZA.{m['p']}.{m['c']}", m["n"], bx0, by0, bx1, by1, c.x, c.y, m.get("km2"))
        for k, v, n in (("pop_est", m.get("pop"), None), ("value_median", m.get("med"), m.get("nv")), ("parcels", m.get("nv"), None)):
            if v:
                metric.append((m["c"], k, AS_OF[k], v, n, SRC[k]))
        search.append((f"area:{m['c']}", m["n"], f"Municipality, {PROV[m['p']]}", 7, c.x, c.y))

    tippecanoe(os.path.join(a.out, "admin.pmtiles"), [
        ("province", wl(os.path.join(tmp, "province.geojsonl"), prov_feats), []),
        ("municipality", wl(os.path.join(tmp, "municipality.geojsonl"), muni_feats), []),
        ("label", wl(os.path.join(tmp, "label.geojsonl"), label_feats), []),
    ], 0, 10, "Grid of SA admin (dev)")

    # ---- grid.pmtiles
    line_feats = []
    for kv, inferred, km, g in d["lines"]:
        mz = 0 if kv >= 220 else 5 if kv >= 66 else 8
        line_feats.append(({"kv": kv, "inferred": int(bool(inferred)), "km": km}, {"type": "LineString", "coordinates": g}, mz))
    fac_feats = []
    caps = {(round(c[0], 2), round(c[1], 2)): c for c in d["cap"]}
    for i, (lon, lat, kv, name, _x, op, deg) in enumerate(d["fac"]):
        p = {"id": f"sub{i}", "kind": "substation", "kv": kv, "name": name, "operator": op}
        cap = caps.get((round(lon, 2), round(lat, 2)))
        if cap and cap[3] is not None:
            p["free_mw"] = cap[3]
        fac_feats.append((p, {"type": "Point", "coordinates": [lon, lat]}, 4 if kv >= 220 else 7))
        if name:
            search.append((f"node:power:sub{i}", name, f"substation {kv} kV {op or ''}".strip(), 4, lon, lat))
    for i, (lon, lat, mw, name, fuel, op) in enumerate(d["st"]):
        fac_feats.append(({"id": f"st{i}", "kind": "power_station", "mw": mw, "fuel": fuel, "name": name, "operator": op},
                          {"type": "Point", "coordinates": [lon, lat]}, 4 if mw >= 500 else 6))
        if name:
            search.append((f"node:power:st{i}", name, f"power station {fuel} {round(mw)} MW", 5, lon, lat))
    tippecanoe(os.path.join(a.out, "grid.pmtiles"), [
        ("line", wl(os.path.join(tmp, "line.geojsonl"), line_feats), []),
        ("facility", wl(os.path.join(tmp, "facility.geojsonl"), fac_feats), ["-r1"]),
    ], 0, 10, "Grid of SA power grid (dev)")

    # ---- pack.sqlite (export_pack.py schema subset)
    out = os.path.join(a.out, "pack.sqlite")
    if os.path.exists(out): os.remove(out)
    db = sqlite3.connect(out)
    db.executescript("""
    CREATE TABLE tree (area_id TEXT PRIMARY KEY, level INT, parent_id TEXT, path TEXT, name TEXT, minx REAL, miny REAL, maxx REAL, maxy REAL, lon REAL, lat REAL, area_km2 REAL);
    CREATE TABLE metric (area_id TEXT, metric TEXT, as_of TEXT, v REAL, n INT, source_id TEXT, PRIMARY KEY (area_id, metric, as_of));
    CREATE TABLE source (source_id TEXT PRIMARY KEY, title TEXT, publisher TEXT, url TEXT, licence TEXT, resale_ok INT, cadence TEXT, privacy TEXT);
    CREATE VIRTUAL TABLE search USING fts5(ref UNINDEXED, label, detail, weight UNINDEXED, lon UNINDEXED, lat UNINDEXED, tokenize='unicode61 remove_diacritics 2');
    CREATE TABLE pack_info (k TEXT PRIMARY KEY, v TEXT);
    CREATE INDEX tree_parent ON tree(parent_id);
    """)
    db.executemany("INSERT INTO tree VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", list(tree_rows.values()))
    db.executemany("INSERT INTO metric VALUES (?,?,?,?,?,?)", metric)
    db.executemany("INSERT INTO source VALUES (?,?,?,?,?,?,?,?)", SOURCES)
    db.executemany("INSERT INTO search VALUES (?,?,?,?,?,?)", search)
    db.executemany("INSERT INTO pack_info VALUES (?,?)", [("format", "gsa-pack/1"), ("built_from", "ui prototype data.json"),
                                                          ("munis", "all"), ("kind", "dev")])
    db.commit(); db.execute("VACUUM"); db.close()
    for f in ("admin.pmtiles", "grid.pmtiles", "pack.sqlite"):
        print(f"  {f}: {os.path.getsize(os.path.join(a.out, f)) / 1e6:.2f} MB")
    print(f"  tree {len(tree_rows)} areas, {len(metric)} metrics, {len(search)} search rows")


if __name__ == "__main__":
    sys.exit(main())
