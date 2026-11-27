"""Build desktop tile packs from the website map's data files, so the app draws the same layers.

    python tools/webpack/build_web_packs.py WEB_DATA_DIR OUT_DIR [--only GROUP ...]

WEB_DATA_DIR holds the website's published data/ folder (the files as the site serves them:
JSON, or a JSON string of base64 gzip). OUT_DIR gets one PMTiles pack per layer group,
web-grid.pmtiles, web-water.pmtiles ... Each vector layer is named after the website's layer id
with ":" turned into "_" (x:pa -> x_pa), which is how the app finds it (layers.cpp).

Every feature carries `name` and `detail` (one line for cards and lists) and, where it helps,
numbers the app can colour or rank by. Personal data stays out: no owner or rights-holder names,
no phone numbers, no ID numbers (POPIA). Needs tippecanoe.
"""
import argparse, base64, gzip, json, os, subprocess, sys, tempfile

GROUPS = ["grid", "water", "risk", "nature", "vote", "prop", "land", "places", "move"]


def load(d, rel):
    p = os.path.join(d, rel)
    if not os.path.exists(p):
        print(f"  missing {rel}, skipped", file=sys.stderr)
        return None
    j = json.load(open(p, encoding="utf-8"))
    if isinstance(j, str):
        j = json.loads(gzip.decompress(base64.b64decode(j)))
    return j


def pt(lon, lat):
    return {"type": "Point", "coordinates": [round(float(lon), 6), round(float(lat), 6)]}


def line(coords):
    return {"type": "LineString", "coordinates": coords}


def clean(p):
    return {k: v for k, v in p.items() if v is not None and v != ""}


def feats_fc(fc, props):
    for f in (fc or {}).get("features", []):
        if not f.get("geometry"):
            continue
        yield clean(props(f.get("properties") or {})), f["geometry"]


def fmt(v, unit=""):
    if v is None:
        return ""
    v = float(v)
    s = f"{v:,.0f}" if abs(v) >= 10 else f"{v:.1f}"
    return (s + " " + unit).strip()


def join(*parts):
    return " · ".join(str(p) for p in parts if p not in (None, "", "None"))


# ------------------------------------------------------------------ layers per group
# Each builder yields (layer_id, props, geometry). Zooms: (minzoom, maxzoom) of the pack.

def grid(d):
    p = load(d, "transformers.json") or []
    for t in p:
        yield "trafos", clean({"name": t.get("n") or "Transformer", "detail": join(t.get("vs") and f"{t['vs']} V", t.get("o"))}), pt(*t["c"])
    z = load(d, "zones.json")
    for props, g in feats_fc(z, lambda q: {"name": q.get("n") or "Service area", "kv": q.get("v"), "km2": q.get("km2"),
                                           "detail": join(q.get("v") and f"{q['v']} kV", q.get("o"), q.get("km2") and f"{q['km2']} km²")}):
        yield "zones", props, g
    s = load(d, "x/solar.json")
    if s:
        for props, g in feats_fc(s.get("plants"), lambda q: {"name": q.get("n") or "Solar plant", "mw": q.get("mw"),
                                                             "detail": join(q.get("m"), q.get("mw") and fmt(q["mw"], "MW"), q.get("st"))}):
            yield "x_splants", props, g
        for props, g in feats_fc(s.get("apps"), lambda q: {"name": q.get("t") or "Application", "mw": q.get("mw"),
                                                           "detail": join(q.get("tech"), q.get("s"), q.get("r"))}):
            yield "x_reea", props, g
        for props, g in feats_fc(s.get("redz"), lambda q: {"name": q.get("n"), "detail": "REDZ" if q.get("k") == "redz" else "Power corridor"}):
            yield "x_redz", props, g
        for r in s.get("emb", []):
            yield "x_sembed", {"name": "Small solar array", "detail": "OSM"}, pt(r[0], r[1])
        cols = s.get("evf", [])
        for r in s.get("ev", []):
            e = dict(zip(cols, r))
            yield "x_ev", clean({"name": e.get("name") or "EV charger", "kw": float(e["max_kw"]) if str(e.get("max_kw") or "").replace(".", "").isdigit() else None,
                                 "detail": join(e.get("operator"), e.get("max_kw") and f"{e['max_kw']} kW", e.get("status"))}), pt(e["lon"], e["lat"])
    g = load(d, "x/gcca_zones.json")
    for props, geo in feats_fc(g, lambda q: {"name": q.get("n"), "mva": q.get("mva"),
                                             "detail": join(q.get("s"), q.get("l"), q.get("mva") is not None and fmt(q["mva"], "MVA"))}):
        yield "x_gcca", props, geo


