# Ariane Plus — SA-MP edition notes

Ariane Plus (branch `samp-edition`) is an unofficial fork of [Ariane](https://github.com/Dryxio/ariane) by
Dryxio. It adds an **SA-MP layer** on top of Ariane so you can map for an SA-MP / open.mp server without
ever touching your game's IPL files. This file is the detailed list of what changed from Ariane.

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
  Delete asks first when the selection holds stock buildings (turn that off with "Don't ask
  again", back on with **Ask before removing world buildings**); Ctrl+Z brings them back.
- **Allow removing world buildings** (Ctrl+Shift+W) — while one IPL is open as the document (for
  example an agent's map), other buildings are locked. With this on you can still select game
  buildings and remove them as above; they can't be moved or edited that way, and no IPL file
  changes. Off by default.
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
- **Show in game** (Ctrl+Shift+T) — syncs, then moves players who are in game to the editor's view
  (the camera target, standing on the ground). **Show in game moves** picks who:
  - **Admins logged in with RCON** (default) — anyone who typed `/rcon login <password>`; works on any
    server with the bundled filterscript.
  - **Players the server script picks** — your gamemode or a filterscript decides, with its own admin
    system: `forward Ariane_CanFollow(playerid); public Ariane_CanFollow(playerid) return <your check>;`
  - **One player** — by name or id.
  - **Everyone on the server** — for a local test server.

  Ariane writes `scriptfiles/ariane_goto.txt`
  (`goto <seq> <x> <y> <z> <angle> <interior> <world> <rcon|script|player|all> [player]`); the bundled
  filterscript acts on it when `seq` changes. A gamemode can read the same file instead.
- **Removed buildings…** — every game building the map removes, nearest first, with a filter. Go to
  flies the camera there; Restore puts the building (and its LOD) back. Imported removals that match
  no loaded building are listed too and can be dropped.
- **Checks…** — finds common mistakes in the SA-MP layer, each with Go to and Select:
  - floating objects (nothing within 35 cm below);
  - objects on roads (tarmac) or dirt tracks; gates and barriers are allowed there;
  - explosive props (1225, 1370);
  - duplicates (same model at the same spot);
  - more than 1000 objects with the native `CreateObject` export;
  - more objects within 300 m than the streamer shows at once by default (500);
  - models Ariane can't draw.
- **Object Info > SA-MP materials** — retexture or recolour a placed object slot by slot: Texture…
  shows every texture in a TXD as thumbnails (the model's own TXD is filled in), the colour box sets the
  material colour, Clear removes the override. Copy materials / Paste to selection copies a look across
  objects. Same data as the agent's `material` commands; it syncs and exports like any edit.
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

## Server side: ariane.inc
[`tools/samp/ariane.inc`](tools/samp/ariane.inc) is everything a server needs to work with Ariane, as one
include for a gamemode or filterscript (open.mp or SA-MP, streamer plugin). Copy it to your compiler's
include folder and `#include <ariane>`. It starts itself (or `#define ARIANE_NO_HOOKS` and call
`Ariane_Start()` yourself). The full reference is at the top of the file.
- **Live map**: follows Sync to server (objects, materials, removed buildings). `Ariane_Reload`,
  `Ariane_EnableLive`, `Ariane_SetLiveWorld`, `Ariane_SetDrawDistance`, `Ariane_GetObjectCount` /
  `GetObject` / `IsMapObject` / `GetObjectIndex`, `Ariane_GetRemoval(Count)`, `Ariane_ApplyRemovals`.
- **Show in game** (editor → server) and **Focus editor** (server → editor): `Ariane_FocusEditor(playerid)`
  / `Ariane_FocusEditorAt(x, y, z)` fly Ariane's camera there (SA-MP > Follow the server's focus requests).
  `Ariane_SendToEditorView`, `Ariane_GetEditorView`, `Ariane_TeleportPlayer` (streamer-aware).
- **Freecam**: `Ariane_FreecamStart(playerid)` / `Ariane_FreecamStop(playerid, landHere)` — fly with W/S/A/D
  along the view, jump up, crouch down, sprint faster, walk slower. Objects stream around the camera;
  leaving puts the player back (or lands them at the camera) and, on open.mp, restores their spawn point.
- **Callbacks** (all optional): `OnArianeMapUpdate(objects, materials, removals)`,
  `OnArianeObjectCreate(objectid, modelid, index)` (give live objects behaviour),
  `OnArianeShowInGame(playerid, x, y, z)` (return 0 to keep a player where they are),
  `OnArianeFreecam(playerid, enabled)`, and `Ariane_CanFollow(playerid)` — your own permission check for
  "Players the server script picks".
- **Static maps**: export with SA-MP > Export Pawn…, format *wrapper*, prefix `Ariane_`, and the file
  compiles against the include (`Ariane_CreateObject`, `Ariane_RemoveBuilding`, `Ariane_SetObjectMaterial(Text)`).

[`tools/samp/ariane_live.pwn`](tools/samp/ariane_live.pwn) is a ready-made filterscript built on it, with
`/arianefreecam`, `/arianefocus` and `/arianeview` for RCON admins.

## Groups
Select some objects and press **Ctrl+Shift+G** to group them under a name (Window > Groups lists them).
- Clicking any member selects the whole group (switch that off in the Groups panel to pick one member);
  the gizmo moves it as one, and rotation turns the whole group around its centre.
- The panel: Select, Go to, Rename, Duplicate (copy + paste through the clipboard, the copy is a new
  group), Prefab (File > Export Prefab with the group's name), Ungroup and Delete (asks first, Ctrl+Z
  brings it back). **Ctrl+Shift+U** ungroups whatever the selection touches.
- Placing a prefab with more than one object makes it a group named after the prefab.
- Groups are kept in the Ariane data folder (`groups.txt`) and found again by model and position when
  Ariane starts, so they survive restarts and agent checkpoint restores. Agents see and create the same
  groups (`editor-groups`, MCP `editor_groups` / `editor_group_create` / `editor_ungroup`).

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
