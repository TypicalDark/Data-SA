"""Grid of SA loader: put source files into the PostGIS data store (schema/001_core.sql).

    pip install "psycopg[binary]"          # the only dependency; geometry work happens inside PostGIS
    export GSA_DSN="dbname=gsa"            # or host=... user=... password=...
    python gsa_load.py sources  ARCHIVE_ROOT/_catalog/sources.json [--snapshots ARCHIVE_ROOT/_catalog/snapshots.jsonl]
    python gsa_load.py admin    MUNIS.geojson WARDS.geojson [--set MDB2021]
    python gsa_load.py suburbs  suburbs_population.geojson.gz
    python gsa_load.py population wards_population.csv
    python gsa_load.py parcels  muni/WC053.geojsonl.gz [more files ...]
    python gsa_load.py valuation parcel_values.csv.gz
    python gsa_load.py grid     grid_nodes_v2.geojson grid_edges_v2.geojson
    python gsa_load.py climate  climate_wards_monthly.csv.gz [--vars rain_mm_imerg,t_mean_c]
    python gsa_load.py dams     dam_levels_current.csv
Every command takes --snap SNAPSHOT_ID (repeatable) naming the raw-archive snapshot(s) the file came from;
without it the file's own path and sha256 go into meta.lineage, so the link back to the source is never lost.

Input files are read, never changed. Loaders are idempotent: each one replaces only the rows it owns.
POPIA rule: columns whose names look like owner names, ID numbers or contact details are dropped on read
(PRIVATE below), and the valuation roll 'unit' column is never read because some rolls put owner names there.
"""
import argparse, csv, gzip, hashlib, io, json, os, re, sys
import psycopg

PRIVATE = re.compile(r"OWNER|OWNR|SURNAME|FIRST_?NAME|INITIAL|ID_?NO|ID_?NUM|IDNUMBER|IDENTITY|PERSON|"
                     r"TEL|PHONE|CELL|EMAIL|POSTAL|ACCOUNT|ADDRESSEE|^UNIT$|^NAME_OF", re.I)
csv.field_size_limit(1 << 30)

def opn(p, mode="rt"):
    return gzip.open(p, mode, encoding="utf-8") if p.endswith(".gz") else open(p, mode, encoding="utf-8")

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def clean_props(d):
    return {k: v for k, v in d.items() if v not in (None, "") and not PRIVATE.search(k)}

def features(path):
    """Yield GeoJSON features from .geojson, .geojsonl, .geojsonseq (optionally .gz)."""
    with opn(path) as f:
        head = f.read(1); f.seek(0)
        first = f.readline()
        f.seek(0)
        if first.lstrip().startswith('{"type":"FeatureCollection"') or first.strip() == "{" or '"features"' in first[:200]:
            for ft in json.load(f)["features"]: yield ft
        else:
            for line in f:
                line = line.strip().lstrip("\x1e")
                if line: yield json.loads(line)

class DB:
    def __init__(self, dsn, snaps, script):
        self.c = psycopg.connect(dsn, autocommit=False)
        self.snaps, self.script = list(snaps or []), script
    def x(self, sql, args=None):
        with self.c.cursor() as cur:
            cur.execute(sql, args); return cur.rowcount
    def q(self, sql, args=None):
        with self.c.cursor() as cur:
            cur.execute(sql, args); return cur.fetchall()
    def copy_rows(self, table, cols, rows):
        n = 0
        with self.c.cursor() as cur, cur.copy(f"COPY {table} ({','.join(cols)}) FROM STDIN") as cp:
            for r in rows: cp.write_row(r); n += 1
        return n
    _h = {}
    def snap_ids(self, *paths):
        for p in paths:
            if p not in DB._h: DB._h[p] = sha(p)[:16]
        return self.snaps or [f"file:{os.path.basename(p)}#sha256={DB._h[p]}" for p in paths]
        return self.snaps or [f"file:{os.path.basename(p)}#sha256={sha(p)[:16]}" for p in paths]
    def lineage(self, target, rows, paths, notes=None):
        self.x("INSERT INTO meta.lineage (target, snapshot_ids, script, script_sha256, rows_written, notes) VALUES (%s,%s,%s,%s,%s,%s)",
               (target, self.snap_ids(*paths), self.script, sha(os.path.abspath(__file__))[:16], rows, notes))
    def commit(self): self.c.commit()

