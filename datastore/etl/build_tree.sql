-- Grid of SA: derive the tree, spatial keys and roll-ups after the loaders have run.
--   psql -d gsa -v ON_ERROR_STOP=1 -f etl/build_tree.sql
-- Safe to re-run. Order: paths -> geometry helpers -> parcel placement -> roll-ups -> H3 -> search.

------------------------------------------------------------------ helpers
-- Bing-style quadkey of a point at zoom z (Web Mercator). Same keys as the z/x/y tile pyramid,
-- so a prefix match finds everything inside a tile: quadkey LIKE '3001%' = inside tile '3001'.
CREATE OR REPLACE FUNCTION core.quadkey(p geometry, z int) RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
  lon float8 := ST_X(p); lat float8 := greatest(least(ST_Y(p), 85.05112878), -85.05112878);
  n float8 := 2 ^ z; x int; y int; q text := ''; i int; d int;
BEGIN
  x := floor((lon + 180) / 360 * n)::int;
  y := floor((1 - ln(tan(radians(lat)) + 1 / cos(radians(lat))) / pi()) / 2 * n)::int;
  FOR i IN REVERSE z..1 LOOP
    d := 0;
    IF (x >> (i - 1)) & 1 = 1 THEN d := d + 1; END IF;
    IF (y >> (i - 1)) & 1 = 1 THEN d := d + 2; END IF;
    q := q || d::text;
  END LOOP;
  RETURN q;
END $$;

------------------------------------------------------------------ 1. tree paths (ltree), areas, labels, simplified copies
WITH RECURSIVE t AS (
  SELECT area_id, text2ltree('ZA') AS path FROM core.admin_area WHERE level = 0
  UNION ALL
  SELECT c.area_id, t.path || text2ltree(regexp_replace(c.area_id, '[^A-Za-z0-9_]', '_', 'g'))
  FROM core.admin_area c JOIN t ON c.parent_id = t.area_id
)
UPDATE core.admin_area a SET path = t.path FROM t WHERE a.area_id = t.area_id;

UPDATE core.admin_area SET
  area_km2 = ST_Area(geom::geography) / 1e6,
  label_pt = ST_PointOnSurface(geom),
  geom_z6  = ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_SimplifyPreserveTopology(geom, 0.01)), 3)),     -- ~1 km
  geom_z10 = ST_Multi(ST_CollectionExtract(ST_MakeValid(ST_SimplifyPreserveTopology(geom, 0.0005)), 3))    -- ~50 m
WHERE geom IS NOT NULL;
UPDATE core.admin_area SET h3_r7 = h3_lat_lng_to_cell(label_pt, 7) WHERE label_pt IS NOT NULL;

------------------------------------------------------------------ 2. place each parcel in the tree (centroid inside ward / suburb)
UPDATE core.parcel p SET h3_r9 = h3_lat_lng_to_cell(ST_PointOnSurface(p.geom), 9),
                         quadkey_z16 = core.quadkey(ST_PointOnSurface(p.geom), 16)
WHERE h3_r9 IS NULL OR quadkey_z16 IS NULL;

UPDATE core.parcel p SET ward_id = w.area_id, muni_id = coalesce(p.muni_id, w.parent_id)
FROM core.admin_area w
WHERE p.ward_id IS NULL AND w.level = 4 AND w.valid_to IS NULL AND ST_Intersects(w.geom, ST_PointOnSurface(p.geom));

UPDATE core.parcel p SET suburb_id = s.area_id
FROM core.admin_area s
WHERE p.suburb_id IS NULL AND s.level = 6 AND ST_Intersects(s.geom, ST_PointOnSurface(p.geom));

UPDATE core.place pl SET area_id = coalesce(
  (SELECT area_id FROM core.admin_area s WHERE s.level = 6 AND ST_Intersects(s.geom, pl.geom) LIMIT 1),
  (SELECT area_id FROM core.admin_area w WHERE w.level = 4 AND ST_Intersects(w.geom, pl.geom) LIMIT 1)),
  h3_r9 = h3_lat_lng_to_cell(ST_PointOnSurface(pl.geom), 9);

------------------------------------------------------------------ 3. roll-ups up the tree
-- 3a. additive ward metrics (population, households, voters ...) summed to every ancestor.
DELETE FROM agg.area_metric m USING core.admin_area a
 WHERE m.area_id = a.area_id AND a.level < 4 AND m.source_id = 'statssa_ward_estimates_2022';
INSERT INTO agg.area_metric (area_id, metric, as_of, v, n, source_id)
SELECT anc.area_id, m.metric, m.as_of, sum(m.v), count(*), m.source_id
FROM agg.area_metric m
JOIN core.admin_area w   ON w.area_id = m.area_id AND w.level = 4
JOIN core.admin_area anc ON anc.level < 4 AND anc.path @> w.path
WHERE m.source_id = 'statssa_ward_estimates_2022'
GROUP BY anc.area_id, m.metric, m.as_of, m.source_id;

-- 3b. suburb population from the suburb file itself.
INSERT INTO agg.area_metric (area_id, metric, as_of, v, n, source_id)
SELECT area_id, 'pop_est', '2022-10-01', (props->>'pop_est')::float, 1, 'hrsl_suburb_split'
FROM core.admin_area WHERE level = 6 AND props ? 'pop_est'
ON CONFLICT (area_id, metric, as_of) DO UPDATE SET v = EXCLUDED.v;

