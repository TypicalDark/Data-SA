-- Grid of SA data store: core schema (PostgreSQL 16 + PostGIS 3.4 + pgRouting + h3-pg + ltree)
-- Apply with:  psql -d gsa -f schema/001_core.sql      (idempotent: safe to re-run)
-- Coordinates are stored in EPSG:4326 (WGS84 lon/lat). Hartebeesthoek94 equals WGS84 within ~1 m.
-- Areas and lengths use geography casts or EPSG:9221 (SA Albers Equal Area, "Lo" independent) where needed.

CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS pgrouting;
CREATE EXTENSION IF NOT EXISTS ltree;
CREATE EXTENSION IF NOT EXISTS pg_trgm;
CREATE EXTENSION IF NOT EXISTS h3;
CREATE EXTENSION IF NOT EXISTS h3_postgis CASCADE;

CREATE SCHEMA IF NOT EXISTS meta;   -- provenance: sources, raw snapshots, files, lineage
CREATE SCHEMA IF NOT EXISTS core;   -- cleaned entities (admin tree, parcels, values, places)
CREATE SCHEMA IF NOT EXISTS net;    -- graphs (power grid, roads, water)
CREATE SCHEMA IF NOT EXISTS ts;     -- time series (climate, dams, elections, rolls over time)
CREATE SCHEMA IF NOT EXISTS agg;    -- roll-ups by tree node and by H3 cell
CREATE SCHEMA IF NOT EXISTS stage;  -- throw-away load tables, one per raw snapshot

------------------------------------------------------------------ provenance
CREATE TABLE IF NOT EXISTS meta.source (
  source_id     text PRIMARY KEY,           -- same id as the raw archive folder, e.g. 'cadastre_dffe_erven'
  title         text NOT NULL,
  publisher     text,
  url           text,
  kind          text,                       -- file | arcgis | api | manual | derived
  licence       text,
  resale_ok     boolean,                    -- may this go into a paid product without a separate licence?
  cadence       text,                       -- how often the publisher updates it
  privacy       text,                       -- what was never requested / is dropped (POPIA rule)
  notes         text
);

CREATE TABLE IF NOT EXISTS meta.snapshot (
  snapshot_id   text PRIMARY KEY,           -- '<source_id>@<YYYYMMDDTHHMMSSZ>' as in the raw archive
  source_id     text NOT NULL REFERENCES meta.source,
  fetched_utc   timestamptz NOT NULL,
  archive_path  text NOT NULL,              -- relative to the archive root
  tree_sha256   text NOT NULL,              -- hash over the sorted file list + file hashes
  n_files       int, bytes bigint,
  same_as       text,                       -- earlier snapshot with identical content (nothing changed upstream)
  status        text                        -- ok | partial | manual
);

CREATE TABLE IF NOT EXISTS meta.snapshot_file (
  snapshot_id   text REFERENCES meta.snapshot ON DELETE CASCADE,
  rel_path      text, sha256 text NOT NULL, bytes bigint,
  PRIMARY KEY (snapshot_id, rel_path)
);

-- Every processed table records which raw snapshots fed it and which script version built it.
CREATE TABLE IF NOT EXISTS meta.lineage (
  id            bigserial PRIMARY KEY,
  target        text NOT NULL,              -- 'core.parcel'
  snapshot_ids  text[] NOT NULL,
  script        text NOT NULL, script_sha256 text,
  rows_written  bigint,
  loaded_utc    timestamptz NOT NULL DEFAULT now(),
  notes         text
);

-- What the tile exporter builds: one row per vector/raster tile layer and zoom band.
CREATE TABLE IF NOT EXISTS meta.tile_layer (
  layer         text PRIMARY KEY,           -- PMTiles layer name
  pack          text NOT NULL,              -- PMTiles file it goes into ('admin', 'parcels', 'grid', ...)
  source_sql    text NOT NULL,              -- query returning (id, props jsonb, geom)
  minzoom       int NOT NULL, maxzoom int NOT NULL,
  tippecanoe    text,                       -- extra tippecanoe flags
  notes         text
);

------------------------------------------------------------------ the admin tree
-- One table holds every level of the hierarchy, so "go one level deeper" is always the same query:
--   level 0 country > 1 province > 2 district > 3 municipality > 4 ward > 5 main place > 6 sub place / suburb
-- Parcels hang under wards (and suburbs) in core.parcel. path is an ltree: 'ZA.WC.DC5.WC053.W10503001'.
-- Boundaries change (2021 vs 2026 wards), so each row belongs to a boundary_set and has a validity range.
CREATE TABLE IF NOT EXISTS core.admin_area (
  area_id       text PRIMARY KEY,           -- 'ZA', 'WC', 'DC5', 'WC053', 'W10503001', 'SP1234567'
  level         smallint NOT NULL CHECK (level BETWEEN 0 AND 6),
  code          text NOT NULL,              -- official code at that level (MDB / Stats SA)
  name          text,
  parent_id     text REFERENCES core.admin_area DEFERRABLE INITIALLY DEFERRED,
  path          ltree,
  boundary_set  text NOT NULL,              -- 'MDB2021', 'MDB2026', 'SAL2011', 'HRSL2026' ...
  valid_from    date, valid_to date,        -- NULL valid_to = current
  area_km2      double precision,
  label_pt      geometry(Point, 4326),      -- point-on-surface for labels
  h3_r7         h3index,                    -- cell of the label point (r7 ~ 5 km2)
  geom          geometry(MultiPolygon, 4326),
  geom_z6       geometry(MultiPolygon, 4326),  -- pre-simplified copies per zoom band (see spec)
  geom_z10      geometry(MultiPolygon, 4326),
  props         jsonb NOT NULL DEFAULT '{}',
  snapshot_ids  text[]
);
CREATE INDEX IF NOT EXISTS admin_area_geom   ON core.admin_area USING gist (geom);
CREATE INDEX IF NOT EXISTS admin_area_path   ON core.admin_area USING gist (path);
CREATE INDEX IF NOT EXISTS admin_area_parent ON core.admin_area (parent_id);
CREATE INDEX IF NOT EXISTS admin_area_level  ON core.admin_area (level, boundary_set);

