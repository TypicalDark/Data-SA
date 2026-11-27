# Architecture

Version 0.1, 27 November 2026. Replaces the Tauri + deck.gl stack of the first plan: the app and
back-end are C++, and the map is drawn by our own Vulkan renderer.

## Parts

| Part | Binary | Does | Later on a server |
|---|---|---|---|
| Front-end | `gsa` | window, input, Vulkan map renderer, Dear ImGui interface, starts the back-end on desktop | unchanged; points at `https://api.<domain>` |
| Back-end | `gsa-backend` | serves `/v1`: tiles from PMTiles, search, place tree, packs, versions; later grid trace, live feeds, pack updater | same code, Linux container |
| Data packs | files | PMTiles (shapes, one per layer group) and one SQLite pack (numbers, search, tree, graph, time series) | object storage (R2) read by range |

The two binaries talk only HTTP. The front-end keeps view state (camera, topic, panel stack); anything
that should sync later (saved views, watchlists) will go through the API.

## Back-end

- C++20, cpp-httplib (thread pool), SQLite (read-only pool, FTS5), zlib.
- `PackStore` loads `packs/installed.json` (written by the updater) or, in development, every
  `*.pmtiles` plus `pack.sqlite` in `packs/`. It hands out immutable snapshots: a request keeps its
  snapshot, so a pack swap never pulls a file out from under it. `reload()` swaps atomically and keeps
  the old set when the new one fails to load.
- Pack files must stay inside `packs/`; pack ids are `[a-z0-9][a-z0-9_-]{0,63}`.
- SQLite pack schema is versioned (`pack_info.format = gsa-pack/1`). The back-end lists the schemas it
  reads in `/v1/version`; it refuses a pack it cannot read and says to update the app first.
- Auth: `Authorization: Bearer <token>` on every route but `/v1/health`. The token comes from the
  `GSA_TOKEN` environment variable, never the command line. `--no-auth` is refused off loopback.
- Ready signal: one JSON line on stdout, `{"event":"ready","port":N,"api":"1"}`. With
  `--exit-with-stdin` it stops when its parent closes the pipe.

## Front-end

- GLFW window, Vulkan 1.2, 4x MSAA, FIFO present. Software rendering (lavapipe) works for CI.
- **Renderer.** One pipeline and one vertex format for fills, lines and symbols:
  position in the tile (0..1), screen-pixel extrusion, colour, and a fade-in zoom. Per-tile push
  constants place each tile relative to the camera in double precision, so there is no float jitter at
  plot zoom. Each tile clips to its own square in the fragment shader, so tile buffers never draw twice.
  Draw order: all fills, then all lines, then all symbols.
- **Tiles.** Four loader threads fetch through the API, inflate and decode MVT (protozero). The main
  thread tessellates (earcut for polygons, extruded quads for lines) and uploads. While a tile loads,
  its nearest loaded ancestor stands in. Least recently drawn tiles are evicted past 600.
- **Topics.** The seven topics of the v7 design (Overview, People, Property, Water, Energy and grid,
  Elections, Transport). Changing topic re-colours cached tiles without refetching. The grid draws only
  in its own topic, never by default.
- **UI.** Dear ImGui drawn in the same render pass: topic switcher, search with type tags, one left
  panel with a back stack (topic home, area card with breadcrumb, facts, function strip, children list),
  key bottom right, status bar. Labels are drawn with greedy declutter and never under the panel.
- **Shell role.** With no `--api`, `gsa` starts `gsa-backend` from its own folder on a random port with
  a random 256-bit token, waits for the ready line, and stops it on exit. On Windows a job object kills
  the back-end even if the app crashes.

## Updates

| Channel | What | How | Restart |
|---|---|---|---|
| Front-end | `gsa` | signed download; the shell swaps the binary and restarts only the front-end, the back-end keeps running | front-end only, about a second |
| Back-end | `gsa-backend` | signed download, swapped when the front-end restarts it | one click |
| Data packs | PMTiles / SQLite | signed manifest, per-province deltas, download beside the old file, verify, atomic swap through `PackStore::reload()`, keep the previous version for rollback | none |

`installed.json` is the hinge: the updater writes it, `PackStore` reads it.

## Moving to a server

Build `gsa-backend` for Linux, run it in a container with the packs on a volume (or later an object
store behind the same `PackStore` interface), put TLS in front, and start `gsa --api host:443`. The
token becomes a user session. Paid tiers are a back-end check, not a front-end flag.

## Trade-offs of the native stack (decided 27 Nov 2026)

- Effort: maps, text and UI that a browser gave for free are our code now (see ROADMAP).
- Dear ImGui suits the dense terminal-style design but has weak screen-reader support and simple text
  shaping (fine for English place names).
- macOS would need MoltenVK; Windows and Linux run Vulkan natively.
