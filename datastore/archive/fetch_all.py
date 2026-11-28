"""Grid of SA: download every original source dataset, unchanged, onto this computer.

Usage (Windows, Python 3.10+, no extra packages):
    python fetch_all.py D:\\GridOfSA\\raw                 # everything except the very big optional sets
    python fetch_all.py D:\\GridOfSA\\raw --big           # also OpenSky flight lists (7 GB) and DFFE national erven
    python fetch_all.py D:\\GridOfSA\\raw --only cadastre_dffe,osm
    python fetch_all.py D:\\GridOfSA\\raw --list          # show the jobs and exit

What "original" means here:
  * Files (PDF, XLSX, CSV, ZIP, GeoTIFF, PBF) are saved byte for byte as the publisher serves them.
  * Map services (ArcGIS REST) have no file to download, so every page the server returns is saved
    as-is (GeoJSON, WGS84) into <job>/pages/NNNNNN.json.gz. Nothing is merged, cleaned or renamed.
  * Privacy rule kept from the project: fields holding owner names, ID numbers or personal contact
    details are never requested (see PRIVATE below). Sale prices, values, addresses, title deeds are kept.

Re-running skips finished files and pages, so an interrupted run just continues.
Each job writes <job>/_source.json (URL, date, licence) and the run writes _log.csv.
"""
import sys, os, json, gzip, csv, time, re, hashlib, ssl, argparse, datetime
import urllib.request, urllib.parse, urllib.error
import concurrent.futures as cf

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) GridOfSA-archive/1.0"
PRIVATE = re.compile(r"OWNER|OWNR|SURNAME|FIRST_?NAME|INITIAL|ID_?NO|ID_?NUM|IDNUMBER|IDENTITY|PERSON|"
                     r"TEL|PHONE|CELL|EMAIL|POSTAL|ACCOUNT|ADDRESSEE|CREATED_USER|LAST_EDITED_USER|EDITOR", re.I)
HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- jobs
DFFE = "https://dffeportal.environment.gov.za/hosting/rest/services/CSG_Cadaster/CSG_Cadastral_Data/MapServer"
WCSG = "https://gis.westerncape.gov.za/server2/rest/services/SpatialDataWarehouse/SG_PlanningCadastre/MapServer"
JHB = "https://ags.joburg.org.za/server/rest/services/Property/MapServer"
JHB_F = ("OBJECTID,SG_ID,STAND_NO,TOWN_NAME_DESC,STATUS_DESC,LAND_TYPE_NAME,AREA_SQMT,PROPERTY_ID,MARKET_VALUE,"
         "VALUATION_EFFECTIVE_DATE,CAT_DESC,ZONING,STREET_NO,STREET_NAME,STREET_TYPE_NAME,WARD_NAME,REGION_NAME,"
         "PURCHASE_PRICE,PURCHASE_DATE,NEW_TITLE_DEED_NUMBER,REGISTRATION_DATE,DIAGRAM_HOTLINK")
KZNT = "https://giswebmaps1.kzncogta.gov.za/server/rest/services/Land_tenure_component_2026/MapServer/0"
KZNT_F = ("OBJECTID_1,GID,PRCLK,SGKEY,PTNNO,LSTATUS,WSTATUS,SGNO,REG_DIVISI,TOWNLINK,ERF,PORTION,LOCAL_MUNI,"
          "DISTRICT_M,WARD,FARM_NAME,REG_DATE,REG_STATUS,REG_EXTENT,SCHEME,NEWPROP,CATEGORY_1,CATEGORY_2,VALUE")
NDA = "https://ndagis.nda.agric.za/arcgis/rest/services/AGIS"
CCT = "https://citymaps.capetown.gov.za/agsext/rest/services"
CSIR = "https://pta-gis-2-web1.csir.co.za/server2/rest/services"
NC = "https://services2.arcgis.com/V9xWeKFsczvmgrcv/arcgis/rest/services"

def A(layer, fields="*", where="1=1", big=False, lic=""):
    return {"kind": "arcgis", "url": layer, "fields": fields, "where": where, "big": big, "licence": lic}
def F(url, name=None, big=False, lic=""):
    return {"kind": "file", "url": url, "name": name, "big": big, "licence": lic}

