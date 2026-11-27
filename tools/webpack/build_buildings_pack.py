"""Build the desktop buildings pack from the website's national buildings bundles (GSB1).

    python tools/webpack/build_buildings_pack.py BUNDLE_DIR OUT_DIR

BUNDLE_DIR holds the website's bundles (grid-of-sa/buildings/bundles/*.png): grayscale PNGs whose
pixel bytes are 'GSB1' | uint32 nTiles | nTiles x (uint16 tx, uint16 ty, uint32 offset, uint32 length)
| tile bytes (little endian). Each z12 tile's bytes go unchanged into buildings.pmtiles at zoom 12
(tile type unknown, gzip); the app decodes them (app/src/gsb.cpp, a port of the
website's gsb_decode.js). 44.2M footprints: OpenStreetMap (ODbL), Google Open Buildings v3
(CC BY 4.0) and Microsoft Global ML Building Footprints (ODbL), merged by VIDA.
"""
import argparse, glob, gzip, os, struct

from PIL import Image
from pmtiles.tile import Compression, TileType, zxy_to_tileid
from pmtiles.writer import Writer


def bundle_tiles(path):
    im = Image.open(path)
    if im.mode != "L":
        im = im.convert("L")
    u8 = im.tobytes()
    if u8[:4] != b"GSB1":
        raise ValueError(f"{path}: not GSB1")
    (n,) = struct.unpack_from("<I", u8, 4)
    for i in range(n):
        tx, ty, off, ln = struct.unpack_from("<HHII", u8, 8 + i * 12)
        yield tx, ty, u8[off:off + ln]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bundles")
    ap.add_argument("out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tiles = {}
    for p in sorted(glob.glob(os.path.join(a.bundles, "*.png"))):
        for tx, ty, data in bundle_tiles(p):
            tiles[zxy_to_tileid(12, tx, ty)] = data
    out = os.path.join(a.out, "buildings.pmtiles")
    with open(out, "wb") as f:
        w = Writer(f)
        for tid in sorted(tiles):
            w.write_tile(tid, gzip.compress(tiles[tid], 9, mtime=0))
        w.finalize(
            {"tile_type": TileType.UNKNOWN, "tile_compression": Compression.GZIP, "min_zoom": 12, "max_zoom": 12,
             "min_lon_e7": int(16.3 * 1e7), "min_lat_e7": int(-35.0 * 1e7), "max_lon_e7": int(33.0 * 1e7),
             "max_lat_e7": int(-22.0 * 1e7), "center_zoom": 12, "center_lon_e7": int(28.0 * 1e7),
             "center_lat_e7": int(-26.2 * 1e7)},
            {"name": "Grid of SA buildings (GSB1)", "format": "gsb1",
             "attribution": "© OpenStreetMap contributors (ODbL); Google Open Buildings (CC BY 4.0); Microsoft (ODbL); VIDA"},
        )
    print(f"buildings: {len(tiles)} z12 tiles ({os.path.getsize(out) / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
