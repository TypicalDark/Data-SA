"""Grid of SA: export the attribute side of the data store to one SQLite file for the desktop back-end.

    python export_pack.py OUT.sqlite [--dsn "dbname=gsa"] [--muni WC053,EC101]

Geometry goes to PMTiles (export_tiles.py). This file holds what the /v1 API answers from:
    tree(area_id, level, parent_id, path, name, minx, miny, maxx, maxy, lon, lat)   drill-down + fly-to
    metric(area_id, metric, as_of, v, n, source_id)                                 numbers for any node
    parcel(sg21, ward_id, suburb_id, kind, town, parcel_no, portion_no, area_m2, h3_r9, quadkey_z16, lon, lat)
    valuation(sg21, muni_code, roll_period, category, usage, extent_m2, market_value, sale_price, sale_date, address, source_url)
    node / edge                                                                     grid trace (power network)
    series / series_data                                                            time series, one packed row per series
    source / snapshot                                                               credits and provenance
    search (FTS5)                                                                   one search box over everything
Standard library sqlite3 plus psycopg. Owner names and ID numbers never exist in the data store, so they cannot leak here.
"""
import argparse, decimal, os, sqlite3, sys
sqlite3.register_adapter(decimal.Decimal, lambda d: int(d) if d == d.to_integral_value() else float(d))
import psycopg

TABLES = {
 "tree": ("""SELECT area_id, level, parent_id, path::text, name, ST_XMin(geom), ST_YMin(geom), ST_XMax(geom), ST_YMax(geom),
                    ST_X(label_pt), ST_Y(label_pt), area_km2 FROM core.admin_area""",
          "area_id TEXT PRIMARY KEY, level INT, parent_id TEXT, path TEXT, name TEXT, minx REAL, miny REAL, maxx REAL, maxy REAL, lon REAL, lat REAL, area_km2 REAL"),
 "metric": ("SELECT area_id, metric, as_of::text, v, n, source_id FROM agg.area_metric",
            "area_id TEXT, metric TEXT, as_of TEXT, v REAL, n INT, source_id TEXT, PRIMARY KEY (area_id, metric, as_of)"),
 "parcel": ("""SELECT sg21, ward_id, suburb_id, kind, town, parcel_no, portion_no, area_m2, h3_r9::text, quadkey_z16,
                      ST_X(ST_PointOnSurface(geom)), ST_Y(ST_PointOnSurface(geom)) FROM core.parcel {muni}""",
            "sg21 TEXT PRIMARY KEY, ward_id TEXT, suburb_id TEXT, kind TEXT, town TEXT, parcel_no TEXT, portion_no TEXT, area_m2 REAL, h3_r9 TEXT, quadkey_z16 TEXT, lon REAL, lat REAL"),
 "valuation": ("""SELECT sg21, muni_code, roll_period, category, usage, extent_m2, market_value, sale_price, sale_date::text, address, source_url
                  FROM core.valuation {vmuni}""",
               "sg21 TEXT, muni_code TEXT, roll_period TEXT, category TEXT, usage TEXT, extent_m2 REAL, market_value INT, sale_price INT, sale_date TEXT, address TEXT, source_url TEXT"),
 "node": ("SELECT network, node_id, ext_id, kind, name, operator, array_to_string(kv,'/'), capacity_mva, ST_X(geom), ST_Y(geom) FROM net.node",
          "network TEXT, node_id INT, ext_id TEXT, kind TEXT, name TEXT, operator TEXT, kv TEXT, capacity_mva REAL, lon REAL, lat REAL, PRIMARY KEY (network, node_id)"),
 "edge": ("SELECT network, edge_id, source, target, cost, reverse_cost, cls, array_to_string(kv,'/'), inferred::int, reason FROM net.edge",
          "network TEXT, edge_id INT, source INT, target INT, cost REAL, reverse_cost REAL, cls TEXT, kv TEXT, inferred INT, reason TEXT, PRIMARY KEY (network, edge_id)"),
 "series": ("SELECT series_id, entity_type, entity_id, variable, unit, step, source_id FROM ts.series",
            "series_id INT PRIMARY KEY, entity_type TEXT, entity_id TEXT, variable TEXT, unit TEXT, step TEXT, source_id TEXT"),
 "source": ("SELECT source_id, title, publisher, url, licence, resale_ok::int, cadence, privacy FROM meta.source",
            "source_id TEXT PRIMARY KEY, title TEXT, publisher TEXT, url TEXT, licence TEXT, resale_ok INT, cadence TEXT, privacy TEXT"),
 "snapshot": ("SELECT snapshot_id, source_id, fetched_utc::text, tree_sha256, same_as FROM meta.snapshot",
              "snapshot_id TEXT PRIMARY KEY, source_id TEXT, fetched_utc TEXT, tree_sha256 TEXT, same_as TEXT"),
}
INDEXES = ["CREATE INDEX tree_parent ON tree(parent_id)", "CREATE INDEX parcel_ward ON parcel(ward_id)",
           "CREATE INDEX parcel_qk ON parcel(quadkey_z16)", "CREATE INDEX val_sg21 ON valuation(sg21)",
           "CREATE INDEX edge_s ON edge(network, source)", "CREATE INDEX edge_t ON edge(network, target)",
           "CREATE INDEX series_e ON series(entity_id, variable)"]

