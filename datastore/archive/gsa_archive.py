"""Grid of SA raw archive: keep every original download, unchanged, with provenance.

Standard library only (Python 3.10+), works on Windows, macOS and Linux.

Layout (see DATA_ARCHITECTURE.md, section 2):
    ROOT/
      _catalog/sources.json        registry of every source (id, publisher, URL, licence, resale, cadence, privacy)
      _catalog/snapshots.jsonl     one line per snapshot (append-only)
      _catalog/files.jsonl         one line per archived file with sha256 (append-only)
      _staging/                    fetch_all.py downloads land here first
      sources/<source_id>/<YYYYMMDDTHHMMSSZ>/
           _source.json            provenance written by the fetcher (URL, time, licence, fields left out)
           _snapshot.json          id, tree hash, file count, bytes, same_as
           <original files, byte for byte>

Snapshots are immutable: files are made read-only after ingest and a later fetch never overwrites an
earlier one. If a new fetch is byte-identical to the latest snapshot of that source it is not stored
again; the catalog records it as `same_as` so you still know the source was checked on that date.

Commands:
    python gsa_archive.py init    ROOT
    python gsa_archive.py fetch   ROOT [--only a,b] [--big]      run fetch_all.py, then ingest each job
    python gsa_archive.py ingest  ROOT SOURCE_ID PATH... [--url U] [--licence L] [--note N] [--fetched ISO]
    python gsa_archive.py adopt   ROOT OLD_KIT_OUTPUT_DIR         import an earlier fetch_all.py output folder
    python gsa_archive.py verify  ROOT [--source ID]               re-hash every file, report changes
    python gsa_archive.py list    ROOT                            latest snapshot per source
    python gsa_archive.py sources ROOT                            (re)write _catalog/sources.json from the registry
"""
import argparse, datetime, hashlib, json, os, shutil, stat, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_VERSION = "gsa_archive/1.0"

