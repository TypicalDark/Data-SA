# datastore/

Raw archive, PostGIS data store and exporters for Grid of SA. The full spec is
[DATA_ARCHITECTURE.md](DATA_ARCHITECTURE.md).

| Path | What |
|---|---|
| `archive/gsa_archive.py` | immutable raw archive: init, fetch (wraps `fetch_all.py`), ingest, adopt, verify, list |
| `archive/fetch_all.py`, `roll_urls.txt`, `municipalities.csv` | the source kit (same as `gsa-8-source-kit.zip`) |
| `schema/001_core.sql` | PostGIS schema: meta, core, net, ts, agg, stage |
| `etl/gsa_load.py` | loaders: sources, admin, suburbs, population, parcels, valuation, grid, climate, dams |
| `etl/build_tree.sql` | tree paths, parcel placement, roll-ups, H3, search |
| `export/export_tiles.py` | PMTiles packs via tippecanoe (zoom bands in `meta.tile_layer`) |
| `export/export_pack.py` | SQLite pack for the back-end (FTS5 search, packed time series) |
| `export/build_packs.py` | release folder: versioned packs, `manifest.json`, `packs/installed.json` (`gsa-backend --data RELEASE`) |
| `run_all.sh` | every step in order, ending in a release folder |