JOBS = {
 # --- terrain
 "terrain_copernicus": {"kind": "copernicus", "licence": "Copernicus DEM licence (free, attribution)"},
 # --- OpenStreetMap (roads, rail, buildings, power lines, places). The map was built from south-africa-261124.osm.pbf.
 "osm": F("https://download.geofabrik.de/africa/south-africa-latest.osm.pbf", lic="ODbL (c) OpenStreetMap contributors"),
 # --- cadastre
 "cadastre_dffe_farms": A(f"{DFFE}/0", lic="Chief Surveyor-General via DFFE"),
 "cadastre_dffe_farm_portions": A(f"{DFFE}/1", lic="Chief Surveyor-General via DFFE"),
 "cadastre_dffe_erven": A(f"{DFFE}/2", big=True, lic="Chief Surveyor-General via DFFE (8.4M features, about 3 h)"),
 "cadastre_dffe_holdings": A(f"{DFFE}/3", lic="Chief Surveyor-General via DFFE"),
 "cadastre_wc_farms": A(f"{WCSG}/3", lic="Western Cape Government / CSG"),
 "cadastre_wc_farm_portions": A(f"{WCSG}/2", lic="Western Cape Government / CSG"),
 "cadastre_wc_erven": A(f"{WCSG}/1", lic="Western Cape Government / CSG"),
 "cadastre_cpt_parcels": A(f"{CCT}/Land_Administration/MapServer/14", lic="City of Cape Town"),
 "cadastre_eku_stands": A("https://gis.ekurhuleni.gov.za/arcgis/rest/services/Ekurhuleni/Ekurhuleni_Propety_Data_Map/MapServer/1", lic="City of Ekurhuleni"),
 **{f"cadastre_jhb_layer{n}": A(f"{JHB}/{n}", JHB_F, lic="City of Johannesburg CGIS (owner names never requested)") for n in (7, 8, 22, 23, 32, 33)},
 "cadastre_kzn_cogta_tenure": A(KZNT, KZNT_F, lic="KZN COGTA (owner, title deed fields never requested)"),
 "cadastre_nc_solplaatje": {"kind": "multi", "jobs": [A(f"{NC}/Cadastre_2020_2021/FeatureServer/{n}") for n in (6, 7, 8, 9)], "licence": "Sol Plaatje LM"},
 # Polokwane and KZN valuation layers use whitelisted fields: run the project scripts (see README).
 # --- valuation rolls (original PDF/XLS files, one per source_url)
 "valuation_rolls": {"kind": "rolls", "licence": "Each municipality (MPRA s85); personal use"},
 # --- elections and boundaries
 "wards_mdb2021": A("https://gis.westerncape.gov.za/hosting/rest/services/Hosted/Demarcation/FeatureServer/2", lic="MDB via Western Cape Government"),
 "munis_mdb2021": A("https://gis.westerncape.gov.za/hosting/rest/services/Hosted/Demarcation/FeatureServer/1", lic="MDB via Western Cape Government"),
 "districts_mdb2021": A("https://gis.westerncape.gov.za/hosting/rest/services/Hosted/Demarcation/FeatureServer/0", lic="MDB via Western Cape Government"),
 "iec_lge2026_wards": {"kind": "iec", "licence": "Electoral Commission of South Africa"},
 # --- power and supply areas
 "eskom_calendar": F("https://github.com/beyarkay/eskom-calendar/archive/refs/heads/main.zip", "eskom-calendar-main.zip", lic="GPL-3.0"),
 "cct_supply_blocks": A(f"{CCT}/Theme_Based/Basic_Services_Infrastructure/MapServer/5", lic="City of Cape Town"),
 "cct_supply_authority": A(f"{CCT}/Theme_Based/Basic_Services_Infrastructure/MapServer/4", lic="City of Cape Town"),
 "cct_wayleave_substations": A(f"{CCT}/Theme_Based/Wayleave_Infrastructure/MapServer/4", lic="City of Cape Town"),
 "cct_wayleave_layer6": A(f"{CCT}/Theme_Based/Wayleave_Infrastructure/MapServer/6", big=True, lic="City of Cape Town (322k lines)"),
 "eskom_lines_agol": A("https://services6.arcgis.com/FLt9QphUNmJwmx9j/arcgis/rest/services/eskom_lines/FeatureServer/4", lic="as published on ArcGIS Online"),
 "eskom_mv_garden_route": A("https://services5.arcgis.com/G8Pqo3DzH8fIDN8F/arcgis/rest/services/Eskom_MV_Lines_Garden_Route_Area/FeatureServer/176", lic="Garden Route DM"),
 "solplaatje_electricity": A(f"{NC}/SPM_Electricity/FeatureServer/14", lic="Sol Plaatje LM"),
 # --- farms (Department of Agriculture AGIS)
 "farms_field_boundaries_2007": A(f"{NDA}/Landuse/MapServer/4", lic="DALRRD AGIS"),
 "farms_irrigated_1995": A(f"{NDA}/Landuse/MapServer/5", lic="DALRRD AGIS"),
 "farms_population_2016": A(f"{NDA}/Landuse/MapServer/12", lic="DALRRD AGIS"),
 "farms_landcover2000": A(f"{NDA}/Landuse/MapServer/17", lic="DALRRD AGIS (whole layer; the project kept agri and mines only)"),
 **{f"farms_crops_{k}": A(f"{NDA}/Crops/MapServer/{n}", lic="DALRRD crop census") for k, n in (("FS_2007", 12), ("GP_2006", 13), ("MP_2006", 14), ("NW_2006", 15))},
 **{f"agri_infra_{n}": A(f"{NDA}/Infrastructure/MapServer/{n}", lic="DALRRD AGIS") for n in (2,3,4,5,6,7,8,9,15,17,18,19,20,21,22,23,24,25,26,27,29,30,31,32,34,42)},
 # --- population
 **{f"hrsl_{t}": F(f"https://dataforgood-fb-data.s3.amazonaws.com/hrsl-cogs/hrsl_general/v1.5/{t}", lic="Meta HRSL CC BY 4.0")
    for t in ("cog_globallat_-30_lon_10_general-v1.5.2.tif", "cog_globallat_-30_lon_20_general-v1.5.2.tif",
              "cog_globallat_-30_lon_30_general-v1.5.4.tif", "cog_globallat_-40_lon_10_general-v1.5.2.tif",
              "cog_globallat_-40_lon_20_general-v1.5.2.tif", "cog_globallat_-40_lon_30_general-v1.5.2.tif")},
 # --- transport
 **{f"ourairports_{n}": F(f"https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/{n}.csv", lic="public domain")
    for n in ("airports", "runways", "navaids", "airport-frequencies", "countries", "regions")},
 "opensky_flightlists": {"kind": "zenodo", "record": "5815448", "big": True, "licence": "OpenSky Network CC BY 4.0 (7 GB)"},
 "worldbank_ship_density": {"kind": "manual", "url": "https://datacatalog.worldbank.org/search/dataset/0037580",
                            "note": "Global Shipping Traffic Density: download shipdensity_global.zip by hand (the API rate-limits scripts)", "licence": "CC BY 4.0"},
 "shipping_lanes": F("https://github.com/newzealandpaul/Shipping-Lanes/archive/refs/heads/main.zip", "Shipping-Lanes-main.zip", lic="CC BY 4.0"),
 # --- traffic (CSIR / SANRAL RAMS)
 "traffic_sanral_roads": A(f"{CSIR}/Hosted/Sanral_roads/FeatureServer/0", lic="SANRAL via CSIR"),
 "traffic_e80_speed": A(f"{CSIR}/E80s_webmap_MIL1/MapServer/7", lic="SANRAL via CSIR"),
 "traffic_rams_provincial_road": A(f"{CSIR}/Hosted/rams_provincial_road/FeatureServer/0", lic="RAMS via CSIR"),
 # --- points of interest
 "schools_dbe": F("https://www.education.gov.za/LinkClick.aspx?fileticket=cF64WfdfGD0%3d", "DBE_National_schools.xlsx", lic="Department of Basic Education (drop column Addressee)"),
}

