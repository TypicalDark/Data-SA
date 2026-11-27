"""Grid of SA: build a release of data packs and the files the back-end and updater read.

    python build_packs.py RELEASE_DIR [--tiles DIR] [--pack FILE.sqlite] [--add FILE ...] [--base-url URL]
                          [--channel stable|beta] [--previous OLD_RELEASE_DIR/manifest.json]

Inputs are finished pack files: the output folder of export_tiles.py (--tiles), the SQLite pack from
export_pack.py (--pack), and any other packs (--add), such as the website packs from tools/webpack
(web-*.pmtiles, raster-*.pmtiles) or JSON data packs (find.json).

Output (RELEASE_DIR):
    manifest.json                  gsa-manifest/1: what the updater downloads (served from --base-url)
    packs/installed.json           gsa-installed/1: what PackStore reads (a full local install)
    packs/<id>/<id>-<version>.<ext>

Pack kinds follow the back-end: "tiles" (.pmtiles, tile_type mvt or png), "sqlite" (pack.sqlite,
schema gsa-pack/N) and "json" (small JSON data packs). A pack's version is content addressed,
<YYYYMMDD>-<first 10 hex of sha256>, and is carried over from --previous when the bytes did not
change, so an unchanged pack is never downloaded again. Split packs (parcels-wc ...) carry a region,
so a user can install only the provinces they want.
"""
import argparse, datetime, hashlib, json, os, re, shutil, sqlite3, struct, sys

ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
PROV_SUFFIX = {"ec": "EC", "fs": "FS", "gt": "GT", "kzn": "KZN", "lim": "LIM", "mp": "MP", "nc": "NC", "nw": "NW", "wc": "WC"}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def pmtiles_header(path):
    """The parts of the PMTiles v3 header the manifest repeats (so the updater can show them unopened)."""
    with open(path, "rb") as f:
        h = f.read(127)
    if h[:7] != b"PMTiles" or h[7] != 3: raise SystemExit(f"{path}: not a PMTiles v3 file")
    tile_type = {1: "mvt", 2: "png", 3: "jpg", 4: "webp", 5: "avif"}.get(h[99], "unknown")
    minz, maxz = h[100], h[101]
    minlon, minlat, maxlon, maxlat = (struct.unpack("<i", h[o:o + 4])[0] / 1e7 for o in (102, 106, 110, 114))
    return {"tile_type": tile_type, "minzoom": minz, "maxzoom": maxz, "bounds": [minlon, minlat, maxlon, maxlat]}

def sqlite_schema(path):
    c = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        fmt = c.execute("SELECT v FROM pack_info WHERE k='format'").fetchone()
    finally:
        c.close()
    m = re.match(r"^gsa-pack/(\d+)$", fmt[0] if fmt else "")
    if not m: raise SystemExit(f"{path}: pack_info.format is not gsa-pack/N")
    return int(m.group(1))

def describe(path):
    """(id, kind, ext, extra) for one pack file."""
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    if ext == ".pmtiles":
        kind, extra = "tiles", pmtiles_header(path)
    elif ext == ".sqlite":
        kind, extra = "sqlite", {"schema": sqlite_schema(path)}
        stem = "pack" if stem in ("gsa_pack", "pack") else stem
    elif ext == ".json":
        with open(path, encoding="utf-8") as f: json.load(f)  # must parse, as PackStore checks
        kind, extra = "json", {}
    else:
        raise SystemExit(f"{path}: unknown pack type {ext}")
    if not ID_RE.match(stem): raise SystemExit(f"{path}: '{stem}' is not a valid pack id")
    m = re.match(r"^[a-z0-9]+-([a-z]+)$", stem)
    if m and m.group(1) in PROV_SUFFIX: extra["region"] = PROV_SUFFIX[m.group(1)]
    return stem, kind, ext, extra

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--tiles", help="folder of .pmtiles from export_tiles.py")
    ap.add_argument("--pack", help="SQLite pack from export_pack.py")
    ap.add_argument("--add", nargs="*", default=[], help="other pack files (.pmtiles, .json, .sqlite)")
    ap.add_argument("--base-url", default="", help="where the release will be served, e.g. https://packs.example/stable/")
    ap.add_argument("--channel", default="stable", choices=["stable", "beta"])
    ap.add_argument("--previous", help="manifest.json of the last release, to keep versions of unchanged packs")
    ap.add_argument("--copy", action="store_true", help="copy files instead of hard-linking them")
    a = ap.parse_args()

    files = []
    if a.tiles: files += sorted(os.path.join(a.tiles, f) for f in os.listdir(a.tiles) if f.endswith(".pmtiles"))
    if a.pack: files.append(a.pack)
    files += a.add
    if not files: sys.exit("nothing to pack: give --tiles, --pack or --add")

    prev = {}
    if a.previous and os.path.exists(a.previous):
        prev = json.load(open(a.previous, encoding="utf-8")).get("packs", {})

    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d")
    packs_dir = os.path.join(a.out, "packs")
    os.makedirs(packs_dir, exist_ok=True)
    manifest = {"format": "gsa-manifest/1", "channel": a.channel,
                "built_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "pack_schemas": sorted({1}), "packs": {}}
    installed = {"format": "gsa-installed/1", "packs": {}}
    seen = set()
    for src in files:
        pid, kind, ext, extra = describe(src)
        if pid in seen: sys.exit(f"pack id {pid} given twice")
        seen.add(pid)
        digest = sha256(src)
        old = prev.get(pid)
        version = old["version"] if old and old.get("sha256") == digest else f"{today}-{digest[:10]}"
        rel = f"{pid}/{pid}-{version}{ext}"
        dest = os.path.join(packs_dir, *rel.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if not os.path.exists(dest):
            try:
                if a.copy: raise OSError
                os.link(src, dest)
            except OSError:
                shutil.copy2(src, dest)
        entry = {"kind": kind, "version": version, "file": rel, "bytes": os.path.getsize(dest), "sha256": digest, **extra}
        installed["packs"][pid] = entry
        manifest["packs"][pid] = dict(entry, url=(a.base_url.rstrip("/") + "/packs/" + rel) if a.base_url else rel,
                                      changed=not (old and old.get("sha256") == digest))
        print(f"  {pid:22s} {kind:6s} {entry['bytes']/1e6:9.1f} MB  {version}{'' if manifest['packs'][pid]['changed'] else '  (unchanged)'}")
    with open(os.path.join(packs_dir, "installed.json"), "w", encoding="utf-8") as f:
        json.dump(installed, f, indent=1)
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    total = sum(p["bytes"] for p in manifest["packs"].values())
    print(f"release in {a.out}: {len(manifest['packs'])} packs, {total/1e9:.2f} GB")

if __name__ == "__main__":
    main()
