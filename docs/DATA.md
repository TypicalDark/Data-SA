# Data rules

The data store (raw archive, PostGIS, exporters) is specified in
`grid-of-sa/datastore/DATA_ARCHITECTURE.md` in the project files. This repository reads only its
exports.

## Packs the back-end reads

| Pack | Format | Built by | Layers or tables |
|---|---|---|---|
| `admin` | PMTiles, MVT | `export_tiles.py --pack admin` | province, district, municipality, ward, suburb, label |
| `grid` | PMTiles, MVT | `export_tiles.py --pack grid` | line, facility |
| `parcels`, `parcels-<prov>` | PMTiles, MVT | `export_tiles.py --pack parcels` | parcel |
| `h3`, `places` | PMTiles, MVT | `export_tiles.py` | hex, place |
| `web-grid`, `web-water`, `web-risk`, `web-nature`, `web-vote`, `web-prop`, `web-land`, `web-places`, `web-move` | PMTiles, MVT | `tools/webpack/build_web_packs.py` | the website map's layers, one vector layer per website layer id (`x:pa` is `x_pa`) |
| `pack` | SQLite, `gsa-pack/1` | `export_pack.py` | tree, metric, parcel, valuation, node, edge, series, value, source, snapshot, search (FTS5), pack_info |

## Privacy (POPIA)

- Owner names, ID numbers and personal contact details are never requested, never stored and have
  no column anywhere. The roll `unit` column is never read.
- Values, sale prices and dates, addresses, zoning, use and ownership category are kept for the
  personal build, each row citing its roll.
- `source.resale_ok` marks what may go into a paid build; roll values and sale prices are not resellable
  without municipal licences, so a paid build shows suburb aggregates instead.

## Website packs

`tools/webpack/build_web_packs.py` turns the website map's published `data/` files into nine
`web-*.pmtiles` packs (about 160 MB together), so the app draws the same layers as the website until
the data store exports them directly. Every feature carries `name` and `detail` plus a few numbers.
Rights holders on mining rights, business phone numbers and anything naming a private person are left
out. Raster layers (fire, flood, solar yield, rain grids, hillshade) are not in these packs yet.

## Development pack

`devdata/packs/` is built by `tools/devpack/make_devpack.py` from the UI prototype's data bundle
(213 municipalities with simplified outlines, Census 2022 population, median roll values, 2026 ward
winners, the power grid). It has no wards, suburbs or plots. See `devdata/README.md`.

## Known tool issue

tippecanoe 2.49 as packaged in Ubuntu 24.04 keeps only one feature per tile when features carry a
per-feature `"tippecanoe": {"minzoom": n}` member. The development pack therefore stores `minzoom` as a
property and the app applies zoom gates itself. Check `export_tiles.py` output with
`tippecanoe-decode` before relying on per-feature zoom members.