def stage_features(db, name, paths, keep=None):
    """COPY features into stage.<name>(props jsonb, gj text). keep = list of property names (None = all non-private)."""
    db.x(f"DROP TABLE IF EXISTS stage.{name}; CREATE UNLOGGED TABLE stage.{name} (props jsonb, gj text)")
    def rows():
        for p in paths:
            for ft in features(p):
                pr = clean_props(ft.get("properties") or {})
                if keep: pr = {k: pr[k] for k in keep if k in pr}
                yield (json.dumps(pr, ensure_ascii=False), json.dumps(ft["geometry"]) if ft.get("geometry") else None)
    n = db.copy_rows(f"stage.{name}", ["props", "gj"], rows())
    print(f"  staged {n} features into stage.{name}")
    return n

# ------------------------------------------------------------------ sources / snapshots
def cmd_sources(db, a):
    reg = json.load(open(a.files[0], encoding="utf-8"))
    for k, v in reg.items():
        db.x("""INSERT INTO meta.source (source_id,title,publisher,url,kind,licence,resale_ok,cadence,privacy,notes)
                VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
                ON CONFLICT (source_id) DO UPDATE SET title=EXCLUDED.title, publisher=EXCLUDED.publisher, url=EXCLUDED.url,
                  kind=EXCLUDED.kind, licence=EXCLUDED.licence, resale_ok=EXCLUDED.resale_ok, cadence=EXCLUDED.cadence,
                  privacy=EXCLUDED.privacy, notes=EXCLUDED.notes""",
             (k, v.get("title") or k, v.get("publisher"), v.get("url"), v.get("kind"), v.get("licence"), v.get("resale"),
              v.get("cadence"), v.get("privacy"), v.get("notes")))
    n = len(reg)
    if a.snapshots and os.path.exists(a.snapshots):
        for l in open(a.snapshots, encoding="utf-8"):
            s = json.loads(l)
            db.x("INSERT INTO meta.source (source_id,title,kind) VALUES (%s,%s,'unknown') ON CONFLICT DO NOTHING", (s["source_id"], s["source_id"]))
            db.x("""INSERT INTO meta.snapshot VALUES (%s,%s,to_timestamp(%s,'YYYYMMDD"T"HH24MISS"Z"'),%s,%s,%s,%s,%s,%s)
                    ON CONFLICT DO NOTHING""",
                 (s["snapshot_id"], s["source_id"], s["fetched_utc"], s["archive_path"], s["tree_sha256"], s["n_files"],
                  s["bytes"], s.get("same_as"), s.get("status")))
    print(f"  {n} sources registered")

# ------------------------------------------------------------------ admin tree (levels 0-4)
PROV = {"EC": "Eastern Cape", "FS": "Free State", "GT": "Gauteng", "GP": "Gauteng", "KZN": "KwaZulu-Natal", "LIM": "Limpopo",
        "MP": "Mpumalanga", "NW": "North West", "NC": "Northern Cape", "WC": "Western Cape"}