# ---------------------------------------------------------------- http
CTX = ssl.create_default_context()
def http(url, data=None, tries=5, timeout=180):
    err = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, data=urllib.parse.urlencode(data).encode() if data else None,
                                         headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            err = e
            if e.code in (400, 403, 404): break
        except Exception as e:
            err = e
        time.sleep(3 * (i + 1))
    raise RuntimeError(f"{url[:120]}: {err}")

def download(url, path, tries=4):
    if os.path.exists(path) and os.path.getsize(path) > 0: return "kept"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=300, context=CTX) as r, open(path + ".part", "wb") as o:
                while True:
                    b = r.read(1 << 20)
                    if not b: break
                    o.write(b)
            os.replace(path + ".part", path); return "ok"
        except Exception as e:
            err = e; time.sleep(5 * (i + 1))
    return f"FAIL {err}"

def meta(dirp, job, extra=None):
    os.makedirs(dirp, exist_ok=True)
    d = {k: v for k, v in job.items() if k != "jobs"}
    d["fetched_utc"] = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None).isoformat(timespec="seconds") + "Z"
    if extra: d.update(extra)
    json.dump(d, open(os.path.join(dirp, "_source.json"), "w"), indent=1)

# ---------------------------------------------------------------- kinds
def run_file(name, job, out):
    fn = job.get("name") or urllib.parse.unquote(os.path.basename(urllib.parse.urlparse(job["url"]).path))
    d = os.path.join(out, name); meta(d, job)
    return download(job["url"], os.path.join(d, fn))

