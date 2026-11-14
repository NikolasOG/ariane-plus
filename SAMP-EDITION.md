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
- **Export Pawn…** — custom helpers (`Map_CreateObject` / `Map_RemoveBuilding`), streamer
  (`CreateDynamicObject`) or native (`CreateObject`).
- **Reload from server** — rebuild the layer from what the server runs now. Ariane does this
  automatically on start, and asks before a sync would wipe a non-empty server map.
- **Server folder…** then **Sync to server** — writes `scriptfiles/ariane_live.txt` (the
  custom gamemode picks it up within ~3 s) and, optionally,
  `gamemodes/wasteland/maps/<name>.inc`.

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