def cmd_admin(db, a):
    munis, wards = a.files[:2]
    stage_features(db, "munis", [munis]); stage_features(db, "wards", [wards])
    bs = a.set
    db.x("DELETE FROM agg.area_metric WHERE area_id IN (SELECT area_id FROM core.admin_area WHERE boundary_set=%s)", (bs,))
    db.x("UPDATE core.parcel SET ward_id=NULL, muni_id=NULL, suburb_id=NULL")
    db.x("DELETE FROM core.admin_area WHERE boundary_set=%s", (bs,))
    G = "ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_SetSRID(ST_GeomFromGeoJSON(gj),4326)),3))"
    # level 3: municipalities (metros sit directly under the province, as in MDB; district_id = NULL)
    db.x(f"""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom,props,snapshot_ids)
             SELECT props->>'cat_b', 3, props->>'cat_b', props->>'municname',
                    CASE WHEN props->>'district' LIKE 'DC%%' THEN props->>'district' ELSE props->>'province' END,
                    %s, {G}, props, %s FROM stage.munis""", (bs, db.snap_ids(munis)))
    # level 2: districts, dissolved from their municipalities
    db.x(f"""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom,props,snapshot_ids)
             SELECT props->>'district', 2, props->>'district', max(props->>'district_n'), max(props->>'province'), %s,
                    ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_Union(ST_SnapToGrid({G},0.00001))),3)), '{{}}', %s
             FROM stage.munis WHERE props->>'district' LIKE 'DC%%' GROUP BY props->>'district'""", (bs, db.snap_ids(munis)))
    # level 1: provinces, dissolved from municipalities (snap first, so slivers do not survive; see map-build-pipeline note)
    db.x(f"""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom,snapshot_ids)
             SELECT props->>'province', 1, props->>'province', NULL, 'ZA', %s,
                    ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_Union(ST_SnapToGrid({G},0.00001))),3)), %s
             FROM stage.munis GROUP BY props->>'province'""", (bs, db.snap_ids(munis)))
    for k, v in PROV.items(): db.x("UPDATE core.admin_area SET name=%s WHERE level=1 AND area_id=%s", (v, k))
    db.x("""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom)
            SELECT 'ZA',0,'ZA','South Africa',NULL,%s, ST_Multi(ST_Union(geom)) FROM core.admin_area WHERE level=1 AND boundary_set=%s
            ON CONFLICT (area_id) DO UPDATE SET geom=EXCLUDED.geom""", (bs, bs))
    # level 4: wards. area_id is 'W' + 8-digit MDB ward id so ids never collide across levels.
    n = db.x(f"""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom,props,snapshot_ids)
                 SELECT 'W'||(props->>'wardid'), 4, props->>'wardid', 'Ward '||(props->>'wardno'), props->>'cat_b', %s, {G}, props, %s
                 FROM stage.wards""", (bs, db.snap_ids(wards)))
    db.lineage("core.admin_area", n, [munis, wards], f"boundary_set={bs}")
    print(f"  admin tree: {n} wards plus municipalities, districts, provinces, country")

# ------------------------------------------------------------------ suburbs (level 6, parent = ward with the largest overlap)
def cmd_suburbs(db, a):
    p = a.files[0]; stage_features(db, "suburbs", [p])
    db.x("UPDATE core.parcel SET suburb_id=NULL")
    db.x("DELETE FROM agg.area_metric WHERE area_id IN (SELECT area_id FROM core.admin_area WHERE level=6)")
    db.x("DELETE FROM core.admin_area WHERE level=6 AND boundary_set=%s", (a.set,))
    n = db.x("""INSERT INTO core.admin_area (area_id,level,code,name,parent_id,boundary_set,geom,props,snapshot_ids)
                SELECT 'S'||(props->>'suburb_id'), 6, props->>'suburb_id', props->>'name', props->>'muni_code', %s,
                       ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_SetSRID(ST_GeomFromGeoJSON(gj),4326)),3)), props, %s
                FROM stage.suburbs WHERE gj IS NOT NULL""", (a.set, db.snap_ids(p)))
    # Parent: the ward holding most of the suburb's area. Suburbs cross ward lines, so the full split
    # goes into core.area_crosswalk (share = area share) and parent_id keeps the single best ward.
    db.x("DELETE FROM core.area_crosswalk WHERE method='suburb_ward_area'")
    db.x("""INSERT INTO core.area_crosswalk (from_id,to_id,share,method)
            SELECT s.area_id, w.area_id,
                   ST_Area(ST_Intersection(s.geom,w.geom)::geography)/NULLIF(ST_Area(s.geom::geography),0), 'suburb_ward_area'
            FROM core.admin_area s JOIN core.admin_area w ON w.level=4 AND ST_Intersects(s.geom,w.geom)
            WHERE s.level=6 AND s.boundary_set=%s ON CONFLICT DO NOTHING""", (a.set,))
    db.x("""UPDATE core.admin_area s SET parent_id=b.to_id FROM (
              SELECT DISTINCT ON (from_id) from_id,to_id FROM core.area_crosswalk WHERE method='suburb_ward_area'
              ORDER BY from_id, share DESC) b
            WHERE s.area_id=b.from_id AND b.to_id IN (SELECT area_id FROM core.admin_area)""")
    db.lineage("core.admin_area[level=6]", n, [p]); print(f"  {n} suburbs")

