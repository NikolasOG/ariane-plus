# Ariane — SA-MP edition (fork notes)

Branch `samp-edition`. Adds an **SA-MP layer** on top of Ariane so you can map for an
SA-MP / open.mp server without ever touching your game's IPL files.

## What changed
- `tools/euryopa/samp.cpp` (new) — the whole feature: layer, import, export, live sync.
- `euryopa.h` — declarations.
- `objectinst.cpp` — in SA-MP mode, new/pasted/prefab objects go into the SA-MP layer
  (`GetOrCreateCurrentPlacementIplFile`).
- `gui.cpp` — **SA-MP** menu; Save / Hot Reload blocked while SA-MP mode protects game
  files; dropping `.pwn/.inc/.txt/.map` on the window imports it.
- `fileloader.cpp` — the SA-MP layer is never written as a game IPL, streamed (binary) IPL
  saves are skipped in SA-MP mode; SA-MP IDE/IMG loading hook.
- `main.cpp`, `telemetry.cpp` — no upstream auto-update, telemetry opt-in, `ARIANE_GAME_DIR`.
- Built on [Dryxio/ariane](https://github.com/Dryxio/ariane) `master` with the librw it pins
  ([Southland-FR/librw](https://github.com/Southland-FR/librw), `ariane` branch, commit
  `4456bb0`, see `.github/workflows/build-euryopa.yml`). No librw changes needed.

## Using it
**SA-MP** menu in the top bar:
- **SA-MP mode (protect game files)** — on by default.
- **Import SA-MP / MTA map…** — `CreateObject`, `CreateDynamicObject(Ex)`, `CreatePlayerObject`,
  `Map_CreateObject`, `RemoveBuildingForPlayer`, `Map_RemoveBuilding` from Pawn files, or
  MTA `<object>` / `<removeWorldObject>` from `.map`. You can also drag a file onto the window.
- Place objects with the normal object browser / spawner / prefabs — they join the layer.
- **Delete a stock building** → exported as `RemoveBuildingForPlayer` (its LOD too).
  **Move a stock building** → exported as remove + a copy at the new spot.
- **Export Pawn…** (Ctrl+Shift+P) — streamer plugin (`CreateDynamicObject`), native open.mp /
  SA-MP (`CreateObject`; both servers share these natives) or your own wrapper functions
  (`<prefix>CreateObject` / `<prefix>RemoveBuilding` / `<prefix>SetObjectMaterial`, prefix
  configurable). Material overrides are exported with the matching material calls.
- **Copy selection as Pawn** (Ctrl+Shift+C) — selected objects as code in the export format.
- **Reload from server** — rebuild the layer from what the server runs now. Ariane does this
  automatically on start, and asks before a sync would wipe a non-empty server map.
- **Server folder…** then **Sync to server** (Ctrl+Shift+S) — writes
  `scriptfiles/ariane_live.txt` and, optionally, `<server>/<include folder>/<name>.inc` in the
  export format. The bundled filterscript [`tools/samp/ariane_live.pwn`](tools/samp/ariane_live.pwn)
  polls the live file and rebuilds the map within ~3 s on any open.mp / SA-MP server
  (streamer plugin required).
- All SA-MP shortcuts are listed under **SA-MP** in Window > Keyboard Shortcuts.

## Model Viewer
Object Browser > **Preview** (or double-click the small preview, or press **P** with an object
selected; also Window > Model Viewer and Object Info > Preview model) opens an orbitable close-up of
the model:
- left-drag orbits, right/middle-drag pans, the wheel zooms, double-click or F frames the model,
  1 / 3 / 7 give front / side / top views, Left / Right step through model ids;
- textured or untextured ("clay") shading, wireframe, and overlays for collision (boxes, spheres,
  mesh), the drawn bounding box, a 1 m ground grid, the pivot axes and a 1.8 m person for scale;
- background, light and field-of-view controls; LOD / damage variants; the SA-MP material
  overrides of the selected object;
- an inspector: model facts (TXD, defining IDE, draw distance, LOD, flags, drawn vs collision size -
  it warns when the mesh floats above its collision - triangle / vertex / material counts), material
  slots with their textures (the SA-MP material index), every texture in the TXD (click to enlarge)
  and collision stats per surface;
- Place, Go to nearest instance, Save PNG (to the Ariane data folder's `screenshots`), Copy ID / name.

Agents can render the same view to a PNG (`arianectl model-view`, MCP `model_view`) or open the
window for you (`viewer-open` / `viewer_open`).

## Mapping with an AI agent
Point an agent at [`SKILLS.md`](SKILLS.md) first: preflight checks, the build loop and
recipes for walls, settlements, ruins, retexturing and custom models. The full reference
is [`tools/agent/MAPPING_TOOLKIT.md`](tools/agent/MAPPING_TOOLKIT.md); workspace setup is in
[`tools/agent/scripts/README.md`](tools/agent/scripts/README.md).

## Rotations
SA-MP uses GTA's `CMatrix::SetRotate(x, y, z)` = `Rz·Rx·Ry`. Ariane stores quaternions;
the conversion self-calibrates against `ObjectInst::UpdateMatrix` on first use (see the log).

## SA-MP models
SA-MP's own objects (IDs 18631–19999: fires, walls, signs, the campfire...) are loaded
from `SAMP.ide` / `SAMP.img` / `SAMPCOL.img` / `custom.img` - first from the game's
`SAMP\` folder, otherwise from the open.mp launcher's copy in
`%LOCALAPPDATA%\mp.open.launcher\samp\shared\SAMP`. They show up in the object
browser and can be placed like stock models. The SA-MP menu shows whether they loaded.

## Other fork changes
- **No auto-update** - upstream's updater would replace this fork with a stock build.
- **Telemetry off** (upstream no longer has a toggle in the UI, so it simply stays off).
- **`ARIANE_GAME_DIR`** - run the editor from anywhere against a game folder
  (`run_ariane_samp.bat` sets it). Settings stay next to `ariane.exe`.

## Known limits
- Particle objects (fire/smoke, 18668+) are effects in-game, so Ariane may show little or nothing for them -
  they're still kept and exported.
- MTA object `scale` has no SA-MP equivalent; scaled objects import at normal size.
- Restoring a removed building on a live server only shows after players reconnect.