def water(d):
    w = load(d, "water.json")
    if w:
        for r in w["rivers"]:
            yield "rivers", clean({"name": r.get("n"), "k": r.get("k"), "detail": "River" if not r.get("k") else "Stream"}), line(r["p"])
        for props, g in feats_fc(w["bodies"], lambda q: {"name": q.get("n"), "ha": q.get("ha"), "detail": join(q.get("ty"), q.get("ha") and f"{q['ha']} ha")}):
            yield "bodies", props, g
        for i in w["infra"]:
            yield "winfra", clean({"name": i.get("n") or i.get("t", "").replace("_", " ").capitalize(), "detail": join(i.get("t", "").replace("_", " "), i.get("o"))}), pt(*i["c"])
    dm = load(d, "x/dams.json")
    if dm:
        for r in dm["rows"]:
            yield "x_damlvl", clean({"name": r[2], "pct": r[5], "mcm": r[4], "detail": join(r[3], r[5] is not None and f"{r[5]}% full", r[15])}), pt(r[0], r[1])
    sv = load(d, "x/wsvc.json")
    if sv:
        cls = sv["classes"]
        for r in sv["rows"]:
            c = cls[int(r[2])] if isinstance(r[2], (int, float)) and int(r[2]) < len(cls) else ""
            yield "x_wsvc", clean({"name": r[3] or c.replace("_", " ").capitalize(), "detail": join(c.replace("_", " "), r[4])}), pt(r[0], r[1])
        for r in sv["gd_critical"]:
            yield "x_gdcrit", clean({"name": r[2], "detail": join(r[3], r[4], r[5] and f"Green Drop risk {r[5]}")}), pt(r[0], r[1])


def risk(d):
    for props, g in feats_fc(load(d, "x/hz_dolo.json"), lambda q: {"name": "Dolomite", "detail": join(q.get("d"), "dolomite land")}):
        yield "x_dolo", props, g
    q = load(d, "x/hz_quakes.json")
    if q:
        for r in q["rows"]:
            yield "x_quakes", clean({"name": f"M{r[3]}", "mag": r[3], "detail": join(f"Magnitude {r[3]}", r[4])}), pt(r[0], r[1])


def nature(d):
    pa = load(d, "x/env_pa.json")
    if pa:
        for key in ("pa", "ca", "mz"):
            for props, g in feats_fc(pa.get(key), lambda q: {"name": q.get("n"), "detail": join(q.get("t"), q.get("s"), q.get("d"))}):
                yield "x_pa", props, g
    for lid, f, fn in [
        ("x_cba", "x/env_cba.json", lambda q: {"name": q.get("c"), "detail": join(q.get("c"), q.get("s"))}),
        ("x_cbaq", "x/env_cbaq.json", lambda q: {"name": q.get("c"), "detail": join(q.get("c"), q.get("s"))}),
        ("x_threat", "x/env_threat.json", lambda q: {"name": q.get("e"), "status": q.get("s"), "detail": join(q.get("e"), {"CR": "Critically endangered", "EN": "Endangered", "VU": "Vulnerable"}.get(q.get("s"), q.get("s")))}),
        ("x_wetl", "x/env_wetl.json", lambda q: {"name": q.get("f") or "Wetland", "detail": q.get("f")}),
        ("x_forest", "x/env_forest.json", lambda q: {"name": (q.get("t") or "Forest").split("_")[-1], "detail": (q.get("t") or "").replace("_", ": ")}),
        ("x_fepa", "x/env_fepa.json", lambda q: {"name": q.get("f"), "detail": q.get("t")}),
        ("x_swsa", "x/env_swsa.json", lambda q: {"name": (q.get("n") or "").split("_")[-1], "detail": "Strategic water source area"}),
        ("x_paexp", "x/env_paexp.json", lambda q: {"name": (q.get("n") or "").strip(), "detail": "Protected area expansion focus"}),
    ]:
        for props, g in feats_fc(load(d, f), fn):
            yield lid, props, g