# ------------------------------------------------------------------ population (metrics on wards; build_tree rolls them up)
# Metric names the back-end and app read (backend/src/api.cpp headline metrics: pop_est, households, parcels, value_median).
METRIC_NAME = {"households_est": "households", "adults_18plus_est": "adults_18plus"}
def cmd_population(db, a):
    p = a.files[0]; rows = []
    cols = ["pop_est", "households_est", "adults_18plus_est", "male", "female", "age_0_14", "age_65plus", "registered_voters_2026"]
    db.x("DELETE FROM agg.area_metric WHERE source_id='statssa_ward_estimates_2022'")
    for r in csv.DictReader(opn(p)):
        for c in cols:
            if r.get(c) not in (None, ""):
                rows.append(("W" + r["ward_id"], METRIC_NAME.get(c, c), "2022-10-01", float(r[c]), 1, "statssa_ward_estimates_2022"))
    db.x("CREATE TEMP TABLE m (LIKE agg.area_metric) ON COMMIT DROP")
    db.copy_rows("m", ["area_id", "metric", "as_of", "v", "n", "source_id"], rows)
    n = db.x("INSERT INTO agg.area_metric SELECT m.* FROM m JOIN core.admin_area a USING (area_id) ON CONFLICT DO NOTHING")
    db.lineage("agg.area_metric[population]", n, [p]); print(f"  {n} ward population metrics")

# ------------------------------------------------------------------ parcels
PARCEL_KEEP = ["sg21", "type", "lstatus", "town", "town_code", "parcel", "portion", "farm_name", "scheme", "area_m2", "sg_date", "muni", "src"]
def cmd_parcels(db, a):
    n_all = 0
    for p in a.files:
        stage_features(db, "parcels", [p], keep=PARCEL_KEEP)
        n = db.x("""INSERT INTO core.parcel (sg21,kind,lstatus,town,town_code,parcel_no,portion_no,farm_name,scheme,area_m2,sg_date,
                                           muni_id,geom,src,snapshot_ids)
                    SELECT DISTINCT ON (props->>'sg21') props->>'sg21', props->>'type', props->>'lstatus', props->>'town', props->>'town_code',
                           props->>'parcel', props->>'portion', props->>'farm_name', props->>'scheme', (props->>'area_m2')::float,
                           (props->>'sg_date')::date,
                           (SELECT area_id FROM core.admin_area WHERE area_id=props->>'muni'),
                           ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_SetSRID(ST_GeomFromGeoJSON(gj),4326)),3)),
                           props->>'src', %s
                    FROM stage.parcels WHERE length(props->>'sg21')=21 AND gj IS NOT NULL
                    ON CONFLICT (sg21) DO UPDATE SET kind=EXCLUDED.kind, lstatus=EXCLUDED.lstatus, geom=EXCLUDED.geom,
                         area_m2=EXCLUDED.area_m2, sg_date=EXCLUDED.sg_date, src=EXCLUDED.src, snapshot_ids=EXCLUDED.snapshot_ids""",
                 (db.snap_ids(p),))
        db.lineage("core.parcel", n, [p]); db.commit(); n_all += n
        print(f"  {os.path.basename(p)}: {n} parcels")
    print(f"  {n_all} parcels loaded")

