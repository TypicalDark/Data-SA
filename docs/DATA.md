# Data rules

The data store (raw archive, PostGIS, exporters) is specified in
`grid-of-sa/datastore/DATA_ARCHITECTURE.md` in the project files. This repository reads only its
exports.

## Packs the back-end reads

| Pack | Format | Built by | Layers or tables |
|---|---|---|---|
| `admin` | PMTiles, MVT | `export_tiles.py --pack admin` | province, district, municipality, ward, suburb, label |
| `grid` | PMTiles, MVT | `export_tiles.py --pack grid` | line, facility |
| `parcels` | PMTiles, MVT | `tools/webpack/build_parcels_pack.py` (until the store's `export_tiles.py --pack parcels`) | parcel: sg21, type, label, town, area, muni, and where a roll matched value, cat, use, sale, sale_date, roll (plus address and zoning from Joburg's roll); zoom 12-14, about 9M plots |
| `h3`, `places` | PMTiles, MVT | `export_tiles.py` | hex, place |
| `web-grid`, `web-water`, `web-risk`, `web-nature`, `web-vote`, `web-prop`, `web-land`, `web-places`, `web-move` | PMTiles, MVT | `tools/webpack/build_web_packs.py` | the website map's layers, one vector layer per website layer id (`x:pa` is `x_pa`) |
| `raster-relief`, `raster-fire`, ... (ten) | PMTiles, PNG | `tools/webpack/build_raster_packs.py` | the website's images as 256 px tiles |
| `base` | PMTiles, MVT | `tools/webpack/build_base_pack.py` | road (r class 0-7, name, ref, bridge, tunnel), rail, land (c class), poi (k kind), place (r rank); the website's base map, zoom 5-9 from its overview and 10-14 from its regional tiles (about 180 MB) |
| `buildings` | PMTiles, GSB1 tiles (gzip) | `tools/webpack/build_buildings_pack.py` | 44.2M footprints at zoom 12 in the website's GSB1 encoding (OSM, Google Open Buildings, Microsoft; about 230 MB); the app decodes them in `gsb.cpp` |
| `find` | JSON | the website's `data/x/find.json`, copied | municipalities (outline, people, area, median value, ward winners), NTCSA free capacity, dams, airports, ports, power stations, towns; served at `/v1/data/find` |
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
out.

`tools/webpack/build_raster_packs.py` turns the website's images into ten PNG tile packs
(`raster-relief`, `raster-fire`, `raster-flood`, `raster-ships`, `raster-farms`, `raster-rain12`,
`raster-rainn`, `raster-pvout`, `raster-ghi`, `raster-roof`, about 87 MB together). The website stretches each image linearly in Web Mercator over
its bounds (deck.gl BitmapLayer); the builder cuts the same images into 256 px tiles up to the zoom
that matches each image's resolution, and the app stretches the last level beyond it. Climate grids
(`x/clim/*.json`) are coloured with the website's ramps. The Global Solar Atlas images (solar yield,
GHI, rooftop potential) are regular in degrees, so the builder resamples their rows to Mercator first.

`tools/webpack/build_base_pack.py` turns the website's base map (`data/bm/overview.json` and the
106 regional tiles `data/bm/r/X_Y.json`, from OpenStreetMap) into the `base` pack. Below zoom 10 it
holds the overview (main roads, rail, big land areas, towns); from 10 every road class, land use,
landmarks and small places, with minor road classes starting at 11, 12 and 14 as on the website.

`tools/webpack/build_parcels_pack.py` builds the `parcels` pack from the project's parcel files: the
national SG cadastre per municipality, the newer KZN copy, and the Joburg and Ekurhuleni metro stands
(which replace SG plots with the same `sg21`), joined to the valuation rolls by `sg21`. No source has
owner names or ID numbers, and the roll `unit` column is never joined.

## Development pack

`devdata/packs/` is built by `tools/devpack/make_devpack.py` from the UI prototype's data bundle
(213 municipalities with simplified outlines, Census 2022 population, median roll values, 2026 ward
winners, the power grid). It has no wards, suburbs or plots. See `devdata/README.md`.

## Known tool issue

tippecanoe 2.49 as packaged in Ubuntu 24.04 keeps only one feature per tile when features carry a
per-feature `"tippecanoe": {"minzoom": n}` member. The development pack therefore stores `minzoom` as a
property and the app applies zoom gates itself. Check `export_tiles.py` output with
`tippecanoe-decode` before relying on per-feature zoom members.