-- Crosswalk between boundary sets (2021 ward -> 2026 ward, with the share of population moved).
CREATE TABLE IF NOT EXISTS core.area_crosswalk (
  from_id text, to_id text, share real, method text,
  PRIMARY KEY (from_id, to_id)
);

------------------------------------------------------------------ parcels and property values
-- sg21 is itself a tree: [1] registration division letter, [2-4] major region, [5-8] minor region / township,
-- [9-16] parcel (erf / farm) number, [17-21] portion. Generated columns expose it for drill-down.
CREATE TABLE IF NOT EXISTS core.parcel (
  sg21          char(21) PRIMARY KEY,
  kind          text,                       -- erf | farm | farm_portion | holding
  lstatus       text,
  town          text, town_code text, parcel_no text, portion_no text, farm_name text, scheme text,
  area_m2       double precision,
  sg_date       date,
  muni_id       text REFERENCES core.admin_area,
  ward_id       text REFERENCES core.admin_area,
  suburb_id     text REFERENCES core.admin_area,
  h3_r9         h3index,                    -- ~0.1 km2 cell of the centroid
  quadkey_z16   text,                       -- tile path for spatial sorting / sharding
  geom          geometry(MultiPolygon, 4326) NOT NULL,
  src           text,
  snapshot_ids  text[],
  rd_code       text GENERATED ALWAYS AS (substr(sg21, 1, 4))  STORED,
  township_code text GENERATED ALWAYS AS (substr(sg21, 1, 8))  STORED,
  parcel_code   text GENERATED ALWAYS AS (substr(sg21, 1, 16)) STORED
);
CREATE INDEX IF NOT EXISTS parcel_geom ON core.parcel USING gist (geom);
CREATE INDEX IF NOT EXISTS parcel_ward ON core.parcel (ward_id);
CREATE INDEX IF NOT EXISTS parcel_h3   ON core.parcel (h3_r9);
CREATE INDEX IF NOT EXISTS parcel_town ON core.parcel (township_code);

-- One row per roll entry (several per parcel for sectional titles). Never add owner / ID columns here:
-- the loader drops them and the CHECK below refuses obvious attempts.
CREATE TABLE IF NOT EXISTS core.valuation (
  id            bigserial PRIMARY KEY,
  sg21          char(21),                   -- NULL when the roll row could not be matched to a parcel
  muni_code     text NOT NULL,
  roll_period   text, roll text,            -- e.g. '2023-2027', 'general' | 'supplementary'
  township text, erf text, portion text, scheme text,
  address       text,
  category      text, usage text,
  extent_m2     double precision,
  market_value  numeric(14,0),
  sale_price    numeric(14,0), sale_date date,
  match_method  text, area_check text, value_flag text,
  source_url    text,
  snapshot_ids  text[],
  CONSTRAINT valuation_no_personal CHECK (address IS NULL OR address !~* '\m(ID ?NO|IDENTITY NUMBER)\M')
);
CREATE INDEX IF NOT EXISTS valuation_sg21 ON core.valuation (sg21);
CREATE INDEX IF NOT EXISTS valuation_muni ON core.valuation (muni_code, roll_period);

------------------------------------------------------------------ places and search
CREATE TABLE IF NOT EXISTS core.place (
  place_id      text PRIMARY KEY,           -- 'osm:node/123', 'dbe:500123', 'dws:D3R002'
  kind          text NOT NULL,              -- school | clinic | dam | airport | power_station | ...
  name          text,
  area_id       text REFERENCES core.admin_area,   -- deepest admin node it falls in
  h3_r9         h3index,
  geom          geometry(Geometry, 4326) NOT NULL,
  props         jsonb NOT NULL DEFAULT '{}',
  snapshot_ids  text[]
);
CREATE INDEX IF NOT EXISTS place_geom ON core.place USING gist (geom);
CREATE INDEX IF NOT EXISTS place_kind ON core.place (kind);

