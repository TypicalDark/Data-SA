# Grid of SA internal build

Unzip anywhere and run `gsa.exe`. It starts `gsa-backend.exe` from the same folder on a random local
port and opens the map on the small development pack in `devdata/`.

To use your own packs, put them in `%LOCALAPPDATA%\GridOfSA\packs\` (or pass `--data DIR`). The nine
`web-*.pmtiles` website packs go in the same folder; the app finds their layers by itself.
Needs Windows 10 or 11 with a Vulkan driver (every current NVIDIA, AMD and Intel driver has one).

Keys: 1 to 0 topics, / search or command, [ sidebar, N find, F2 add a layer, ? all keys, Esc back.
This build is unsigned, so SmartScreen may warn.
