# Development data pack

Small packs the back-end, app, tests and CI run on, so nothing needs the 30 GB data store.

Built with:

```
python tools/devpack/make_devpack.py <project files>/grid-of-sa/design/ui-research-2026/prototype/data.json
```

| File | Size | Holds |
|---|---|---|
| `packs/admin.pmtiles` | 1.3 MB | province, municipality, label layers, z0-10 |
| `packs/grid.pmtiles` | 1.6 MB | line (kV, inferred), facility (substations, power stations), z0-10 |
| `packs/find.json` | 0.3 MB | the website's Find data (`data/x/find.json`): municipality outlines and figures, NTCSA free capacity, dams, airports, ports, power stations, towns |
| `packs/pack.sqlite` | 0.3 MB | place tree (country, 9 provinces, 213 municipalities), metrics, sources, FTS5 search |

Sources: Stats SA Census 2022, municipal valuation rolls (medians only), IEC LGE 2026, OpenStreetMap
(ODbL), NTCSA GCCA 2025, MDB boundaries (simplified). No owner names or ID numbers.