# ------------------------------------------------------------------ source registry
# Sources fetched by fetch_all.py are read from its JOBS table. The ones below are fetched some other way
# (project threads, the user's computer, by hand) and are listed so that every dataset on the map has a
# registry entry. resale: True = open licence that allows commercial re-use, False = needs permission,
# None = unclear. See saas-business-plan for why rolls and sale prices are False.
EXTRA_SOURCES = {
    "osm_hot_snapshots": dict(title="OpenStreetMap snapshots via HOT Raw Data API", publisher="OpenStreetMap contributors",
                              url="https://api-prod.raw-data.hotosm.org/v1/snapshot/", kind="api", licence="ODbL 1.0", resale=True, cadence="daily"),
    "nasa_power_monthly": dict(title="NASA POWER MERRA-2 and IMERG monthly Zarr", publisher="NASA LaRC",
                               url="https://nasa-power.s3.amazonaws.com/", kind="api", licence="NASA open data", resale=True, cadence="monthly"),
    "ghcn_daily": dict(title="GHCN-Daily stations (South Africa)", publisher="NOAA NCEI", url="https://www.ncei.noaa.gov/pub/data/ghcn/daily/",
                       kind="file", licence="WMO Res 40: free, not for resale of raw data", resale=False, cadence="daily"),
    "dws_dam_levels": dict(title="DWS weekly state of reservoirs", publisher="Department of Water and Sanitation",
                           url="https://www.dws.gov.za/Hydrology/Weekly/ProvinceWeek.aspx", kind="manual",
                           licence="no open licence stated", resale=None, cadence="weekly",
                           notes="Blocked from the cloud; fetched on the user's computer with dams/fetch_dws.py"),
    "ntcsa_gcca_2025": dict(title="NTCSA Generation Connection Capacity Assessment 2025", publisher="NTCSA (Eskom)",
                            url="https://www.ntcsa.co.za/", kind="file", licence="published report", resale=None, cadence="yearly"),
    "global_solar_atlas": dict(title="Global Solar Atlas GeoTIFFs (South Africa)", publisher="World Bank / Solargis",
                               url="https://globalsolaratlas.info/download/south-africa", kind="file", licence="CC BY 4.0", resale=True, cadence="rare"),
    "google_open_buildings": dict(title="Google Open Buildings v3", publisher="Google Research", url="https://sites.research.google/open-buildings/",
                                  kind="file", licence="CC BY 4.0 / ODbL", resale=True, cadence="rare"),
    "open_charge_map": dict(title="Open Charge Map EV chargers", publisher="Open Charge Map", url="https://api.openchargemap.io/v3/poi",
                            kind="api", licence="CC BY 4.0", resale=True, cadence="daily"),
    "dffe_reea": dict(title="DFFE Renewable Energy EIA Applications", publisher="DFFE", url="https://egis.environment.gov.za/",
                      kind="file", licence="government open data", resale=None, cadence="quarterly"),
    "statssa_ward_estimates_2022": dict(title="Stats SA Census 2022 ward products", publisher="Statistics South Africa",
                                        url="https://census.statssa.gov.za/", kind="file", licence="Stats SA copyright, free re-use with credit", resale=True, cadence="10 years"),
    "mdb_2026_ward_gazettes": dict(title="MDB 2026 ward delimitation gazettes and voting district lists", publisher="Municipal Demarcation Board",
                                   url="https://www.demarcation.org.za/", kind="manual", licence="government publication", resale=True, cadence="5 years"),
    "kzn_cogta_valuation": dict(title="KZN COGTA municipal valuation layers", publisher="KZN COGTA",
                                url="https://giswebmaps1.kzncogta.gov.za/server/rest/services", kind="arcgis",
                                licence="government GIS, roll data under MPRA", resale=False, cadence="per roll cycle",
                                privacy="owner names and ID numbers never requested"),
    "cct_gv2025_roll": dict(title="City of Cape Town GV2025 roll (per-erf search)", publisher="City of Cape Town",
                            url="https://web1.capetown.gov.za/web1/gv2025/SearchProperty", kind="manual",
                            licence="MPRA public roll", resale=False, cadence="4 years", privacy="page shows no owners"),
    "opensky_routes": dict(title="OpenSky aircraft database and routes", publisher="OpenSky Network", url="https://opensky-network.org/",
                           kind="file", licence="CC BY 4.0 (non-commercial for some sets)", resale=None, cadence="monthly"),
    "copernicus_dem_glo30": dict(title="Copernicus DEM GLO-30", publisher="ESA / EU Copernicus", url="https://copernicus-dem-30m.s3.amazonaws.com/",
                                 kind="file", licence="Copernicus DEM licence (free with attribution)", resale=True, cadence="rare"),
}
RESALE_HINTS = (("ODbL", True), ("CC BY", True), ("public domain", True), ("GPL", True), ("MPRA", False), ("personal use", False))

def registry():
    out = {}
    try:
        sys.path.insert(0, HERE)
        import fetch_all  # the kit in the same folder
        for k, j in fetch_all.JOBS.items():
            lic = j.get("licence") or ""
            resale = next((v for h, v in RESALE_HINTS if h.lower() in lic.lower()), None)
            out[k] = dict(title=k.replace("_", " "), publisher=lic.split("(")[0].strip() or None, url=j.get("url"),
                          kind=j["kind"], licence=lic, resale=resale, cadence=None,
                          privacy="owner/ID/contact fields never requested (fetch_all.PRIVATE)" if j["kind"] in ("arcgis", "multi") else None,
                          fetcher="fetch_all.py")
    except Exception as e:
        print("note: fetch_all.py not importable, registry has extras only:", e)
    for k, v in EXTRA_SOURCES.items():
        out.setdefault(k, {}).update(v)
    return out

# ------------------------------------------------------------------ helpers
def now_stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")

def sha256_file(p, buf=1 << 20):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(buf)
            if not b: break
            h.update(b)
    return h.hexdigest()

def walk_files(d):
    for root, dirs, files in os.walk(d):
        dirs.sort()
        for fn in sorted(files):
            if fn in ("_snapshot.json",) or fn.endswith(".part"): continue
            p = os.path.join(root, fn)
            yield os.path.relpath(p, d).replace(os.sep, "/"), p

def tree_hash(entries):
    """entries: list of (rel_path, sha256). _source.json is excluded because it holds the fetch time."""
    h = hashlib.sha256()
    for rel, s in sorted(entries):
        if rel == "_source.json": continue
        h.update(f"{rel}\0{s}\n".encode())
    return h.hexdigest()

def catalog(root, name):
    return os.path.join(root, "_catalog", name)

def append_jsonl(path, rows):
    with open(path, "a", encoding="utf-8") as f:
        for r in rows: f.write(json.dumps(r, ensure_ascii=False) + "\n")