def run_arcgis(name, job, out, workers=3, step=1000):
    url = job["url"].rstrip("/"); d = os.path.join(out, name); pg = os.path.join(d, "pages")
    info = json.loads(http(url + "?f=json"))
    allf = [f["name"] for f in info.get("fields", [])]
    want = allf if job["fields"] == "*" else [f for f in job["fields"].split(",") if f in allf]
    fields = [f for f in want if not PRIVATE.search(f)]
    skipped = sorted(set(allf) - set(fields))
    meta(d, job, {"layer_name": info.get("name"), "fields_requested": fields, "fields_not_requested": skipped,
                  "layer_info": info})
    step = min(step, info.get("maxRecordCount") or step)
    oidf = next((f["name"] for f in info.get("fields", []) if f.get("type") == "esriFieldTypeOID"), "OBJECTID")
    try:
        ids = json.loads(http(url + "/query", {"where": job["where"], "returnIdsOnly": "true", "f": "json"}, timeout=900))
        oids = sorted(ids["objectIds"] or [])
        chunks = [("ids", oids[i:i + step]) for i in range(0, len(oids), step)]
    except Exception:  # very large layers: fall back to OBJECTID ranges
        st = json.loads(http(url + "/query", {"where": "1=1", "f": "json", "outStatistics": json.dumps(
            [{"statisticType": "max", "onStatisticField": oidf, "outStatisticFieldName": "mx"}])}))
        mx = int(st["features"][0]["attributes"]["mx"]); oids = range(mx + 1)
        chunks = [("range", (a, a + step)) for a in range(0, mx + 1, step)]
    os.makedirs(pg, exist_ok=True)
    def one(k):
        f = os.path.join(pg, f"{k:06d}.json.gz")
        if os.path.exists(f): return 0
        how, c = chunks[k]
        q = {"objectIds": ",".join(map(str, c))} if how == "ids" else \
            {"where": f"{oidf}>={c[0]} AND {oidf}<{c[1]}" + ("" if job["where"] == "1=1" else f" AND ({job['where']})")}
        q.update({"outFields": ",".join(fields), "outSR": "4326", "f": "geojson", "returnGeometry": "true"})
        body = http(url + "/query", q)
        json.loads(body)  # must parse
        with gzip.open(f + ".part", "wb") as o: o.write(body)
        os.replace(f + ".part", f); time.sleep(1); return 1
    done = fail = 0
    with cf.ThreadPoolExecutor(workers) as ex:
        for k, r in zip(range(len(chunks)), ex.map(lambda k: _safe(one, k), range(len(chunks)))):
            if r == "FAIL": fail += 1
            else: done += 1
            if (k + 1) % 50 == 0: print(f"   {name}: {k+1}/{len(chunks)} pages", flush=True)
    return f"ok {len(oids)} features, {len(chunks)} pages" if not fail else f"PARTIAL {fail} pages failed (re-run)"

def _safe(fn, *a):
    try: return fn(*a)
    except Exception as e:
        print("   page failed:", e, flush=True); return "FAIL"

def run_multi(name, job, out):
    return "; ".join(run_arcgis(f"{name}/layer{j['url'].rsplit('/',1)[1]}", j, out) for j in job["jobs"])

def run_copernicus(name, job, out):
    B = "https://copernicus-dem-30m.s3.amazonaws.com/"; d = os.path.join(out, name); meta(d, job)
    tiles = []
    for n in http(B + "tileList.txt").decode().split():
        p = n.split("_")
        if p[4][0] == "S" and p[6][0] == "E" and 22 <= int(p[4][1:]) <= 35 and 16 <= int(p[6][1:]) <= 32:
            tiles.append(n)
    with cf.ThreadPoolExecutor(6) as ex:
        res = list(ex.map(lambda n: download(f"{B}{n}/{n}.tif", os.path.join(d, n + ".tif")), tiles))
    bad = [r for r in res if r.startswith("FAIL")]
    return f"ok {len(tiles)} tiles" if not bad else f"PARTIAL {len(bad)} failed"

