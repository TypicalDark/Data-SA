# Grid of SA: data architecture

Raw archive, processed formats, data structures, database and serving. Version 2, 27 November 2026.

This document says how every dataset behind the map is kept, from the original download through to the
tiles the map reads. It fits the desktop app (C++ front-end that talks only to a versioned `/v1` API, the C++
back-end `gsa-backend`, PMTiles and SQLite data packs; see `docs/ARCHITECTURE.md`). The scripts it
describes are in `datastore/` of the Data-SA repository and have been run end to end on the national
data (section 9).

## 1. The short version

| Question | Answer |
|---|---|
| Raw data | Keep every original download **byte for byte** in an immutable, append-only archive on your PC, with a sha256 for every file and a provenance record for every fetch. A second copy goes on an external drive or a cloud bucket. |
| Working format | **PostgreSQL 16 + PostGIS**, with pgRouting (graphs), h3-pg (hexagon grid) and ltree (the admin tree). This is the one place where data is cleaned, joined and checked. |
| What the map reads | Exports from the database: **PMTiles** (the zoom pyramid) for shapes, and one **SQLite pack** for numbers, search, plot cards, the grid graph and time series. No database runs on a user's machine. |
| Tree structure | Yes, and two trees work together: a **spatial quadtree** (z/x/y tiles, where you are looking) and a **semantic tree** (country > province > district > municipality > ward > suburb > parcel, what you are looking at). Zoom picks the level; each level carries roll-ups of everything below it. |
| Other structures | R-tree spatial index, H3 hexagons, graphs for the grid, roads and rivers, long-format time series, versioned boundaries with crosswalks, full-text search, raster pyramids, GeoParquet for analysis (section 4). |
| MySQL or GraphQL | Neither as the database. GraphQL is an API style, not storage: it would sit on top of a database. MySQL's spatial support is too thin for this project (section 5). PostGIS is the recommendation. GraphQL can be added later as an optional endpoint for tree queries. |
| Where it lives | Raw archive and database on your PC (about 30 GB and 30 GB). Published packs in a cloud bucket. The project folder keeps code, this spec, catalogs and small samples only (section 7). |

## 2. Raw archive (keep everything, unedited)

### 2.1 Rules