STEP_MONTHS = {"month": 1, "year": 12}
def _offset(t0, t, step):
    """Index of date t in a regular series that starts at t0."""
    if step in STEP_MONTHS:
        return ((t.year - t0.year) * 12 + t.month - t0.month) // STEP_MONTHS[step]
    days = (t - t0).days
    return days // 7 if step == "week" else days

def write_series_data(pg, lite):
    """series_data(series_id, t0, step, n, v): the whole series as little-endian float32, NaN where missing.
    21 M climate values as rows take about 1 GB; packed they take about 90 MB and one read returns a whole chart."""
    import array, math
    lite.execute("CREATE TABLE series_data (series_id INT PRIMARY KEY, t0 TEXT, step TEXT, n INT, v BLOB)")
    rows, cur_id, pts, step = [], None, [], None
    def flush():
        if cur_id is None or not pts: return
        t0 = pts[0][0]
        if step in ("month", "year", "week", "day"):
            n = _offset(t0, pts[-1][0], step) + 1
            a = array.array("f", [math.nan]) * n
            for t, v in pts: a[_offset(t0, t, step)] = math.nan if v is None else v
        else:  # irregular: keep values in order; times are not regular, so store them in t0 as a list
            n = len(pts); a = array.array("f", [math.nan if v is None else v for _, v in pts])
            t0 = ",".join(str(t) for t, _ in pts)
        if sys.byteorder != "little": a.byteswap()
        rows.append((cur_id, str(t0) if step != "event" else t0, step, n, a.tobytes()))
    with pg.cursor(name="x_series_data") as c:
        c.itersize = 50000
        c.execute("SELECT v.series_id, s.step, v.t, v.v FROM ts.value v JOIN ts.series s USING (series_id) ORDER BY v.series_id, v.t")
        for sid, st, t, v in c:
            if sid != cur_id:
                flush(); cur_id, pts, step = sid, [], st
                if len(rows) >= 2000:
                    lite.executemany("INSERT INTO series_data VALUES (?,?,?,?,?)", rows); rows.clear()
            pts.append((t, v))
    flush()
    lite.executemany("INSERT INTO series_data VALUES (?,?,?,?,?)", rows)
    print(f"  series_data: {lite.execute('SELECT count(*) FROM series_data').fetchone()[0]} series")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out"); ap.add_argument("--dsn", default=os.environ.get("GSA_DSN", "dbname=gsa")); ap.add_argument("--muni")
    a = ap.parse_args()
    if os.path.exists(a.out): os.remove(a.out)
    pg = psycopg.connect(a.dsn); lite = sqlite3.connect(a.out)
    lite.execute("PRAGMA journal_mode=OFF"); lite.execute("PRAGMA synchronous=OFF")
    munis = [m for m in (a.muni or "").split(",") if m]
    fmt = dict(muni=f"WHERE muni_id = ANY(%(m)s)" if munis else "", vmuni=f"WHERE muni_code = ANY(%(m)s)" if munis else "")
    for name, (sql, ddl) in TABLES.items():
        lite.execute(f"CREATE TABLE {name} ({ddl})")
        q = sql.format(**fmt)
        n = 0
        with pg.cursor(name=f"x_{name}") as c:
            c.itersize = 20000
            c.execute(q, {"m": munis} if munis else None)
            while True:
                rows = c.fetchmany(20000)
                if not rows: break
                ph = ",".join("?" * len(rows[0]))
                lite.executemany(f"INSERT OR REPLACE INTO {name} VALUES ({ph})", rows); n += len(rows)
        print(f"  {name}: {n} rows")
    for ix in INDEXES: lite.execute(ix)
    write_series_data(pg, lite)
    lite.execute("CREATE VIRTUAL TABLE search USING fts5(ref UNINDEXED, label, detail, weight UNINDEXED, lon UNINDEXED, lat UNINDEXED, tokenize='unicode61 remove_diacritics 2')")
    with pg.cursor(name="x_search") as c:
        c.execute("SELECT ref, label, detail, rank, ST_X(pt), ST_Y(pt) FROM core.search")
        while True:
            rows = c.fetchmany(20000)
            if not rows: break
            lite.executemany("INSERT INTO search VALUES (?,?,?,?,?,?)", rows)
    lite.execute("CREATE TABLE pack_info (k TEXT PRIMARY KEY, v TEXT)")
    lite.executemany("INSERT INTO pack_info VALUES (?,?)", [("format", "gsa-pack/1"), ("built_utc", pg.execute("select now()::text").fetchone()[0]),
                                                           ("munis", ",".join(munis) or "all")])
    lite.commit(); lite.execute("VACUUM"); lite.close()
    print(f"wrote {a.out} {os.path.getsize(a.out)/1e6:.1f} MB")

if __name__ == "__main__":
    main()