# ------------------------------------------------------------------ valuation (rows already matched to parcels by parcels/scripts/join_rolls.py)
VAL_COLS = ["sg21", "muni", "market_value", "category", "usage", "extent", "township", "erf", "portion", "scheme",
            "sale_price", "sale_date", "roll", "roll_period", "source_url", "value_flag", "match", "area_check"]
def num(x):
    try: return round(float(x)) if x not in (None, "") else None
    except ValueError: return None
def valid_date(s):
    import datetime
    try: datetime.date.fromisoformat(s[:10]); return True
    except ValueError: return False
def cmd_valuation(db, a):
    p = a.files[0]
    rdr = csv.DictReader(opn(p))
    assert "unit" not in VAL_COLS
    munis = set(); snap = db.snap_ids(p)
    def rows():
        for r in rdr:
            munis.add(r["muni"])
            sd = r.get("sale_date") or None
            if sd and not valid_date(sd): sd = None   # rolls carry typos like 2014-25-07
            yield (r["sg21"] or None, r["muni"], r.get("roll_period"), r.get("roll"), r.get("township"), r.get("erf"), r.get("portion"),
                   r.get("scheme"), r.get("category"), r.get("usage"), num(r.get("extent")), num(r.get("market_value")),
                   num(r.get("sale_price")), sd, r.get("match"), r.get("area_check"), r.get("value_flag"), r.get("source_url"), snap)
    db.x("CREATE TEMP TABLE v (LIKE core.valuation INCLUDING DEFAULTS) ON COMMIT DROP")
    n = db.copy_rows("v", ["sg21", "muni_code", "roll_period", "roll", "township", "erf", "portion", "scheme", "category", "usage",
                           "extent_m2", "market_value", "sale_price", "sale_date", "match_method", "area_check", "value_flag",
                           "source_url", "snapshot_ids"], rows())
    db.x("DELETE FROM core.valuation WHERE muni_code = ANY(%s)", (sorted(munis),))
    db.x("INSERT INTO core.valuation SELECT * FROM v")
    db.lineage("core.valuation", n, [p], "unit column never read (owner names in some rolls)"); print(f"  {n} valuation rows, {len(munis)} municipalities")

