"""Build raster tile packs from the website map's images and grids.

    python tools/webpack/build_raster_packs.py WEB_DATA_DIR OUT_DIR [--only NAME ...]

Maximum zooms match each image's own resolution; the app stretches the last level beyond it.

The website draws these as single images stretched over lon/lat bounds in Web Mercator (deck.gl
BitmapLayer). This cuts each into 256 px XYZ tiles (PNG, transparent where empty) and writes one
PMTiles pack per layer: raster-relief, raster-fire, raster-flood, raster-ships, raster-farms,
raster-rain12, raster-rainn, raster-pvout, raster-ghi, raster-roof. The solar images are the website's
asset-store blobs, saved as x/solar/<asset id>.png. The app maps website layer ids to these packs (layers.cpp).

Needs Pillow and the `pmtiles` Python package.
"""
import argparse, base64, gzip, io, json, math, os, sys

from PIL import Image
from pmtiles.tile import Compression, TileType, zxy_to_tileid
from pmtiles.writer import Writer

Image.MAX_IMAGE_PIXELS = None
FIREB = [16.347656, -34.957995, 32.958984, -22.024546]


def mx(lon):
    return (lon + 180) / 360


def my(lat):
    s = math.sin(math.radians(lat))
    return 0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)


class Source:
    """An image covering lon/lat bounds, linear in Web Mercator."""

    def __init__(self, path_or_image, bounds, opacity=1.0):
        self.src, self.b, self.opacity, self._im = path_or_image, bounds, opacity, None
        self.x0, self.x1 = mx(bounds[0]), mx(bounds[2])
        self.y0, self.y1 = my(bounds[3]), my(bounds[1])  # north edge has the smaller y

    def image(self):
        if self._im is None:
            im = self.src if isinstance(self.src, Image.Image) else Image.open(self.src)
            im = im.convert("RGBA")
            if self.opacity < 1:
                a = im.getchannel("A").point(lambda v: int(v * self.opacity))
                im.putalpha(a)
            self._im = im
        return self._im

    def release(self):
        if not isinstance(self.src, Image.Image):
            self._im = None


def render_tiles(sources, zmin, zmax, resample=Image.BILINEAR):
    """Yields (z, x, y, png bytes) for every non-empty tile."""
    for z in range(zmin, zmax + 1):
        n = 1 << z
        tiles = {}
        for s in sources:
            im = s.image()
            W, H = im.size
            tx0, tx1 = int(s.x0 * n), min(n - 1, int(s.x1 * n))
            ty0, ty1 = int(s.y0 * n), min(n - 1, int(s.y1 * n))
            for tx in range(tx0, tx1 + 1):
                for ty in range(ty0, ty1 + 1):
                    # the part of the source image under this tile
                    wx0, wx1, wy0, wy1 = tx / n, (tx + 1) / n, ty / n, (ty + 1) / n
                    u0 = (wx0 - s.x0) / (s.x1 - s.x0) * W
                    u1 = (wx1 - s.x0) / (s.x1 - s.x0) * W
                    v0 = (wy0 - s.y0) / (s.y1 - s.y0) * H
                    v1 = (wy1 - s.y0) / (s.y1 - s.y0) * H
                    t = im.transform((256, 256), Image.EXTENT, (u0, v0, u1, v1), resample=resample, fillcolor=(0, 0, 0, 0))
                    if t.getchannel("A").getbbox() is None:
                        continue
                    key = (tx, ty)
                    tiles[key] = Image.alpha_composite(tiles[key], t) if key in tiles else t
            s.release()
        for (tx, ty), t in sorted(tiles.items()):
            b = io.BytesIO()
            t.save(b, "PNG", optimize=True)
            yield z, tx, ty, b.getvalue()


def write_pack(path, tiles, name, bounds, zmin, zmax, attribution):
    entries = sorted(((zxy_to_tileid(z, x, y), data) for z, x, y, data in tiles), key=lambda e: e[0])
    with open(path, "wb") as f:
        w = Writer(f)
        for tid, data in entries:
            w.write_tile(tid, data)
        w.finalize(
            {
                "tile_type": TileType.PNG, "tile_compression": Compression.NONE,
                "min_zoom": zmin, "max_zoom": zmax,
                "min_lon_e7": int(bounds[0] * 1e7), "min_lat_e7": int(bounds[1] * 1e7),
                "max_lon_e7": int(bounds[2] * 1e7), "max_lat_e7": int(bounds[3] * 1e7),
                "center_zoom": zmin, "center_lon_e7": int((bounds[0] + bounds[2]) / 2 * 1e7),
                "center_lat_e7": int((bounds[1] + bounds[3]) / 2 * 1e7),
            },
            {"name": name, "attribution": attribution},
        )
    return len(entries)


def load_json(d, rel):
    j = json.load(open(os.path.join(d, rel), encoding="utf-8"))
    return json.loads(gzip.decompress(base64.b64decode(j))) if isinstance(j, str) else j


