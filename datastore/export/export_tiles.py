"""Grid of SA: export the data store to PMTiles packs (the zoom pyramid the map and the back-end read).

    apt install tippecanoe   (provides tippecanoe and tile-join; on Windows run it in WSL)
    python export_tiles.py OUT_DIR [--pack admin,grid,parcels,h3,places] [--dsn "dbname=gsa"] [--bbox minlon,minlat,maxlon,maxlat]

Output: OUT_DIR/<pack>.pmtiles, plus parcels-<prov>.pmtiles per province (parcels are split so a user
installs only the provinces they need and an update ships one province at a time).

Zoom bands. Every layer query returns (id, props, geom, minzoom, maxzoom). Rows are grouped by their
(minzoom, maxzoom) band and each band goes through its own tippecanoe run with -Z/-z set to the band;
tile-join then merges all bands and layers into the pack. We do NOT use tippecanoe's per-feature
"tippecanoe": {"minzoom": n} member: tippecanoe 2.49 (Ubuntu 24.04) keeps only one feature per tile
when it is present (found while building the desktop dev pack; see docs/DATA.md). The band's minzoom
is also written as a `minzoom` property, so the app can gate drawing itself.
"""
import argparse, json, os, shutil, subprocess, sys, tempfile
import psycopg

ATTRIB = "Grid of SA data store; credits per source in /v1/sources"
AREA_METRICS = """LEFT JOIN LATERAL (SELECT jsonb_object_agg(metric, round(v::numeric,1)) m FROM (
      SELECT DISTINCT ON (metric) metric, v FROM agg.area_metric WHERE area_id=a.area_id ORDER BY metric, as_of DESC) x) m ON true"""

def admin(level, minz, maxz, geom="a.geom"):
    return f"""SELECT a.area_id, jsonb_build_object('id',a.area_id,'name',a.name,'parent',a.parent_id) || coalesce(m.m,'{{}}'),
                      {geom}, {minz}, {maxz}
               FROM core.admin_area a {AREA_METRICS} WHERE a.level={level} AND a.valid_to IS NULL"""

# name, pack, sql (id, props, geom, minzoom, maxzoom), extra tippecanoe flags
LAYERS = [
 ("province", "admin", admin(1, 0, 12, "a.geom_z10"), ["--no-tiny-polygon-reduction"]),
 ("district", "admin", admin(2, 4, 12, "a.geom_z10"), []),
 ("municipality", "admin", admin(3, 5, 12, "a.geom_z10"), []),
 ("ward", "admin", admin(4, 8, 14), []),
 ("suburb", "admin", admin(6, 11, 14), []),
 ("label", "admin", """SELECT a.area_id, jsonb_build_object('id',a.area_id,'name',a.name,'level',a.level), a.label_pt,
        CASE a.level WHEN 1 THEN 3 WHEN 2 THEN 6 WHEN 3 THEN 7 WHEN 4 THEN 11 ELSE 13 END, 14
   FROM core.admin_area a WHERE a.level BETWEEN 1 AND 6 AND a.name IS NOT NULL AND a.valid_to IS NULL""", ["-r1"]),
 ("line", "grid", """SELECT e.edge_id::text, jsonb_strip_nulls(jsonb_build_object('id',e.edge_id,'kv',e.kv[1],'cls',e.cls,'inferred',e.inferred,
        'source',e.source,'target',e.target)), e.geom,
        CASE WHEN e.kv[1] >= 220 THEN 0 WHEN e.kv[1] >= 66 THEN 5 WHEN e.kv[1] >= 22 THEN 8 ELSE 10 END, 14
   FROM net.edge e WHERE e.network='power'""", []),
 ("facility", "grid", """SELECT n.node_id::text, jsonb_strip_nulls(jsonb_build_object('id',n.node_id,'name',n.name,'kind',n.kind,
        'kv',n.kv[array_upper(n.kv,1)],'operator',n.operator,'mva',n.capacity_mva)), n.geom,
        CASE WHEN n.kv[array_upper(n.kv,1)] >= 220 OR n.kind='power_station' THEN 4 WHEN n.kv[array_upper(n.kv,1)] >= 66 THEN 7 ELSE 10 END, 14
   FROM net.node n WHERE n.network='power' AND n.kind <> 'endpoint'""", ["-r1"]),
 ("parcel", "parcels", """SELECT p.sg21, jsonb_strip_nulls(jsonb_build_object('id',p.sg21,'kind',p.kind,'ward',p.ward_id,'area_m2',round(p.area_m2::numeric),
        'value',v.value,'sale',v.sale,'cat',v.cat)), p.geom, 13, 16
   FROM core.parcel p LEFT JOIN LATERAL (SELECT sum(market_value) value, max(sale_price) sale, max(category) cat FROM core.valuation
        WHERE sg21=p.sg21 AND coalesce(value_flag,'')<>'zero') v ON true {split}""", ["--no-feature-limit", "--no-tile-size-limit"]),
 ("hex", "h3", """SELECT h.h3::text || ':' || h.metric, jsonb_build_object('h3',h.h3::text,'res',h.res,'metric',h.metric,'v',round(h.v::numeric),'n',h.n),
        h3_cell_to_boundary_geometry(h.h3),
        CASE h.res WHEN 5 THEN 4 WHEN 6 THEN 6 WHEN 7 THEN 8 WHEN 8 THEN 10 ELSE 11 END,
        CASE h.res WHEN 5 THEN 5 WHEN 6 THEN 7 WHEN 7 THEN 9 WHEN 8 THEN 10 ELSE 13 END
   FROM agg.h3_metric h""", ["--no-tiny-polygon-reduction"]),
 ("place", "places", """SELECT pl.place_id, jsonb_build_object('id',pl.place_id,'name',pl.name,'kind',pl.kind) || pl.props, pl.geom, 5, 14
   FROM core.place pl""", ["-r1"]),
]
# Packs split into one file per province (pack id <pack>-<prov>, lower case).
SPLIT = {"parcels": """WHERE p.muni_id IN (SELECT area_id FROM core.admin_area WHERE path <@ text2ltree('ZA.{prov}'))"""}
PROVS = ["EC", "FS", "GT", "KZN", "LIM", "MP", "NC", "NW", "WC"]