1. **Byte for byte.** Files (PDF, XLS, CSV, ZIP, GeoTIFF, PBF) are stored exactly as the publisher served them. Nothing is renamed inside, re-encoded or cleaned.
2. **Map services have no file**, so every page an ArcGIS REST server returns is stored as-is (`pages/NNNNNN.json.gz`), with the layer's full metadata in `_source.json`. Together the pages are the original.
3. **Immutable snapshots.** Each fetch becomes a new snapshot folder named by its UTC time. Files are set read-only. A later fetch never overwrites an earlier one.
4. **No duplicates.** If a fetch is byte-identical to the latest snapshot of that source, it is not stored again. The catalog still records the check (`same_as`), so you know the source was unchanged on that date.
5. **Provenance for every snapshot:** source URL, publisher, fetch time, licence, whether resale is allowed, fetch tool version, and which fields were left out.
6. **Verifiable.** `gsa_archive.py verify` re-hashes every file against the catalog and reports anything changed or missing.
7. **The one exception to "unedited":** fields holding owner names, ID numbers or personal contact details are never requested from map services (the project's POPIA rule). Since they are never downloaded, nothing is edited afterwards; `_source.json` lists the fields that were not requested. Original roll files (PDF/XLS) are kept whole because they are the publisher's document, but the archive is private to your PC and the processing step never reads owner columns (section 8).

### 2.2 Layout

```
D:\GridOfSA\raw\
  _catalog\
    sources.json          registry: id, title, publisher, URL, kind, licence, resale, cadence, privacy
    snapshots.jsonl       one line per snapshot (append-only)
    files.jsonl           one line per file: snapshot, path, sha256, bytes (append-only)
  _staging\               downloads land here; only complete jobs move into sources\
  sources\
    cadastre_dffe_erven\
      20261128T061500Z\
        _source.json      provenance from the fetcher
        _snapshot.json    snapshot id, tree hash, file count, bytes
        pages\000000.json.gz ...
    valuation_rolls\
      20261128T063000Z\
        www.overstrand.gov.za\Register_of_Properties.zip
        manifest.csv
    osm\
      20261124T000000Z\south-africa-261124.osm.pbf
```

A snapshot id is `<source_id>@<UTC stamp>`, for example `cadastre_dffe_erven@20261128T061500Z`.
The tree hash is a sha256 over the sorted list of (path, file sha256), excluding `_source.json`, so two
fetches with the same content have the same tree hash even though they ran at different times.

### 2.3 Tools

`datastore/archive/gsa_archive.py` (standard library only, runs on Windows):

| Command | Does |
|---|---|
| `init ROOT` | creates the folders and writes `sources.json` (100 sources: the 85 jobs of the source kit plus 15 fetched other ways) |
| `fetch ROOT [--only a,b] [--big]` | runs the existing source kit `fetch_all.py` into `_staging`, then ingests every complete job as a snapshot; incomplete jobs stay in staging and finish on the next run |
| `ingest ROOT SOURCE_ID PATH... --url --licence --fetched` | adds files fetched some other way (the OSM pbf you already have, `dws_dams.zip`, gazette PDFs, manual downloads) |
| `adopt ROOT OLD_OUTPUT` | imports a folder from an earlier `fetch_all.py OUT` run, keeping its original fetch dates |
| `verify ROOT` | re-hashes everything |
| `list ROOT` | latest snapshot per source, and which sources are not archived yet |

### 2.4 What can and cannot be recovered

The project mostly kept processed copies, and most government hosts block the cloud, so the raw
archive has to be built on your PC with the source kit. Most sources re-download as they were. Some
will not match what the map used: OpenStreetMap moves daily (keep the `south-africa-261124.osm.pbf` you
have and `ingest` it), municipalities take roll files down, and live feeds only exist from the day you
start archiving them. The snapshot date makes the difference visible. Not scriptable at all: the Cape
Town GV2025 roll (per-erf search only), Tshwane GIS (login), SANRAL traffic yearbooks (on request),
NGI 5 m contours (sold).

## 3. Processed data: the trees

### 3.1 Two trees, joined by zoom

A single tree is the right instinct, but the map needs two that line up:

- **The spatial quadtree** answers "what is inside the part of the map I am looking at". Each zoom level splits every tile into four (z/x/y). PMTiles stores this pyramid in one file, ordered along a Hilbert curve so neighbouring tiles sit next to each other on disk. The map only ever fetches the handful of tiles on screen, at any zoom.
- **The semantic tree** answers "what is this place and what is it part of": country, province, district, municipality, ward, suburb, parcel. Every node carries roll-ups of everything below it (population, parcel count, median value, rainfall).

The zoom band table says which semantic level lives in which zoom levels of the quadtree:

| Level | Node | Id example | Zoom shown | Shape detail |
|---|---|---|---|---|
| 0 | Country | `ZA` | 0 to 4 | ~1 km |
| 1 | Province | `WC` | 0 to 12 | ~50 m |
| 2 | District (metros skip it) | `DC5` | 4 to 12 | ~50 m |
| 3 | Municipality | `WC053`, `CPT` | 5 to 12 | ~50 m |
| 4 | Ward | `W10503001` | 8 to 14 | full |
| 6 | Suburb / sub place | `SSGT0JR0352` | 11 to 14 | full |
| 7 | Parcel (in `core.parcel`) | `C02900020000000400000` | 12 to 14, overzoomed after | full |

Level 5 (Stats SA main place) is reserved. Labels have their own layer with a start zoom per level.
These bands live in the `meta.tile_layer` table, so changing them is a row edit, not a code change.

"Going deeper as you zoom" then works like this: at zoom 6 the tile holds municipalities, each already
carrying its totals. Zoom to 9 and the same area's tiles hold wards with ward totals; the municipality
outline stays as context. Click anything and the API answers from the tree table: its parent chain
(breadcrumb), its children, and their numbers, without touching geometry.

### 3.2 The tree in the database

All admin levels sit in one table, `core.admin_area`, with `parent_id` and an `ltree` path such as
`ZA.WC.DC5.WC053.W10503001`. That makes every tree question one indexed query:

```sql
-- children of a node
SELECT * FROM core.admin_area WHERE parent_id = 'WC053';
-- everything under a node, any depth
SELECT * FROM core.admin_area WHERE path <@ 'ZA.WC.DC5';
-- breadcrumb of a ward
SELECT * FROM core.admin_area WHERE path @> 'ZA.WC.DC5.WC053.W10503001' ORDER BY level;
```

Roll-ups are stored, not computed per click: `agg.area_metric(area_id, metric, as_of, v, n, source_id)`.
`build_tree.sql` sums additive metrics (population, households, voters) up every ancestor, takes medians
for values, and averages climate.

### 3.3 Where the tree is not a tree

Real places do not nest cleanly, so the design admits it instead of forcing it:

- **Suburbs cross ward lines.** A suburb's `parent_id` is the ward holding most of it; the full split (area share per ward) is in `core.area_crosswalk`. Strictly this is a graph (a DAG), and roll-ups that must be exact use the shares.
- **Boundaries change.** Each area row has a `boundary_set` (`MDB2021`, `MDB2026`, ...) and `valid_from`/`valid_to`. The 2021 to 2026 ward crosswalk from `wards2026/` goes into `core.area_crosswalk` with population shares, so history can be shown on either set.
- **Parcels have their own tree.** The 21-character SG code is hierarchical: registration division (1-4), township or farm region (5-8), parcel number (9-16), portion (17-21). `core.parcel` exposes these as generated columns, so "all portions of farm 347" or "every erf in this township" is a prefix match.
- **Networks are graphs**, not trees (section 4.3).

## 4. Other data structures

| Structure | Used for | Where |
|---|---|---|
| Tile pyramid (quadtree, Hilbert order) | every map layer, all zooms | PMTiles packs |
| Ltree hierarchy + stored roll-ups | drill-down, breadcrumbs, totals at every level | `core.admin_area`, `agg.area_metric` |
| R-tree (GiST index) | "what is under the cursor", "what is in this box", nearest substation (KNN `<->`) | every geometry column |
| H3 hexagons (res 4 to 9) | equal-area heatmaps, joining datasets with different boundaries, the hexagon view that also gets finer as you zoom | `h3_*` columns, `agg.h3_metric`, `h3.pmtiles` |
| Quadkeys / Z-order | sorting parcels so neighbours sit together, splitting packs by area, tile-aligned updates | `core.parcel.quadkey_z16` |
| Graphs | power grid trace and islands, road travel time, river flow, flight routes | `net.node`, `net.edge` (pgRouting layout) |
| Long-format time series | climate, dam levels, elections, roll values over cycles, load shedding | `ts.series`, `ts.value` |
| Versioned rows + crosswalks | boundary changes, roll cycles | `valid_from/valid_to`, `core.area_crosswalk` |
| Inverted index (full text + trigram) | one search box for places, wards, parcels, substations | `core.search`, FTS5 in the pack |
| Raster pyramids (COG overviews, raster PMTiles) | terrain, solar, population density, rain grids | raster PMTiles, COGs in the archive |
| Columnar (GeoParquet) | bulk analysis in DuckDB, exchange with other tools | optional export |
| Content-addressed store (sha256 + tree hash) | the raw archive, pack updates (only changed tiles move) | `_catalog`, pack manifests |

### 4.1 Spatial index (R-tree)

PostGIS builds an R-tree (GiST) on every geometry. It is what makes "parcels in this tile",
"which ward contains this point" and "nearest 132 kV substation" fast. In the desktop pack the
equivalent is the quadkey column plus PMTiles, so the back-end does not need SpatiaLite.

### 4.2 H3

H3 is a hierarchical hexagon grid: every cell has seven children at the next resolution, so it is a
tree as well, with equal-sized cells that ignore admin lines. Every parcel and place stores its
resolution 9 cell (about 0.1 km²). `build_tree.sql` rolls parcel counts and median values up to
resolutions 8 to 5, and the `hex` layer shows res 5 at z4-5, res 6 at z6-7, res 7 at z8-9, res 8 at z10 and
res 9 at z11-13. It is also the cleanest way to put rainfall (a grid), population (wards) and values
(parcels) on one footing.

### 4.3 Graphs

`net.node` and `net.edge` use the layout pgRouting expects (`id, source, target, cost, reverse_cost`),
with a `network` column so the power grid, roads and rivers share one structure.

- **Power:** edge cost is km. Inferred gap joins keep `inferred = true` and their `reason`, never silent. Gives grid trace (`pgr_dijkstra`), islands (`pgr_connectedComponents`), "which transmission substation feeds this ward" and N-1 style what-ifs (`pgr_bridges`). Substation capacity from the NTCSA GCCA 2025 goes on `net.node.capacity_mva`.
- **Roads:** edge cost is seconds per mode (car, walk, cycle) from OSM speed limits plus SA defaults, as in the reach thread. Isochrones come from `pgr_drivingDistance`; for national use the existing precomputed reach tiles stay the delivery format.
- **Rivers and water:** a directed graph (flow direction), so "everything upstream of this dam" is a traversal.

The pack copies `node` and `edge` without geometry, so the C++ back-end runs traces in memory (`backend/src/graph.cpp`, `/v1/trace`).

### 4.4 Time series

One long table, `ts.value(series_id, t, v, flag)`, partitioned by decade with a BRIN index on time, and
`ts.series` saying what each series is (entity, variable, unit, step, source). Loaded so far: 40,658
monthly climate series (4,468 wards × 9 variables + 1981 to 2026) and weekly dam levels. The same tables take
election results per cycle, valuation per roll cycle and load shedding stages. TimescaleDB can be added
later if live feeds grow; the schema does not need to change.

## 5. Database recommendation

**PostgreSQL 16 with PostGIS 3.4, pgRouting, h3-pg and ltree** as the working database, on your PC
(Docker image `postgis/postgis:16-3.4`, or the Windows installer with Stack Builder), later the same on a
server.

**GraphQL is not a database.** It is a way for the front-end to ask an API for nested data. It needs a
database underneath it either way. It would fit the tree well (`area { name children { name metrics } }`),
and tools such as PostGraphile or Hasura can generate it straight from this PostGIS schema. It fits tiles
badly (binary, cached by URL), so the desktop plan's REST `/v1` API stays the main interface. GraphQL can
be added later as one more endpoint if the front-end ends up making many small tree queries.

**Why not MySQL.** MySQL 8 has spatial types and R-tree indexes, but this project leans on things it lacks:

| Needed here | PostGIS | MySQL 8 |
|---|---|---|
| Dissolve wards into municipalities and provinces (`ST_Union` aggregate) | yes | no aggregate union |
| Repair broken polygons (`ST_MakeValid`), topology-safe simplify per zoom | yes | no `MakeValid`; basic simplify only |
| Vector tiles straight from the database (`ST_AsMVT`) | yes | no |
| Graph routing and trace (pgRouting) | yes | no |
| H3, ltree, trigram search, rasters | extensions | none |
| Geography type (true metres and km² on the globe) | yes | partial |
| Tooling (GDAL, tippecanoe pipelines, Martin, QGIS) | first class | limited |

**On users' machines** there is no database server: the app reads PMTiles and the SQLite pack. A
hosted web version can serve the same files, or run Martin against PostGIS for live tiles.
**DuckDB** with GeoParquet is the recommended side tool for one-off analysis.

## 6. The pipeline

```
raw archive  ->  stage.*  ->  core.* / net.* / ts.*  ->  agg.*  ->  PMTiles packs + SQLite pack (+ GeoParquet)
(immutable)      (load)       (clean, join, check)       (roll up)    (publish)
```

Every step is idempotent and replaces only the rows it owns. Every load writes a `meta.lineage` row:
target table, the snapshot ids it came from, the script and its hash, rows written. So any number on the
map can be traced to the raw file it came from.

| Step | Script | Status |
|---|---|---|
| Archive | `archive/gsa_archive.py` (wraps `archive/fetch_all.py`) | tested |
| Schema | `schema/001_core.sql` | tested |
| Load | `etl/gsa_load.py sources, admin, suburbs, population, parcels, valuation, grid, climate, dams` | tested on real data |
| Tree, keys, roll-ups, H3, search | `etl/build_tree.sql` | tested |
| Tiles | `export/export_tiles.py` (tippecanoe per zoom band + tile-join; packs admin, grid, parcels-<prov>, h3, places) | national run |
| Attribute pack | `export/export_pack.py` (SQLite, FTS5 search, packed time series) | national run |
| Release | `export/build_packs.py` (versions, sha256, `manifest.json`, `packs/installed.json`) | national run |
| Run everything | `run_all.sh` | written |
| Serve | `gsa-backend --data RELEASE` (C++, `backend/`) | national run |

Still to write, in this order: KZN COGTA and metro valuation extras, 2026 wards and the 2021-2026
crosswalk, IEC results into `ts`, farms and crops, transport, solar and wind, hazards and environment,
roads graph, raster packs (terrain, solar, rain) and GeoParquet export. The current loaders read the
project's processed files; each will gain a `--from-raw SNAPSHOT` mode that reads the archive directly
once the archive exists on your PC.

## 7. Where everything lives (given the 10 GB project limit)

The project folder holds 9.2 of its 10 GB and cannot take the raw archive or the database.

| Thing | Size (estimate) | Lives | Backup |
|---|---|---|---|
| Raw archive | 12 GB without the big sets, about 25-30 GB with DFFE erven, OpenSky and Copernicus | your PC, for example `D:\GridOfSA\raw` | external drive, or a private Cloudflare R2 / Backblaze B2 bucket (about R10 per month for 30 GB) |
| PostGIS database | about 25-35 GB with 9 M parcels and indexes (9 M parcels at about 2 KB each, 2.9 M roll rows, 21 M climate values) | your PC (Docker) | rebuildable from the archive; `pg_dump` weekly |
| PMTiles + SQLite packs (one release) | 4.4 GB national: plot tiles 0.96 GB in nine province packs, attribute pack 3.2 GB, the rest 0.2 GB | cloud bucket for the app, copy on your PC | the bucket |
| This spec and the scripts | under 1 MB | the Data-SA repository, `datastore/` | GitHub |

What this means for the project folder: once the archive and database exist on your PC, the processed
copies in the project folder become rebuildable outputs rather than the only copy. The download zips in
`grid-of-sa-download/` (3.4 GB) are a second copy of `grid-of-sa/` itself, so they are the first thing that
can go once you have them on your PC. The rest can stay until the loaders read from the archive.
Nothing has been deleted; that is your call.

## 8. Privacy and licence rules carried through

- Owner names, ID numbers and contact details are never requested from map services, never read from roll files (the roll `unit` column is skipped because some rolls put owner names there), and have no column anywhere in the schema. Loaders drop any column whose name matches the private pattern.
- Values, sale prices and dates, addresses, zoning, use and ownership category are kept, with the roll and its date cited on every row (`source_url`, `roll_period`).
- `meta.source.resale_ok` marks what may go into a paid product. Roll values and sale prices are `false` (municipal licences needed, as in the SaaS plan); NASA POWER, Copernicus, OSM and CC BY sets are `true`.

## 9. Test run (27 November 2026, cloud container)

PostgreSQL 16.10, PostGIS 3.4, pgRouting 3.6, h3-pg 4.1, tippecanoe 2.49, on the project's processed files:

| Step | Result | Time |
|---|---|---|
| Archive: fetch 2 OurAirports files, ingest a DWS file twice | 2 snapshots stored, second DWS ingest detected as unchanged, verify: 6 files ok | seconds |
| Admin tree | 1 country, 9 provinces, 44 districts, 213 municipalities, 4,468 wards, 6,946 suburbs | 5 s |
| Parcels (WC053 Beaufort West, EC101 Dr Beyers Naude) | 40,835 parcels, 99.3% placed in a ward, 88.6% in a suburb | 3 s |
| Valuation | 2,212,074 matched roll rows, 146 municipalities, typos like sale date 2014-25-07 nulled | 32 s |
| Power grid | 20,875 edges, pgRouting finds 755 connected pieces | 2 s |
| Climate | 21.2 M monthly values in 40,658 series | about 3 min |
| Roll-ups | e.g. ZA population 61,939,579 from 4,468 wards; WC053 14,893 parcels, median value R130,000 | 1 min |
| Tiles | admin 47.8 MB, grid 8.4 MB, h3 0.5 MB, parcels 17.1 MB (2 municipalities), places 0.3 MB; checked wards absent at z5, suburbs only from z11 | 1.5 min |
| SQLite pack | 1.66 GB: tree 11,681, metrics 54,442, values 2.2 M, graph, 21.2 M climate values, FTS5 search ("beaufort" finds the municipality and its wards) | 1 min |


### National run (27 November 2026, evening)

| Step | Result | Time |
|---|---|---|
| Plots | 8,881,163 unique SG parcels in 256 files; 98.7% placed in a ward, 95.5% in a suburb | 60 min load + 50 min tree build |
| Database | 20 GB on disk with indexes | |
| Roll-ups | plots and median values at every level, e.g. Gauteng 2.42 M plots, median R510,000; national median R240,000 | in the tree build |
| Tiles | admin 96 MB, grid 19 MB, h3 68 MB, places 0.4 MB, plots 0.96 GB in nine province packs at z12-14 (Gauteng 214 MB largest, North West 64 MB smallest; 1.92 GB when cut to z16) | 35 min |
| Attribute pack | 3.2 GB: plots 1.15 GB, search 1 GB, valuations 0.4 GB, packed climate 0.15 GB | 4 min |
| Release | 15 packs, 4.4 GB, served by `gsa-backend`; plot card in 8 ms, FEED trace in under 40 ms | |

Next size win: split the attribute pack's plot, valuation and plot-search rows per province like the tiles,
so a user who installs one province downloads about a ninth of the 2.7 GB plot part.

## 10. Running it on your PC

```
# once
docker run -d --name gsa-db -e POSTGRES_PASSWORD=gsa -p 5432:5432 -v D:\GridOfSA\pg:/var/lib/postgresql/data postgis/postgis:16-3.4
pip install "psycopg[binary]"
# tippecanoe: WSL (sudo apt install tippecanoe) or build from github.com/felt/tippecanoe

# raw archive
python datastore\archive\gsa_archive.py init  D:\GridOfSA\raw
python datastore\archive\gsa_archive.py fetch D:\GridOfSA\raw
python datastore\archive\gsa_archive.py ingest D:\GridOfSA\raw osm C:\path\south-africa-261124.osm.pbf --fetched 2026-11-24T00:00:00Z

# database and exports
set GSA_DSN=host=localhost user=postgres password=gsa dbname=gsa
bash datastore/run_all.sh          # or the same steps one by one, see run_all.sh
```

h3-pg is not in the stock PostGIS image; add it with `apt install postgresql-16-h3` inside the container
or use the `run_all.sh` note. Each script prints its own help with `--help`.

## 11. Serving: release folders, the back-end and the website

A **release folder** is the hand-over point between the data store and everything that shows data:

```
RELEASE/manifest.json                    gsa-manifest/1: packs, versions, sha256, url, changed
RELEASE/packs/installed.json             gsa-installed/1: what gsa-backend's PackStore reads
RELEASE/packs/<id>/<id>-<version>.<ext>  pmtiles, sqlite or json
```

- **Versions** are `<YYYYMMDD>-<sha256 prefix>`. `build_packs.py --previous OLD/manifest.json` keeps the
  version of a pack whose bytes did not change, so the updater downloads only changed packs.
- **Split packs** carry `region` (`parcels-wc` is `WC`), so the app can offer province by province.
- **Desktop:** the updater (roadmap step 11) downloads changed packs beside the old ones, checks sha256,
  rewrites `installed.json` and calls `PackStore::reload()`; the old release stays for rollback.
- **Server:** the same folder on a volume (or R2 behind the same interface) with the same binary.
- **Routes over the attribute pack:** `/v1/areas/{id}` (tree), `/v1/search`, `/v1/parcels/{sg21}` (plot card),
  `/v1/series` (charts), `/v1/graph/{network}/nodes/{id}` and `/v1/trace` (TR, PATH, FEED, FAIL). The grid
  graph is built in memory from the pack's `node`/`edge` tables on first use and cached per pack snapshot.
  The contract is `api/openapi.yaml`.

**The website** (the published deck.gl artifact) cannot read PMTiles from its asset store: the artifact
host refuses binary files, so its data ships as JSON/base64 bundles uploaded as assets (see the map
pipeline notes). Today those bundles come from the website's own scripts, and `tools/webpack` converts
them into `web-*` and `raster-*` packs for the app. The target is one source for both: an
`export/export_web.py` that writes the website's bundle format from the same PostGIS tables, so a fix in
the data store reaches the website and the app in the same release. That exporter is not written yet.