def grid_image(d, rel, colour):
    """A climate grid (int16, north-first rows) as an image, coloured by `colour(value)`."""
    g = load_json(d, rel)
    m = g["meta"]
    raw = gzip.decompress(base64.b64decode(g["data"]))
    import array
    a = array.array("h")
    a.frombytes(raw[: m["width"] * m["height"] * 2])
    if sys.byteorder == "big":
        a.byteswap()
    im = Image.new("RGBA", (m["width"], m["height"]))
    px = im.load()
    for i, v in enumerate(a):
        if v == m.get("nodata", 32767):
            continue
        px[i % m["width"], i // m["width"]] = colour(v * m.get("scale", 1))
    b = m["bounds"]
    return to_mercator(im.resize((m["width"] * 4, m["height"] * 4), Image.NEAREST), b), b


def to_mercator(im, b):
    """An image regular in degrees (equirectangular, north row first) resampled to Mercator rows."""
    W, H = im.size
    out = Image.new("RGBA", (W, H))
    y0, y1 = my(b[3]), my(b[1])
    for row in range(H):
        lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (y0 + (y1 - y0) * (row + 0.5) / H)))))
        src = int((b[3] - lat) / (b[3] - b[1]) * H)
        if 0 <= src < H:
            out.paste(im.crop((0, src, W, src + 1)), (0, row))
    return out


def ramp(stops):
    def f(v):
        if v <= stops[0][0]:
            c = stops[0][1]
        elif v >= stops[-1][0]:
            c = stops[-1][1]
        else:
            for (a, ca), (b, cb) in zip(stops, stops[1:]):
                if a <= v <= b:
                    t = (v - a) / (b - a)
                    c = tuple(int(ca[i] + (cb[i] - ca[i]) * t) for i in range(4))
                    break
        return c
    return f


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("web_data")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    d = a.web_data
    os.makedirs(a.out, exist_ok=True)

    def p(rel):
        return os.path.join(d, rel)

    jobs = {}
    jobs["relief"] = lambda: ([Source(p("hillshade.webp"), FIREB)], 3, 9, "Hillshade (website)", "Copernicus DEM via the website map")
    jobs["fire"] = lambda: ([Source(p("x/hz_fire.png"), FIREB)], 3, 8, "Fire history", "MODIS burnt area via the website map")

    def flood():
        meta = load_json(d, "x/flood/meta.json")
        chunks = [Source(p(f"x/flood/{n}.png"), b) for n, b in meta["chunks"]]
        lo = [Source(p("x/flood/lo.png"), meta["lo"])]
        return (lo, chunks), 3, 10, "Flood-prone land (modelled)", "Modelled flood zones via the website map"
    jobs["flood"] = flood

    def ships():
        air = load_json(d, "x/air.json")
        return [Source(p("x/ships.png"), air["ships"]["bounds"], 0.85)], 3, 6, "Ship density", "World Bank / IMF ship density 2015-2021"
    jobs["ships"] = ships
    jobs["farms"] = lambda: ([Source(p("x/farm_lo.png"), FIREB)], 3, 9, "Farm use", "DALRRD field crop boundaries via the website map")

    def rain12():
        im, b = grid_image(d, "x/clim/rain_last12_anom.json",
                           ramp([(-60, (166, 97, 26, 200)), (-20, (223, 194, 125, 160)), (0, (245, 245, 245, 40)),
                                 (20, (128, 205, 193, 160)), (60, (1, 133, 113, 200))]))
        return [Source(im, b)], 3, 8, "Rain, last 12 months vs normal", "NASA POWER / GPM IMERG"
    jobs["rain12"] = rain12

    def rainn():
        im, b = grid_image(d, "x/clim/rain_normal_annual.json",
                           ramp([(100, (255, 255, 204, 170)), (400, (161, 218, 180, 170)), (800, (65, 182, 196, 180)),
                                 (1200, (34, 94, 168, 190)), (2000, (12, 44, 132, 200))]))
        return [Source(im, b)], 3, 8, "Average yearly rain", "NASA POWER"
    jobs["rainn"] = rainn

    # Global Solar Atlas images; the website draws them in degrees (_imageCoordinateSystem LNGLAT)
    SOLB = [16, -35, 34, -22]
    for name, blob, title in (("pvout", "c54cb8475d84835b6af3e9ca50b631eb", "Solar yield"),
                              ("ghi", "47343fe645a9293d64bf4fb4d33f30dc", "Sunshine on flat ground (GHI)"),
                              ("roof", "970fc04885ce54c9e623053f0d769900", "Rooftop solar potential")):
        def solar(blob=blob, title=title):
            im = to_mercator(Image.open(p(f"x/solar/{blob}.png")).convert("RGBA"), SOLB)
            return [Source(im, SOLB)], 3, 8, title, "Global Solar Atlas (World Bank / Solargis) via the website map"
        jobs[name] = solar

    for name, job in jobs.items():
        if a.only and name not in a.only:
            continue
        try:
            srcs, z0, z1, title, attr = job()
        except FileNotFoundError as e:
            print(f"raster-{name}: missing {e.filename}, skipped", file=sys.stderr)
            continue
        if isinstance(srcs, tuple):  # flood: overview image to z8, chunks after
            lo, hi = srcs
            tiles = list(render_tiles(lo, z0, 8)) + list(render_tiles(hi, 9, z1))
            bounds = [min(s.b[0] for s in hi), min(s.b[1] for s in hi), max(s.b[2] for s in hi), max(s.b[3] for s in hi)]
        else:
            tiles = list(render_tiles(srcs, z0, z1))
            bounds = srcs[0].b
        out = os.path.join(a.out, f"raster-{name}.pmtiles")
        n = write_pack(out, tiles, title, bounds, z0, z1, attr)
        print(f"raster-{name}: {n} tiles, {os.path.getsize(out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