def run_rolls(name, job, out):
    d = os.path.join(out, name); meta(d, job)
    urls = [u.strip() for u in open(os.path.join(HERE, "roll_urls.txt"), encoding="utf-8") if u.strip()]
    rows = []
    def one(u):
        p = urllib.parse.urlparse(u)
        b = urllib.parse.unquote(os.path.basename(p.path)) or "index"
        if p.query: b += "_" + hashlib.md5(p.query.encode()).hexdigest()[:8]
        f = os.path.join(d, p.netloc, re.sub(r'[<>:"/\\|?*]', "_", b))
        s = download(urllib.parse.quote(u, safe=":/?&=%#+~"), f)
        if s.startswith("FAIL") and u.startswith("http://"):
            s2 = download("https://" + urllib.parse.quote(u[7:], safe=":/?&=%#+~"), f)
            if not s2.startswith("FAIL"): s = s2
        return [u, os.path.relpath(f, d), s]
    with cf.ThreadPoolExecutor(4) as ex:
        rows = list(ex.map(one, urls))
    with open(os.path.join(d, "manifest.csv"), "w", newline="", encoding="utf-8") as fo:
        w = csv.writer(fo); w.writerow(["url", "file", "status"]); w.writerows(rows)
    bad = sum(r[2].startswith("FAIL") for r in rows)
    return f"ok {len(rows)-bad}/{len(rows)} roll files" + (f", {bad} failed (see manifest.csv)" if bad else "")

def run_iec(name, job, out):
    d = os.path.join(out, name); meta(d, job)
    B = "https://results.elections.org.za/home/LGEPublicReports/1887/Downloadable%20Party%20Results/"
    P = {"EC": "EC", "FS": "FS", "GP": "GP", "KZN": "KZN", "GT": "GP", "LIM": "LIM", "MP": "MP", "NW": "NW", "NC": "NC", "WC": "WP"}
    codes = [r for r in csv.DictReader(open(os.path.join(HERE, "municipalities.csv"), encoding="utf-8"))]
    ok = 0
    for r in codes:
        s = download(f"{B}{P[r['province']]}/{r['code']}_Ward.csv", os.path.join(d, f"{r['code']}_Ward.csv"))
        ok += not s.startswith("FAIL"); time.sleep(1)
    return f"ok {ok}/{len(codes)} municipalities"

def run_zenodo(name, job, out):
    d = os.path.join(out, name); rec = json.loads(http(f"https://zenodo.org/api/records/{job['record']}")); meta(d, job)
    res = [download(f["links"]["self"], os.path.join(d, f["key"])) for f in rec["files"]]
    return f"ok {sum(not r.startswith('FAIL') for r in res)}/{len(res)} files"

def run_manual(name, job, out):
    d = os.path.join(out, name); meta(d, job)
    return "MANUAL: " + job["note"] + " -> " + job["url"]

KINDS = {"file": run_file, "arcgis": run_arcgis, "multi": run_multi, "copernicus": run_copernicus,
         "rolls": run_rolls, "iec": run_iec, "zenodo": run_zenodo, "manual": run_manual}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("--big", action="store_true")
    ap.add_argument("--only", default=""); ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    only = [x for x in a.only.split(",") if x]
    sel = {k: v for k, v in JOBS.items() if (not only or any(k.startswith(o) for o in only)) and (a.big or only or not v.get("big"))}
    if a.list:
        for k, v in JOBS.items(): print(f"{k:34s} {v['kind']:10s} {'BIG ' if v.get('big') else ''}{v.get('url','')}")
        return
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "_log.csv"); new = not os.path.exists(log)
    with open(log, "a", newline="", encoding="utf-8") as lf:
        w = csv.writer(lf)
        if new: w.writerow(["utc", "job", "result"])
        for k, v in sel.items():
            print(f"== {k}", flush=True)
            try: r = KINDS[v["kind"]](k, v, a.out)
            except Exception as e: r = f"FAIL {e}"
            print("  ", r, flush=True)
            w.writerow([datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None).isoformat(timespec="seconds"), k, r]); lf.flush()

if __name__ == "__main__":
    main()