# ------------------------------------------------------------------ power grid graph (pgRouting layout)
def cmd_grid(db, a):
    nodes_p, edges_p = a.files[:2]; net = a.network
    stage_features(db, "gnodes", [nodes_p]); stage_features(db, "gedges", [edges_p])
    db.x("DELETE FROM net.edge WHERE network=%s", (net,)); db.x("DELETE FROM net.node WHERE network=%s", (net,))
    db.x("""INSERT INTO net.node (network,node_id,ext_id,kind,name,operator,kv,geom,props)
            SELECT %s, row_number() OVER (ORDER BY props->>'id'), props->>'id', props->>'kind', props->>'name', props->>'operator',
                   (SELECT array_agg(x::int) FROM jsonb_array_elements_text(CASE WHEN jsonb_typeof(props->'kv')='array' THEN props->'kv' END) x WHERE x ~ '^\\d+$'),
                   ST_SetSRID(ST_GeomFromGeoJSON(gj),4326), props - 'id' - 'name' - 'kind' - 'operator' - 'kv'
            FROM stage.gnodes""", (net,))
    # Edges may name nodes the node file does not hold (line ends, towers): add them as 'endpoint' nodes.
    db.x("""INSERT INTO net.node (network,node_id,ext_id,kind,geom)
            SELECT %s, (SELECT coalesce(max(node_id),0) FROM net.node WHERE network=%s) + row_number() OVER (ORDER BY ext), ext, 'endpoint', pt
            FROM (SELECT DISTINCT ON (ext) ext, pt FROM (
                    SELECT props->>'u' ext, ST_StartPoint(ST_GeometryN(ST_SetSRID(ST_GeomFromGeoJSON(gj),4326),1)) pt FROM stage.gedges
                    UNION ALL
                    SELECT props->>'v', ST_EndPoint(ST_GeometryN(ST_SetSRID(ST_GeomFromGeoJSON(gj),4326),-1)) FROM stage.gedges) x
                  WHERE ext IS NOT NULL AND NOT EXISTS (SELECT 1 FROM net.node n WHERE n.network=%s AND n.ext_id=x.ext)) y""", (net, net, net))
    n = db.x("""INSERT INTO net.edge (network,edge_id,ext_id,source,target,cost,reverse_cost,cls,kv,circuits,inferred,reason,geom,props)
                SELECT %s, row_number() OVER (ORDER BY e.props->>'id'), e.props->>'id', u.node_id, v.node_id,
                       coalesce((e.props->>'km')::float, ST_Length(g::geography)/1000), coalesce((e.props->>'km')::float, ST_Length(g::geography)/1000),
                       e.props->>'cls',
                       (SELECT array_agg(x::int) FROM jsonb_array_elements_text(CASE WHEN jsonb_typeof(e.props->'kv')='array' THEN e.props->'kv' END) x WHERE x ~ '^\\d+$'),
                       CASE WHEN e.props->>'circuits' ~ '^\\d+$' THEN (e.props->>'circuits')::int END,
                       coalesce((e.props->>'inferred')::boolean,false), e.props->>'reason', ST_LineMerge(g), e.props
                FROM (SELECT props, ST_SetSRID(ST_GeomFromGeoJSON(gj),4326) g FROM stage.gedges) e
                JOIN net.node u ON u.network=%s AND u.ext_id=e.props->>'u'
                JOIN net.node v ON v.network=%s AND v.ext_id=e.props->>'v'""", (net, net, net))
    db.lineage("net.edge", n, [nodes_p, edges_p], f"network={net}"); print(f"  {n} grid edges")

# ------------------------------------------------------------------ climate monthly (ts.series + ts.value)
def cmd_climate(db, a):
    p = a.files[0]; src = "nasa_power_monthly"
    want = a.vars.split(",") if a.vars else None
    db.x("INSERT INTO meta.source (source_id,title,kind) VALUES (%s,%s,'api') ON CONFLICT DO NOTHING", (src, "NASA POWER monthly"))
    rdr = csv.DictReader(opn(p)); key = rdr.fieldnames[0]
    vars_ = [c for c in rdr.fieldnames[2:] if not want or c in want]
    units = {"rain": "mm", "t_": "degC", "frost": "days", "rh": "%"}
    ent = "W" if key == "wardid" else ""
    sid = {}
    db.x("CREATE TEMP TABLE tv (e text, var text, t date, v float8) ON COMMIT DROP")
    def rows():
        for r in rdr:
            for c in vars_:
                if r[c] != "": yield (ent + r[key], c, r["month"] + "-01", float(r[c]))
    n = db.copy_rows("tv", ["e", "var", "t", "v"], rows())
    db.x("""INSERT INTO ts.series (entity_type,entity_id,variable,unit,step,source_id)
            SELECT DISTINCT 'area', e, var, NULL, 'month', %s FROM tv ON CONFLICT DO NOTHING""", (src,))
    for pre, u in units.items(): db.x("UPDATE ts.series SET unit=%s WHERE source_id=%s AND variable LIKE %s", (u, src, pre + "%"))
    db.x("""DELETE FROM ts.value WHERE series_id IN (SELECT series_id FROM ts.series WHERE source_id=%s AND variable = ANY(%s))""", (src, vars_))
    db.x("""INSERT INTO ts.value (series_id,t,v) SELECT s.series_id, tv.t, tv.v FROM tv
            JOIN ts.series s ON s.entity_type='area' AND s.entity_id=tv.e AND s.variable=tv.var AND s.source_id=%s""", (src,))
    db.lineage("ts.value[climate]", n, [p], ",".join(vars_)); print(f"  {n} climate values ({len(vars_)} variables)")

