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
- Pack files must stay inside `packs/`; pack ids are `[a-z0-9][a-z0-9_-]{0,63}`. `installed.json` is
  `gsa-installed/1` (see docs/DATA.md); it carries each pack's `sha256` and, for split packs, `region`,
  and `/v1/packs` repeats both.
- Plot cards, time series and the grid graph (`api_data.cpp`): `/v1/parcels/{sg21}`, `/v1/series`,
  `/v1/graph/{network}/nodes/{id}` and `/v1/trace` (modes connected, path, feed, fail). The graph is
  built in memory from the pack's `node` and `edge` tables on first use (`graph.cpp`, about 20,000 lines
  for the national grid) and cached per pack snapshot, so a pack swap rebuilds it.
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
- **Text and icons.** A second pipeline draws map text and point icons from one distance-field
  atlas (R8, 1024 x 1024): glyphs are rendered on first use with stb_truetype at 24 px and drawn
  at any size with a sharp edge and a halo from the same texture; icons (square, circle, diamond,
  triangle) are distance fields computed from their shapes. Each frame the label engine
  (`text/labels.cpp`, no GPU) takes candidates from the drawn tiles (`map_labels.cpp`), places them
  in priority order on a collision grid, keeps them out of every interface window, fades them in
  and out over 0.2 s, and bends line labels along their line (upright, no sharp turns, one repeat
  per 420 px). Icons always draw and text steps around them, right of the icon first, then left.
- **Layers and topics (website parity).** The website's catalogue: 71 layers in nine groups (Power,
  Water, Farms, Nature, Places, Travel, Votes, Property, Hazards) plus five base toggles, and its ten
  topics on keys 1 to 0 (`layers.cpp`). A topic fills the On the map list; any layer can be added, hidden
  or removed; one choropleth colours the municipalities at a time. `layers::map_style` turns that list
  into what the renderer and labels draw. Layers whose pack is not installed are listed as needing it.
  The website's own layers come from `web-*` packs: the app reads each pack's vector layer names from
  `/v1/packs`, fetches a web pack only while one of its layers is on, and draws areas tinted with an
  edge, lines, and points as small squares in the layer's colour. Clicking one opens a card.
  Image layers (terrain shading, fire, flood, ships, farm use, rain, solar) come from `raster-*` PNG packs:
  the loader decodes them with stb_image, `MapRenderer` uploads each as its own texture outside the
  render pass (a few per frame) and draws textured quads between the fills and the lines at the
  website's opacities. A missing tile shows the matching part of its nearest loaded parent.
  The base map comes from the `base` pack, drawn just above the municipalities: land use as fills
  under the images, roads with casings by class (motorway, major, minor ... tracks, each class from
  the zoom the website shows it), rail, and in the label pass towns by rank, street names along the
  roads from zoom 13.3 and landmarks as coloured dots from zoom 14.3 (names from 15.5). The Roads,
  Names and Landmarks base toggles switch these; the pack is not fetched when all three are off.
  Buildings come from the `buildings` pack: z12 tiles in the website's GSB1 encoding (rectangles as
  centre, size and angle; the rest as polygons). `gsb.cpp` decodes a tile into a vector layer with
  each footprint's height (measured, or estimated as the website does), and the mesh draws them over
  the roads from zoom 13.3. The pack is fetched only with Buildings on and from zoom 13
  (`PackZoom::show_from`). Base toggles are saved in the settings under `base`.
  Plots come from the `parcels` pack from zoom 12.5 while Property plots is on: fills in the website's
  value bands where a roll matched, thin outlines from 13.5, and a plot card (value, roll, category,
  use, last sale, area, SG code) on click.
  3D: `geo::Camera::pitch` (0 to 60 degrees) tilts the ground around the screen centre as seen by a
  camera 1.5 screen heights above it. Every vertex is placed flat as before and then tilted on the GPU
  with the same formula as `Camera::tilt` (map and image shaders), with a height in metres for
  extruded buildings; labels, icons and overlays go through `Camera::to_screen`, and clicks through
  its inverse `to_world`. Tilted, buildings stand up to their heights: walls shaded by the way they
  face and roofs, drawn far to near since there is no depth buffer. The 3D button tilts to 50
  degrees and back; Ctrl+drag tilts by hand; `--pitch` sets it for screenshots.
- **UI (matches the live website, v57).** Dear ImGui in the same render pass after the map, with the
  website's tokens (`ui_theme.hpp`) and drawn line icons (`icons.cpp`): 60 px icon rail, 380 px docked
  sidebar with the search box and command line, the card or panel, and the On the map dock (group tabs,
  eye and remove buttons, legend under the colour layer, base toggles); layer catalogue (F2); breadcrumb
  pill; 3D, zoom and home buttons; status bar. Cards carry a type tag, title, data line and a function
  strip (DES, HERE, NEAR, TR, FAIL, FEED, PATH, REACH, MEAS, WATCH, GO, COPY, PNG; unbuilt ones are shown
  greyed). Panels: Find, Rank and List of what is in view, Areas, saved Views, Watchlist, Keys, Measure.
  Right-click opens a spot card.
- **Find and Rank** (`find.cpp`, the website's patch_ui2). The `find` JSON pack (`/v1/data/find`) holds
  every municipality's outline and figures and the point lists (free grid capacity, dams, airports,
  ports, power stations, towns). Twelve conditions (density, people, median value, party, and "within
  N km of" a dam, town, substation with free MW, power station, airport or port, measured from the
  boundary) are tested per municipality; the five presets, editable numbers, why-it-matched text and
  CSV copy match the website. Matching municipalities get an accent outline and the rest a dark shade,
  drawn with the admin lines so it covers every pack's fills and images. Rank lists seven things in the
  current view (substations, power stations, free capacity, dams lowest first, municipalities by value
  or people, airports). The spot card adds the website's summary (nearest town, dam, free capacity,
  airport, port, and which Find conditions the spot fails). Saved views keep the conditions.
- **Filters** (`layers::Filters`, F3). The website's filter panel: power lines and substations by
  voltage band, power stations by fuel, 2026 ward winners by party, turnout band and province. What
  fails is dimmed in the mesh and icon passes, or left out with "hide". Filters are saved settings.
- **Measure** (`measure.cpp`, the website's Measure v2). Path with leg lengths and bearings, area with
  perimeter, circle with radius, area and circumference; click the last point (or Enter) to finish
  and the first to close an area; a result pill; copy as text or GeoJSON. Routes along roads and the
  elevation profile come with Reach. The watchlist shows which watched facilities are in view, fits
  them all, and exports CSV. Each window's rectangle goes to the label
  engine as a blocker.
- **Command line** (`commands.cpp`). An UPPERCASE first word or a leading `:` is a command (`GO`, `ADD`,
  `RM`, `HIDE`, `FIND`, `RANK`, `SAVE`, `VIEW` ...); `name CODE` finds the subject and runs the function
  on it (`apollo DES`); anything else searches.
- **Settings** (`settings.cpp`). Saved views, watchlist, sidebar and topic in
  `%APPDATA%\GridOfSA\settings.json` (or `~/.config/GridOfSA`), written atomically. Scripted runs never
  touch it.
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
