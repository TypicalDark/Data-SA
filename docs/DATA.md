# Data rules

The data store (raw archive, PostGIS, exporters) lives in `datastore/` and is specified in
[`datastore/DATA_ARCHITECTURE.md`](../datastore/DATA_ARCHITECTURE.md). The back-end reads only its
exports: a release folder made by `datastore/export/build_packs.py`.

## Packs the back-end reads

| Pack | Format | Built by | Layers or tables |
|---|---|---|---|
| `admin` | PMTiles, MVT | `export_tiles.py --pack admin` | province, district, municipality, ward, suburb, label |
| `grid` | PMTiles, MVT | `export_tiles.py --pack grid` | line, facility |
| `parcels-<prov>` (`parcels-wc`, `parcels-gt`, ...) | PMTiles, MVT | `export_tiles.py --pack parcels` | parcel, z13-16, one pack per province (`region` in installed.json) |
| `h3`, `places` | PMTiles, MVT | `export_tiles.py` | hex, place |
| `web-grid`, `web-water`, `web-risk`, `web-nature`, `web-vote`, `web-prop`, `web-land`, `web-places`, `web-move` | PMTiles, MVT | `tools/webpack/build_web_packs.py` | the website map's layers, one vector layer per website layer id (`x:pa` is `x_pa`) |
| `raster-relief`, `raster-fire`, ... (ten) | PMTiles, PNG | `tools/webpack/build_raster_packs.py` | the website's images as 256 px tiles |
| `find` | JSON | the website's `data/x/find.json`, copied | municipalities (outline, people, area, median value, ward winners), NTCSA free capacity, dams, airports, ports, power stations, towns; served at `/v1/data/find` |
| `pack` | SQLite, `gsa-pack/1` | `export_pack.py` | tree, metric, parcel, valuation, node, edge, series, series_data, source, snapshot, search (FTS5), pack_info |

## Release folder, manifest and installed.json

`build_packs.py RELEASE --tiles TILES_DIR --pack pack.sqlite --add web-*.pmtiles raster-*.pmtiles find.json`
writes:

```
RELEASE/manifest.json            gsa-manifest/1  what the updater downloads
RELEASE/packs/installed.json     gsa-installed/1 what PackStore reads
RELEASE/packs/<id>/<id>-<version>.<ext>
```

Each pack entry has `kind` (`tiles`, `sqlite`, `json`), `version`, `file` (relative to `packs/`),
`bytes`, `sha256`, and for tiles `tile_type` (`mvt` or `png`), zooms and bounds; split packs add
`region`. Versions are `<YYYYMMDD>-<sha256 prefix>` and are kept from `--previous` when the bytes did
not change, so the updater skips unchanged packs. `manifest.json` adds `url` and `changed` per pack;
signing (an ed25519 signature over the manifest) comes with roadmap step 11. A release folder is a
complete data directory: `gsa-backend --data RELEASE` serves it.

## Time series in the pack

`series_data(series_id, t0, step, n, v)` stores each series as one row: `v` is little-endian float32,
one value per `step` from `t0`, NaN where missing. The 21 million monthly climate values take about
90 MB this way instead of 1 GB as rows, and `/v1/series` returns a whole chart in one read.

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
out.

`tools/webpack/build_raster_packs.py` turns the website's images into ten PNG tile packs
(`raster-relief`, `raster-fire`, `raster-flood`, `raster-ships`, `raster-farms`, `raster-rain12`,
`raster-rainn`, `raster-pvout`, `raster-ghi`, `raster-roof`, about 87 MB together). The website stretches each image linearly in Web Mercator over
its bounds (deck.gl BitmapLayer); the builder cuts the same images into 256 px tiles up to the zoom
that matches each image's resolution, and the app stretches the last level beyond it. Climate grids
(`x/clim/*.json`) are coloured with the website's ramps. The Global Solar Atlas images (solar yield,
GHI, rooftop potential) are regular in degrees, so the builder resamples their rows to Mercator first.

## Development pack

`devdata/packs/` is built by `tools/devpack/make_devpack.py` from the UI prototype's data bundle
(213 municipalities with simplified outlines, Census 2022 population, median roll values, 2026 ward
winners, the power grid). It has no wards, suburbs or plots. See `devdata/README.md`.

## Known tool issue

tippecanoe 2.49 as packaged in Ubuntu 24.04 keeps only one feature per tile when features carry a
per-feature `"tippecanoe": {"minzoom": n}` member. Neither the development pack nor `export_tiles.py`
uses it: `export_tiles.py` groups each layer's features into zoom bands, runs tippecanoe once per band
and merges the bands with tile-join. Every feature also carries its band's `minzoom` as a property, so
the app can gate drawing itself.
