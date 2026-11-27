"""Build the desktop plots pack (all 9M cadastral parcels with roll values) from the project's parcel files.

    python tools/webpack/build_parcels_pack.py PARCELS_DIR OUT_DIR [--only CODE ...]

PARCELS_DIR is grid-of-sa/parcels: muni/{CODE}.geojsonl.gz (national SG copy and WC SG), kzn/ (newer KZN
copy, replaces muni/ files with the same name), gp_extra/{JHB,EKU}_metro.geojsonl.gz (newer metro
stands, replace SG parcels with the same sg21) and parcel_values.csv.gz (roll values joined by sg21).

Writes parcels.pmtiles, vector layer `parcel`, zoom 12-14 (the app overzooms to 22; the website shows
plots from 12.5). Properties: sg21, type, label (erf / portion / farm), town, area (m2), muni, and
where a roll matched: value (market value, rand), cat (roll category), use, sale (price), sale_date,
roll (name and period), plus address and zoning from the Joburg roll.

POPIA: no source carries owner names or ID numbers and none are read; the roll `unit` column was
never joined (some rolls put owner names there). Needs tippecanoe.
"""
import argparse, csv, glob, gzip, io, json, os, subprocess, sys, tempfile

csv.field_size_limit(1 << 24)


def num(v):
    try:
        return float(v) if v not in (None, "") else None
    except ValueError:
        return None


def load_values(path):
    vals = {}
    with gzip.open(path, "rt", encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            v = {"value": num(r["market_value"]), "cat": r["category"] or None, "use": r["usage"] or None,
                 "sale": num(r["sale_price"]), "sale_date": r["sale_date"] or None,
                 "roll": " ".join(x for x in (r["roll"], r["roll_period"]) if x) or None,
                 "units": int(num(r["units"]) or 1)}
            vals[r["sg21"]] = {k: x for k, x in v.items() if x is not None and not (k == "units" and x == 1)}
    return vals


def label(p):
    t = p.get("type")
    if p.get("stand"):
        return p["stand"]
    if t in ("farm", "farm_portion"):
        name = p.get("farm_name") or p.get("parcel") or ""
        por = p.get("portion")
        return f"Ptn {int(por)} of {name}" if por and str(por).strip("0") else str(name)
    erf = p.get("parcel") or p.get("erf") or ""
    por = p.get("portion")
    s = f"Erf {str(erf).lstrip('0') or erf}"
    if por and str(por).strip("0"):
        s += f" ptn {str(por).lstrip('0')}"
    return s


def props(p, vals):
    sg = p.get("sg21")
    out = {"sg21": sg, "type": p.get("type"), "label": label(p),
           "town": p.get("town") or p.get("township") or p.get("farm_name"), "area": p.get("area_m2"),
           "muni": p.get("muni"), "scheme": p.get("scheme")}
    if p.get("market_value") is not None:  # Joburg GV2023 on the stand itself
        out.update({"value": p.get("market_value"), "cat": p.get("roll_category"), "sale": p.get("last_sale_price"),
                    "sale_date": p.get("last_sale_date"), "roll": "JHB GV2023", "address": p.get("address"),
                    "zoning": p.get("zoning")})
    elif sg and sg in vals:
        out.update(vals[sg])
    if isinstance(out.get("area"), float):
        out["area"] = round(out["area"], 1)
    return {k: v for k, v in out.items() if v not in (None, "")}


def lines(path):
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for ln in f:
            if ln.strip():
                yield json.loads(ln)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("parcels")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    vals = load_values(os.path.join(a.parcels, "parcel_values.csv.gz"))
    print(f"values: {len(vals)} parcels", file=sys.stderr)
    files = {os.path.basename(p): p for p in glob.glob(os.path.join(a.parcels, "muni", "*.geojsonl.gz"))}
    files.update({os.path.basename(p): p for p in glob.glob(os.path.join(a.parcels, "kzn", "*.geojsonl.gz"))})
    metro = {"JHB": "JHB_metro", "EKU": "EKU_metro"}
    tmp = tempfile.mkdtemp(prefix="gsa_parcels_")
    src = os.path.join(tmp, "parcels.geojsonl")
    n = withv = 0
    with open(src, "w", encoding="utf-8") as out:
        def put(f):
            nonlocal n, withv
            if not f.get("geometry"):
                return
            pr = props(f.get("properties") or {}, vals)
            n += 1
            withv += "value" in pr
            out.write(json.dumps({"type": "Feature", "properties": pr, "geometry": f["geometry"]},
                                 ensure_ascii=False, separators=(",", ":")) + "\n")
        for name in sorted(files):
            code = name.split("__")[0].split(".")[0]
            if a.only and code not in a.only:
                continue
            replaced = set()
            if code in metro and "__" not in name:
                mp = os.path.join(a.parcels, "gp_extra", metro[code] + ".geojsonl.gz")
                for f in lines(mp):
                    sg = (f.get("properties") or {}).get("sg21")
                    if sg:
                        replaced.add(sg)
                    put(f)
            for f in lines(files[name]):
                if replaced and (f.get("properties") or {}).get("sg21") in replaced:
                    continue
                put(f)
            print(f"  {name}: {n} so far", file=sys.stderr)
    print(f"parcels: {n}, with a roll value {withv}", file=sys.stderr)
    out = os.path.join(a.out, "parcels.pmtiles")
    subprocess.run(["tippecanoe", "-o", out, "--force", "-q", "-Z", "12", "-z", "14", "-l", "parcel",
                    "-n", "Grid of SA plots", "-A", "Cadastre: Chief Surveyor-General (via DFFE), WCG, KZN COGTA, "
                    "City of Johannesburg, City of Ekurhuleni; values: municipal valuation rolls",
                    "--drop-densest-as-needed", "--no-tiny-polygon-reduction", "--simplification=2",
                    "--detect-shared-borders", "-P", src], check=True)
    os.remove(src)
    print(f"parcels.pmtiles: {os.path.getsize(out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
