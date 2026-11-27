# Roadmap

First signed release in about **22 to 28 weeks** (own Vulkan renderer, decided 27 Nov 2026). The first
release is personal and carries all data, plot values included.

| # | Step | Weeks | Status |
|---|---|---|---|
| 1 | Repository, CMake, CI on Linux and Windows, API contract | 1 | done (PR 1) |
| 2 | Back-end: PMTiles, SQLite pack, `/v1` tiles, search, place tree, packs, versions, auth | 2 | first cut done (PR 1) |
| 3 | Renderer core: Vulkan device, swapchain, MSAA, vector tiles, fills, lines, symbols, pan and zoom | 3 | first cut done (PR 1) |
| 4 | UI shell: topics, search, panel and back stack, key, status bar | 2 | first cut done (PR 1) |
| 5 | Text and labels: SDF glyph atlas in the map pass, collision, curved road labels, icons atlas | 3 | first cut done (PR 1): atlas, collision, fades, curved line labels, icons; road labels wait for road tiles |
| 6 | Map quality: line joins and caps, dashes, hatching for no data, hover and selection outlines, raster tiles (hillshade, solar) | 2 | |
| 7 | Real packs from the data store: admin with wards and suburbs, grid, parcels per province, places, h3; `export_tiles.py` and `export_pack.py` feeding `installed.json` | 2 | |
| 8 | Plot level: parcel tiles z13-16, plot card from the SQLite pack, search by erf | 1.5 | |
| 9 | Functions of the v7 model: DES, CHK, NEAR, RANK, CMP, LIST, CONN, FOLLOW, MEAS, command line | 3 | |
| 10 | 3D: terrain mesh from raster PMTiles, camera pitch and orbit, planes at altitude | 3 | |
| 11 | Update system: signed manifest, per-province deltas, atomic swap, rollback, beta and stable, Update data screen, app self-update | 2 | |
| 12 | Live feeds in the back-end (load shedding, flights, ships), server-sent events | 1 | |
| 13 | Signing (Azure Trusted Signing), installer, release pipeline to R2 | 1 | |
| 14 | Server dry run: Linux container on R2 packs | 0.5 | |

A usable internal Windows build exists from PR 1 on (CI artifact `GridOfSA-internal-windows-x64`).