def read_jsonl(path):
    if not os.path.exists(path): return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]

def latest_snapshots(root):
    last = {}
    for s in read_jsonl(catalog(root, "snapshots.jsonl")):
        last[s["source_id"]] = s
    return last

def make_readonly(d):
    for _, p in walk_files(d):
        try: os.chmod(p, stat.S_IREAD | stat.S_IRGRP | stat.S_IROTH)
        except OSError: pass

# ------------------------------------------------------------------ commands
def cmd_init(a):
    for sub in ("_catalog", "_staging", "sources"):
        os.makedirs(os.path.join(a.root, sub), exist_ok=True)
    cmd_sources(a)
    print("archive ready at", a.root)

def cmd_sources(a):
    os.makedirs(os.path.join(a.root, "_catalog"), exist_ok=True)
    reg = registry()
    with open(catalog(a.root, "sources.json"), "w", encoding="utf-8") as f:
        json.dump(reg, f, indent=1, ensure_ascii=False)
    print(f"{len(reg)} sources in _catalog/sources.json")

def ingest_dir(root, source_id, src_dir, fetched=None, move=True, extra_meta=None):
    """Turn a finished download folder into an immutable snapshot. Returns the catalog row."""
    stamp = fetched or now_stamp()
    snap_id = f"{source_id}@{stamp}"
    rel_snap = f"sources/{source_id}/{stamp}"
    dest = os.path.join(root, *rel_snap.split("/"))
    if os.path.exists(dest):
        raise SystemExit(f"{snap_id} already exists; snapshots are never overwritten")
    files = [(rel, sha256_file(p), os.path.getsize(p)) for rel, p in walk_files(src_dir)]
    if not files:
        print(f"  {source_id}: nothing to ingest in {src_dir}"); return None
    th = tree_hash([(r, s) for r, s, _ in files])
    prev = latest_snapshots(root).get(source_id)
    row = dict(snapshot_id=snap_id, source_id=source_id, fetched_utc=stamp, archive_path=rel_snap, tree_sha256=th,
               n_files=len(files), bytes=sum(b for *_, b in files), tool=TOOL_VERSION, same_as=None, status="ok")
    if extra_meta: row.update(extra_meta)
    if prev and prev["tree_sha256"] == th:
        row["same_as"] = prev.get("same_as") or prev["snapshot_id"]
        row["archive_path"] = prev["archive_path"]
        append_jsonl(catalog(root, "snapshots.jsonl"), [row])
        if move: shutil.rmtree(src_dir, ignore_errors=True)
        print(f"  {source_id}: unchanged since {row['same_as']} (checked, not stored again)")
        return row
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if move: shutil.move(src_dir, dest)
    else: shutil.copytree(src_dir, dest)
    with open(os.path.join(dest, "_snapshot.json"), "w", encoding="utf-8") as f:
        json.dump(row, f, indent=1)
    append_jsonl(catalog(root, "files.jsonl"),
                 [dict(snapshot_id=snap_id, rel_path=r, sha256=s, bytes=b) for r, s, b in files])
    append_jsonl(catalog(root, "snapshots.jsonl"), [row])
    make_readonly(dest)
    print(f"  {source_id}: stored {len(files)} files, {row['bytes']/1e6:.1f} MB as {snap_id}")
    return row

def cmd_fetch(a):
    stage = os.path.join(a.root, "_staging", now_stamp())
    os.makedirs(stage, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, "fetch_all.py"), stage]
    if a.big: cmd.append("--big")
    if a.only: cmd += ["--only", a.only]
    print("running", " ".join(cmd))
    rc = subprocess.call(cmd)
    results = {}
    log = os.path.join(stage, "_log.csv")
    if os.path.exists(log):
        import csv
        for r in csv.DictReader(open(log, encoding="utf-8")): results[r["job"]] = r["result"]
    for job in sorted(os.listdir(stage)):
        d = os.path.join(stage, job)
        if not os.path.isdir(d): continue
        res = results.get(job, "")
        status = "ok" if res.startswith("ok") else ("manual" if res.startswith("MANUAL") else "partial")
        if status == "partial":
            print(f"  {job}: left in staging because it is incomplete ({res}); re-run fetch to finish it"); continue
        ingest_dir(a.root, job, d, extra_meta={"fetch_result": res, "status": status})
    print("fetch_all exit code", rc)