PARTY_SHORT = {"AFRICAN NATIONAL CONGRESS": "ANC", "DEMOCRATIC ALLIANCE": "DA", "ECONOMIC FREEDOM FIGHTERS": "EFF",
               "INKATHA FREEDOM PARTY": "IFP", "UMKHONTO WESIZWE": "MK", "ACTIONSA": "ActionSA", "PATRIOTIC ALLIANCE": "PA"}


def vote(d):
    w = load(d, "wards.json")
    for props, g in feats_fc(w, lambda q: {"name": f"Ward {q.get('no')}, {q.get('m')}", "id": q.get("id"), "muni": q.get("mc"),
                                           "party": PARTY_SHORT.get((q.get("w") or "").upper(), (q.get("w") or "").title()),
                                           "turnout": round(q["t"] / q["reg"] * 100, 1) if q.get("t") and q.get("reg") else None,
                                           "detail": join(PARTY_SHORT.get((q.get("w") or "").upper(), (q.get("w") or "").title()),
                                                          q.get("wv") and f"{q['wv']:,} votes", q.get("t") and q.get("reg") and f"turnout {q['t'] / q['reg'] * 100:.0f}%")}):
        yield "wards", props, g


def prop(d):
    for props, g in feats_fc(load(d, "x/planning.json"), lambda q: {"name": q.get("l"), "theme": q.get("th"), "detail": join(q.get("a"), q.get("s"))}):
        yield "x_sdf", props, g
    for props, g in feats_fc(load(d, "x/suburbs.json"), lambda q: {"name": q.get("n"), "pop": q.get("p"), "dens": q.get("d"),
                                                                   "val": (q.get("v") or [None, None])[1],
                                                                   "detail": join(q.get("p") and f"{q['p']:,} people", q.get("d") and f"{q['d']:,.0f} per km²",
                                                                                  (q.get("v") or [None, None])[1] and f"R {q['v'][1]:,} median")}):
        yield "x_sub", props, g
    n = load(d, "x/near.json")
    if n:
        for r in n["rows"]:
            yield "x_near", clean({"name": f"Erf {r[3]}" if r[3] else "Plot", "kv": r[9], "m": r[7],
                                   "detail": join(r[9] and f"{r[9]} kV line", r[7] is not None and f"{r[7]:.0f} m away", r[11])}), pt(r[0], r[1])


def land(d):
    m = load(d, "x/mining.json")
    if m:
        ty = m.get("ty", [])
        # the rights holder ("h") is left out: it can name a private person
        for props, g in feats_fc(m["fc"], lambda q: {"name": ty[q["t"]] if isinstance(q.get("t"), int) and q["t"] < len(ty) else "Mining right",
                                                     "detail": join(q.get("r"), q.get("s"), q.get("l"))}):
            yield "x_mining", props, g


def places(d):
    c = load(d, "x/crime.json")
    if c:
        for props, g in feats_fc(c, lambda q: {"name": q.get("s"), "pop": q.get("pop"), "detail": join(q.get("d"), q.get("pr"))}):
            yield "x_crime", props, g
    mx = load(d, "x/mastx.json")
    if mx:
        for r in mx["rows"]:
            yield "x_mastx", clean({"name": r[2] or "Mast", "h": r[5], "detail": join(r[4], r[5] and f"{r[5]:.0f} m", r[11] and r[11].lower())}), pt(r[0], r[1])
    t = load(d, "x/telecom.json")
    if t:
        ops = t.get("ops", [])
        for r in t["sites"]:
            yield "x_cells", clean({"name": ops[int(r[2])] if int(r[2]) < len(ops) else "Cell", "detail": join(ops[int(r[2])] if int(r[2]) < len(ops) else "", f"seen {int(r[6])}")}), pt(r[0], r[1])
        for r in t["pts"]:
            yield "x_masts", clean({"name": r[3] or r[2], "detail": join(r[2], r[4])}), pt(r[0], r[1])
        for r in t["wifi"]:
            yield "x_wifi", clean({"name": r[2], "detail": join(r[3], r[4])}), pt(r[0], r[1])
    for props, g in feats_fc(load(d, "x/fibre2.json"), lambda q: {"name": q.get("n"), "k": q.get("k"), "detail": join(f"{q.get('k')} fibre networks", ", ".join(q.get("w") or [])[:80])}):
        yield "x_fibre2", props, g