-- One search table across everything (places, areas, parcels, addresses, substations).
CREATE TABLE IF NOT EXISTS core.search (
  ref           text PRIMARY KEY,           -- 'area:W10503001', 'parcel:C029...', 'place:osm:node/1'
  label         text NOT NULL,
  detail        text,
  rank          real DEFAULT 0,
  pt            geometry(Point, 4326),
  tsv           tsvector GENERATED ALWAYS AS (to_tsvector('simple', coalesce(label,'') || ' ' || coalesce(detail,''))) STORED
);
CREATE INDEX IF NOT EXISTS search_tsv  ON core.search USING gin (tsv);
CREATE INDEX IF NOT EXISTS search_trgm ON core.search USING gin (label gin_trgm_ops);

------------------------------------------------------------------ graphs (pgRouting layout: id, source, target, cost)
CREATE TABLE IF NOT EXISTS net.node (
  network       text NOT NULL,              -- 'power' | 'road' | 'water'
  node_id       bigint NOT NULL,
  ext_id        text,                       -- 'sub:w28088740' (OSM / source id)
  kind          text,                       -- substation | power_station | junction | tower | ...
  name          text, operator text,
  kv            int[],
  capacity_mva  real,                       -- NTCSA GCCA 2025 where known
  area_id       text,
  geom          geometry(Point, 4326),
  props         jsonb NOT NULL DEFAULT '{}',
  PRIMARY KEY (network, node_id)
);
CREATE UNIQUE INDEX IF NOT EXISTS node_ext ON net.node (network, ext_id);
CREATE INDEX IF NOT EXISTS node_geom ON net.node USING gist (geom);

CREATE TABLE IF NOT EXISTS net.edge (
  network       text NOT NULL,
  edge_id       bigint NOT NULL,
  ext_id        text,
  source        bigint NOT NULL, target bigint NOT NULL,
  cost          double precision NOT NULL,  -- km for power, seconds for roads
  reverse_cost  double precision NOT NULL,  -- -1 = one way
  cls           text, kv int[], circuits int,
  inferred      boolean NOT NULL DEFAULT false,   -- gap joins are always flagged, never silent
  reason        text,
  geom          geometry(Geometry, 4326),   -- LineString, or MultiLineString where the source line has gaps
  props         jsonb NOT NULL DEFAULT '{}',
  PRIMARY KEY (network, edge_id)
);
CREATE INDEX IF NOT EXISTS edge_geom ON net.edge USING gist (geom);
CREATE INDEX IF NOT EXISTS edge_st   ON net.edge (network, source, target);

------------------------------------------------------------------ time series (long format)
-- A series is "variable X for entity Y from source Z". Values are (series, time, value).
CREATE TABLE IF NOT EXISTS ts.series (
  series_id     serial PRIMARY KEY,
  entity_type   text NOT NULL,              -- 'area' | 'place' | 'node' | 'h3'
  entity_id     text NOT NULL,              -- 'W10503001', 'dws:D3R002'
  variable      text NOT NULL,              -- 'rain_mm', 't_mean_c', 'dam_pct', 'votes_ANC'
  unit          text,
  step          text NOT NULL,              -- 'day' | 'week' | 'month' | 'year' | 'event'
  source_id     text REFERENCES meta.source,
  UNIQUE (entity_type, entity_id, variable, source_id)
);
CREATE TABLE IF NOT EXISTS ts.value (
  series_id     int NOT NULL REFERENCES ts.series,
  t             date NOT NULL,
  v             double precision,
  flag          text,
  PRIMARY KEY (series_id, t)
) PARTITION BY RANGE (t);
CREATE TABLE IF NOT EXISTS ts.value_pre2000 PARTITION OF ts.value FOR VALUES FROM ('1900-01-01') TO ('2000-01-01');
CREATE TABLE IF NOT EXISTS ts.value_2000s   PARTITION OF ts.value FOR VALUES FROM ('2000-01-01') TO ('2010-01-01');
CREATE TABLE IF NOT EXISTS ts.value_2010s   PARTITION OF ts.value FOR VALUES FROM ('2010-01-01') TO ('2020-01-01');
CREATE TABLE IF NOT EXISTS ts.value_2020s   PARTITION OF ts.value FOR VALUES FROM ('2020-01-01') TO ('2030-01-01');
CREATE INDEX IF NOT EXISTS value_t_brin ON ts.value USING brin (t);

------------------------------------------------------------------ roll-ups
-- Every metric at every tree node: the map shows the parent level's numbers until you zoom into children.
CREATE TABLE IF NOT EXISTS agg.area_metric (
  area_id   text REFERENCES core.admin_area,
  metric    text,                           -- 'pop', 'parcels', 'value_median', 'value_sum', 'rain_mm_2025'
  as_of     date NOT NULL DEFAULT '1900-01-01',
  v         double precision,
  n         bigint,                         -- how many children / records fed it
  source_id text,
  PRIMARY KEY (area_id, metric, as_of)
);
-- Same metrics on the H3 grid (resolutions 4..9), for heatmaps that ignore admin lines.
CREATE TABLE IF NOT EXISTS agg.h3_metric (
  h3        h3index, res smallint, metric text, as_of date NOT NULL DEFAULT '1900-01-01',
  v         double precision, n bigint,
  PRIMARY KEY (h3, metric, as_of)
);