def cmd_ingest(a):
    tmp = os.path.join(a.root, "_staging", f"ingest_{a.source_id}_{now_stamp()}")
    os.makedirs(tmp)
    for p in a.paths:
        if os.path.isdir(p): shutil.copytree(p, os.path.join(tmp, os.path.basename(p.rstrip("/\\"))))
        else: shutil.copy2(p, tmp)
    meta = dict(source_id=a.source_id, url=a.url, licence=a.licence, note=a.note, ingested_by=TOOL_VERSION,
                fetched_utc=a.fetched or None, original_paths=[os.path.abspath(p) for p in a.paths])
    if not os.path.exists(os.path.join(tmp, "_source.json")):
        with open(os.path.join(tmp, "_source.json"), "w", encoding="utf-8") as f: json.dump(meta, f, indent=1)
    stamp = None
    if a.fetched:
        stamp = datetime.datetime.fromisoformat(a.fetched.replace("Z", "")).strftime("%Y%m%dT%H%M%SZ")
    ingest_dir(a.root, a.source_id, tmp, fetched=stamp)

def cmd_adopt(a):
    """Import the folders of an earlier `fetch_all.py OUT` run. The fetch time comes from each _source.json."""
    for job in sorted(os.listdir(a.old)):
        d = os.path.join(a.old, job)
        if not os.path.isdir(d) or job.startswith("_"): continue
        stamp = None
        try:
            m = json.load(open(os.path.join(d, "_source.json"), encoding="utf-8"))
            stamp = datetime.datetime.fromisoformat(m["fetched_utc"].replace("Z", "")).strftime("%Y%m%dT%H%M%SZ")
        except Exception: pass
        ingest_dir(a.root, job, d, fetched=stamp, move=not a.copy, extra_meta={"adopted_from": os.path.abspath(d)})

def cmd_verify(a):
    files = read_jsonl(catalog(a.root, "files.jsonl"))
    snaps = {s["snapshot_id"]: s for s in read_jsonl(catalog(a.root, "snapshots.jsonl")) if not s.get("same_as")}
    bad = missing = ok = 0
    for f in files:
        if a.source and not f["snapshot_id"].startswith(a.source + "@"): continue
        s = snaps.get(f["snapshot_id"])
        if not s: continue
        p = os.path.join(a.root, *s["archive_path"].split("/"), *f["rel_path"].split("/"))
        if not os.path.exists(p): missing += 1; print("MISSING", p); continue
        if sha256_file(p) != f["sha256"]: bad += 1; print("CHANGED", p)
        else: ok += 1
    print(f"verified {ok} files, {bad} changed, {missing} missing")
    sys.exit(1 if bad or missing else 0)

def cmd_list(a):
    reg = {}
    try: reg = json.load(open(catalog(a.root, "sources.json"), encoding="utf-8"))
    except Exception: pass
    last = latest_snapshots(a.root)
    for k in sorted(set(reg) | set(last)):
        s = last.get(k)
        if s: print(f"{k:36s} {s['fetched_utc']}  {s['n_files']:6d} files {s['bytes']/1e6:10.1f} MB {'(unchanged)' if s.get('same_as') else ''}")
        else: print(f"{k:36s} not archived yet")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    for n in ("init", "sources", "list"):
        p = sp.add_parser(n); p.add_argument("root")
    p = sp.add_parser("fetch"); p.add_argument("root"); p.add_argument("--only", default=""); p.add_argument("--big", action="store_true")
    p = sp.add_parser("ingest"); p.add_argument("root"); p.add_argument("source_id"); p.add_argument("paths", nargs="+")
    p.add_argument("--url"); p.add_argument("--licence"); p.add_argument("--note"); p.add_argument("--fetched")
    p = sp.add_parser("adopt"); p.add_argument("root"); p.add_argument("old"); p.add_argument("--copy", action="store_true")
    p = sp.add_parser("verify"); p.add_argument("root"); p.add_argument("--source")
    a = ap.parse_args()
    {"init": cmd_init, "sources": cmd_sources, "fetch": cmd_fetch, "ingest": cmd_ingest, "adopt": cmd_adopt,
     "verify": cmd_verify, "list": cmd_list}[a.cmd](a)

if __name__ == "__main__":
    main()
