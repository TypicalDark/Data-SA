#!/usr/bin/env bash
# Grid of SA data store: build everything from the project's processed files.
#   GSA_DSN="dbname=gsa" DATA=/path/to/grid-of-sa OUT=/path/to/out bash run_all.sh
# Needs: PostgreSQL 16 + postgis + pgrouting + h3 (postgresql-16-h3) + ltree, python3 with psycopg, tippecanoe (with tile-join).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${DATA:-$HERE/..}"
OUT="${OUT:-$HERE/out}"
RAW="${RAW:-}"                      # raw archive root, if you have one (adds sources and snapshots)
export GSA_DSN="${GSA_DSN:-dbname=gsa}"
L="python3 $HERE/etl/gsa_load.py"
mkdir -p "$OUT"

psql "$GSA_DSN" -q -v ON_ERROR_STOP=1 -f "$HERE/schema/001_core.sql"
if [ -n "$RAW" ]; then
  $L sources "$RAW/_catalog/sources.json" --snapshots "$RAW/_catalog/snapshots.jsonl"
fi
$L admin      "$DATA/climate/sources/munis.geojson" "$DATA/climate/sources/wards.geojson" --set MDB2021
$L suburbs    "$DATA/population/suburbs_population.geojson.gz"
$L population "$DATA/population/wards_population.csv"
$L parcels    "$DATA"/parcels/muni/*.geojsonl.gz "$DATA"/parcels/kzn/*.geojsonl.gz
$L valuation  "$DATA/parcels/parcel_values.csv.gz"
$L grid       "$DATA/network/v2/grid_nodes_v2.geojson" "$DATA/network/v2/grid_edges_v2.geojson"
$L climate    "$DATA/climate/climate_wards_monthly.csv.gz"
$L dams       "$DATA/dams/dam_levels_current.csv"
psql "$GSA_DSN" -q -v ON_ERROR_STOP=1 -f "$HERE/etl/build_tree.sql"
python3 "$HERE/export/export_tiles.py" "$OUT/tiles"
python3 "$HERE/export/export_pack.py"  "$OUT/pack.sqlite"
# A release folder the back-end serves as is (gsa-backend --data "$OUT/release").
# EXTRA_PACKS: website packs and JSON data packs to include, e.g. "web-*.pmtiles raster-*.pmtiles find.json".
PREV=()
[ -f "$OUT/release/manifest.json" ] && PREV=(--previous "$OUT/release/manifest.json")
python3 "$HERE/export/build_packs.py" "$OUT/release.new" --tiles "$OUT/tiles" --pack "$OUT/pack.sqlite" \
  ${EXTRA_PACKS:+--add $EXTRA_PACKS} "${PREV[@]}"
rm -rf "$OUT/release.old"; [ -d "$OUT/release" ] && mv "$OUT/release" "$OUT/release.old"
mv "$OUT/release.new" "$OUT/release"
echo "done: $OUT/release"