-- 3c. parcels and values per node, at every level (ward, suburb and all ancestors).
DELETE FROM agg.area_metric WHERE source_id = 'derived:parcels';
WITH pv AS (
  SELECT p.sg21, p.ward_id, p.suburb_id, sum(v.market_value) AS value, max(v.sale_price) AS sale
  FROM core.parcel p LEFT JOIN core.valuation v ON v.sg21 = p.sg21 AND coalesce(v.value_flag,'') <> 'zero'
  GROUP BY p.sg21, p.ward_id, p.suburb_id
), node AS (            -- every parcel counted once under each node above it
  SELECT anc.area_id, pv.value FROM pv JOIN core.admin_area w ON w.area_id = pv.ward_id
                                       JOIN core.admin_area anc ON anc.path @> w.path
  UNION ALL
  SELECT pv.suburb_id, pv.value FROM pv WHERE pv.suburb_id IS NOT NULL
)
INSERT INTO agg.area_metric (area_id, metric, as_of, v, n, source_id)
SELECT area_id, x.metric, '1900-01-01', x.v, x.n, 'derived:parcels' FROM (
  SELECT area_id, 'parcels' metric, count(*)::float8 v, count(*) n FROM node GROUP BY area_id
  UNION ALL
  SELECT area_id, 'value_median', percentile_cont(0.5) WITHIN GROUP (ORDER BY value), count(value) FROM node WHERE value > 0 GROUP BY area_id
  UNION ALL
  SELECT area_id, 'value_sum', sum(value), count(value) FROM node WHERE value > 0 GROUP BY area_id
) x;

-- 3d. latest full climate year per node (mean of ward values for ancestors).
DELETE FROM agg.area_metric WHERE source_id = 'derived:climate';
WITH yr AS (SELECT max(extract(year FROM t))::int - 1 AS y FROM ts.value v JOIN ts.series s USING (series_id) WHERE s.variable = 'rain_mm_merra2'),
ward AS (
  SELECT s.entity_id AS area_id, s.variable, CASE WHEN s.variable LIKE 'rain%' THEN sum(v.v) ELSE avg(v.v) END AS val
  FROM ts.series s JOIN ts.value v USING (series_id), yr
  WHERE s.source_id = 'nasa_power_monthly' AND s.variable IN ('rain_mm_merra2', 't_mean_c')
    AND v.t >= make_date(yr.y, 1, 1) AND v.t < make_date(yr.y + 1, 1, 1)
  GROUP BY s.entity_id, s.variable)
INSERT INTO agg.area_metric (area_id, metric, as_of, v, n, source_id)
SELECT anc.area_id, ward.variable || '_year', make_date((SELECT y FROM yr), 12, 31), avg(ward.val), count(*), 'derived:climate'
FROM ward JOIN core.admin_area w ON w.area_id = ward.area_id JOIN core.admin_area anc ON anc.path @> w.path
GROUP BY anc.area_id, ward.variable;

------------------------------------------------------------------ 4. H3 grid roll-ups (r9 -> r8 ... r5), independent of admin lines
TRUNCATE agg.h3_metric;
WITH pv AS (
  SELECT p.h3_r9, sum(v.market_value) AS value FROM core.parcel p
  LEFT JOIN core.valuation v ON v.sg21 = p.sg21 AND coalesce(v.value_flag,'') <> 'zero'
  GROUP BY p.sg21, p.h3_r9
), r AS (SELECT generate_series(5, 9) AS res)
INSERT INTO agg.h3_metric (h3, res, metric, v, n)
SELECT h3_cell_to_parent(pv.h3_r9, r.res), r.res, x.metric,
       CASE x.metric WHEN 'parcels' THEN count(*) ELSE percentile_cont(0.5) WITHIN GROUP (ORDER BY pv.value) END,
       count(pv.value)
FROM pv CROSS JOIN r CROSS JOIN (VALUES ('parcels'), ('value_median')) x(metric)
WHERE pv.h3_r9 IS NOT NULL AND (x.metric = 'parcels' OR pv.value > 0)
GROUP BY 1, 2, 3;

------------------------------------------------------------------ 5. one search table
TRUNCATE core.search;
INSERT INTO core.search (ref, label, detail, rank, pt)
SELECT 'area:' || a.area_id, coalesce(a.name, a.code),
       concat_ws(', ', CASE a.level WHEN 1 THEN 'Province' WHEN 2 THEN 'District' WHEN 3 THEN 'Municipality' WHEN 4 THEN 'Ward' WHEN 6 THEN 'Suburb' END,
                 (SELECT p.name FROM core.admin_area p WHERE p.area_id = a.parent_id)),
       10 - a.level, a.label_pt
FROM core.admin_area a;
INSERT INTO core.search (ref, label, detail, rank, pt)
SELECT 'place:' || place_id, name, kind, 3, ST_PointOnSurface(geom) FROM core.place WHERE name IS NOT NULL;
INSERT INTO core.search (ref, label, detail, rank, pt)
SELECT 'node:power:' || node_id, name, concat_ws(' ', kind, array_to_string(kv, '/') || ' kV', operator), 4, geom
FROM net.node WHERE network = 'power' AND name IS NOT NULL;
INSERT INTO core.search (ref, label, detail, rank, pt)
SELECT 'parcel:' || sg21, coalesce(town, farm_name, '') || ' ' || coalesce(parcel_no, '') || coalesce('/' || nullif(portion_no, ''), ''),
       kind, 1, ST_PointOnSurface(geom)   -- the SG code is the ref; look it up with /v1/parcels/{sg21}
FROM core.parcel;

ANALYZE;