# ------------------------------------------------------------------ dams (place + weekly level series)
def num_f(x):
    try: return float(x)
    except (TypeError, ValueError): return None
def cmd_dams(db, a):
    p = a.files[0]; src = "dws_dam_levels"
    db.x("INSERT INTO meta.source (source_id,title,kind) VALUES (%s,%s,'manual') ON CONFLICT DO NOTHING", (src, "DWS weekly state of reservoirs"))
    rows = list(csv.DictReader(opn(p)))
    rows = [r for r in rows if num_f(r.get("lat")) is not None and num_f(r.get("lon")) is not None]  # a few dams have no coordinates
    for r in rows:
        props = clean_props({k: r[k] for k in ("river", "fsc_mcm", "drainage", "catchment_km2", "wma", "surface_ha", "osm_id")})
        db.x("""INSERT INTO core.place (place_id,kind,name,geom,props,snapshot_ids) VALUES (%s,'dam',%s,ST_SetSRID(ST_MakePoint(%s,%s),4326),%s,%s)
                ON CONFLICT (place_id) DO UPDATE SET name=EXCLUDED.name, geom=EXCLUDED.geom, props=EXCLUDED.props""",
             ("dws:" + r["station"], r["name"], num_f(r["lon"]), num_f(r["lat"]), json.dumps(props), db.snap_ids(p)))
        for var, col, unit in (("dam_pct", "pct", "%"), ("dam_vol_mcm", "vol_mcm", "million m3")):
            if r.get(col):
                sid = db.q("""INSERT INTO ts.series (entity_type,entity_id,variable,unit,step,source_id) VALUES ('place',%s,%s,%s,'week',%s)
                              ON CONFLICT (entity_type,entity_id,variable,source_id) DO UPDATE SET unit=EXCLUDED.unit RETURNING series_id""",
                           ("dws:" + r["station"], var, unit, src))[0][0]
                db.x("INSERT INTO ts.value VALUES (%s,%s,%s,%s) ON CONFLICT (series_id,t) DO UPDATE SET v=EXCLUDED.v",
                     (sid, r["report_date"], num_f(r[col]), "stale" if r.get("stale") == "True" else None))
    db.lineage("core.place[dam]+ts.value[dam]", len(rows), [p]); print(f"  {len(rows)} dams")

CMDS = {"sources": cmd_sources, "admin": cmd_admin, "suburbs": cmd_suburbs, "population": cmd_population, "parcels": cmd_parcels,
        "valuation": cmd_valuation, "grid": cmd_grid, "climate": cmd_climate, "dams": cmd_dams}

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=CMDS); ap.add_argument("files", nargs="+")
    ap.add_argument("--snap", action="append", help="raw archive snapshot id(s) the files came from")
    ap.add_argument("--set", default=None, help="boundary set name (admin: MDB2021, suburbs: HRSL2026)")
    ap.add_argument("--snapshots"); ap.add_argument("--network", default="power"); ap.add_argument("--vars")
    ap.add_argument("--dsn", default=os.environ.get("GSA_DSN", "dbname=gsa"))
    a = ap.parse_args()
    if a.set is None: a.set = "HRSL2026" if a.cmd == "suburbs" else "MDB2021"
    db = DB(a.dsn, a.snap, f"gsa_load.py {a.cmd}")
    print(f"== {a.cmd}")
    CMDS[a.cmd](db, a); db.commit()

if __name__ == "__main__":
    main()