def seed(db):
    with db.cursor() as c:
        for name, pack, sql, flags in LAYERS:
            c.execute("""INSERT INTO meta.tile_layer (layer,pack,source_sql,minzoom,maxzoom,tippecanoe) VALUES (%s,%s,%s,0,16,%s)
                         ON CONFLICT (layer) DO UPDATE SET pack=EXCLUDED.pack, source_sql=EXCLUDED.source_sql, tippecanoe=EXCLUDED.tippecanoe""",
                      (name, pack, sql, " ".join(flags)))
    db.commit()

def write_bands(db, layer, sql, tmp, bbox):
    """Stream one layer into one GeoJSONSeq file per zoom band. Returns {(zmin, zmax): (path, n)}."""
    where = f" WHERE ST_Intersects(q.geom, ST_MakeEnvelope({bbox},4326))" if bbox else ""
    q = f"SELECT q.id, q.props, ST_AsGeoJSON(q.geom, 6), q.zmin, q.zmax FROM ({sql}) q(id, props, geom, zmin, zmax){where}"
    files, out = {}, {}
    try:
        with db.cursor(name=f"c_{layer}") as c:
            c.itersize = 5000
            c.execute(q)
            for fid, props, gj, zmin, zmax in c:
                if gj is None: continue
                band = (int(zmin), int(zmax))
                if band not in files:
                    p = os.path.join(tmp, f"{layer}_z{band[0]}-{band[1]}.geojsonl")
                    files[band] = open(p, "w", encoding="utf-8"); out[band] = [p, 0]
                props = dict(props or {}); props["minzoom"] = band[0]
                files[band].write(json.dumps({"type": "Feature", "properties": props, "geometry": json.loads(gj)},
                                             ensure_ascii=False, separators=(",", ":")) + "\n")
                out[band][1] += 1
    finally:
        for f in files.values(): f.close()
        db.rollback()
    return {k: tuple(v) for k, v in out.items()}

def build_pack(db, pack_id, layers, out_dir, tmp, bbox, split_where=""):
    parts = []
    for name, _, sql, flags in layers:
        bands = write_bands(db, name, sql.replace("{split}", split_where), tmp, bbox)
        for (zmin, zmax), (path, n) in sorted(bands.items()):
            print(f"  {pack_id}/{name} z{zmin}-{zmax}: {n} features", flush=True)
            part = os.path.join(tmp, f"{pack_id}__{name}_z{zmin}-{zmax}.pmtiles")
            cmd = ["tippecanoe", "-o", part, "--force", "-q", "-l", name, "-Z", str(zmin), "-z", str(zmax),
                   "--read-parallel", "--detect-shared-borders", "--simplification=4",
                   "--drop-densest-as-needed", "--extend-zooms-if-still-dropping"] + flags + [path]
            subprocess.run(cmd, check=True)
            parts.append(part)
    if not parts:
        print(f"  {pack_id}: no features, skipped"); return None
    dest = os.path.join(out_dir, f"{pack_id}.pmtiles")
    subprocess.run(["tile-join", "-o", dest, "--force", "-q", "--no-tile-size-limit", "-n", f"Grid of SA {pack_id}", "-A", ATTRIB] + parts,
                   check=True)
    print(f"  wrote {pack_id}.pmtiles {os.path.getsize(dest)/1e6:.1f} MB", flush=True)
    return dest

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out"); ap.add_argument("--pack", default="", help="comma list of packs (default all)")
    ap.add_argument("--prov", default="", help="for split packs: only these provinces, e.g. WC,GT")
    ap.add_argument("--dsn", default=os.environ.get("GSA_DSN", "dbname=gsa")); ap.add_argument("--bbox")
    ap.add_argument("--keep-geojson", action="store_true")
    a = ap.parse_args()
    for t in ("tippecanoe", "tile-join"):
        if not shutil.which(t): sys.exit(f"{t} not found on PATH")
    os.makedirs(a.out, exist_ok=True)
    db = psycopg.connect(a.dsn); seed(db)
    want = [p for p in a.pack.split(",") if p]
    packs = {}
    for l in LAYERS:
        if not want or l[1] in want: packs.setdefault(l[1], []).append(l)
    tmp = tempfile.mkdtemp(prefix="gsa_tiles_")
    try:
        for pack, layers in packs.items():
            if pack in SPLIT:
                for prov in (a.prov.split(",") if a.prov else PROVS):
                    build_pack(db, f"{pack}-{prov.lower()}", layers, a.out, tmp, a.bbox, SPLIT[pack].format(prov=prov))
            else:
                build_pack(db, pack, layers, a.out, tmp, a.bbox)
    finally:
        if not a.keep_geojson: shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main()