def move(d):
    a = load(d, "x/air.json")
    if a:
        for r in a["ap"]:
            yield "x_air", clean({"name": r[3], "iata": r[4], "detail": join(r[4], r[5], r[6])}), pt(r[0], r[1])
        for r in a["rt"]:
            yield "x_flights", clean({"name": f"{r[2]} to {r[3]}", "n": r[7], "detail": join(f"{r[4]} to {r[5]}", r[7] and f"{r[7]:,} flights")}), line([r[0], r[1]])
        for r in a["ports"]:
            yield "x_ships", clean({"name": r[2], "detail": join(r[4], r[6])}), pt(r[0], r[1])
    t = load(d, "x/traffic.json")
    if t:
        for r in t["s"]:
            # delta-coded 1e-5 degrees, as the website decodes them
            g, ll, x, y = r[0], [], 0, 0
            for k in range(0, len(g) - 1, 2):
                x += g[k]
                y += g[k + 1]
                ll.append([x / 1e5, y / 1e5])
            if len(ll) < 2:
                continue
            yield "x_traffic", clean({"name": join(r[4], r[6] and r[7] and f"{r[6]} to {r[7]}"), "adt": r[1], "detail": join(r[1] and f"{r[1]:,} vehicles a day", r[2])}), line(ll)
        for r in t.get("tolls", []):
            yield "x_tolls", clean({"name": f"{r[2]} toll", "detail": join(r[3], r[4])}), pt(r[0], r[1])


BUILDERS = {"grid": grid, "water": water, "risk": risk, "nature": nature, "vote": vote, "prop": prop, "land": land,
            "places": places, "move": move}
ZOOMS = {"grid": (3, 12), "water": (3, 12), "risk": (3, 11), "nature": (3, 11), "vote": (3, 12), "prop": (5, 13),
         "land": (5, 12), "places": (5, 13), "move": (3, 11)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("web_data")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*", default=GROUPS)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="gsa_webpack_")
    for g in a.only:
        files, counts = {}, {}
        for lid, props, geom in BUILDERS[g](a.web_data):
            if lid not in files:
                files[lid] = open(os.path.join(tmp, f"{g}-{lid}.geojsonl"), "w", encoding="utf-8")
            files[lid].write(json.dumps({"type": "Feature", "properties": props, "geometry": geom}, ensure_ascii=False) + "\n")
            counts[lid] = counts.get(lid, 0) + 1
        for f in files.values():
            f.close()
        if not files:
            print(f"web-{g}: nothing to build")
            continue
        z0, z1 = ZOOMS[g]
        out = os.path.join(a.out, f"web-{g}.pmtiles")
        cmd = ["tippecanoe", "-o", out, "--force", "-q", "-Z", str(z0), "-z", str(z1), "-n", f"Grid of SA {g} (website layers)",
               "-A", "Website map data; sources listed per layer in the app", "--drop-densest-as-needed",
               "--extend-zooms-if-still-dropping", "--simplification=4", "--detect-shared-borders"]
        for lid in files:
            cmd += ["-L", json.dumps({"file": os.path.join(tmp, f"{g}-{lid}.geojsonl"), "layer": lid})]
        subprocess.run(cmd, check=True)
        print(f"web-{g}: " + ", ".join(f"{k} {v}" for k, v in counts.items()) + f"  ({os.path.getsize(out) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
