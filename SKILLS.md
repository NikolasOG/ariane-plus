---
name: ariane-samp-mapping
description: Map for an SA-MP / open.mp server with the Ariane SA-MP edition through arianectl or the ariane MCP connector - first-run checks, the build loop, and ready-made recipes for walls, settlements, ruins, retexturing and custom models. Use when asked to build, edit, check or sync a map in Ariane.
---

# Ariane SA-MP mapping - first-run skill

Read this once before your first edit. It is the short path; the full
reference is `tools/agent/MAPPING_TOOLKIT.md` (MCP: `mapping_toolkit_guide`).
To use this as a Claude Code skill, copy it to
`.claude/skills/ariane-samp-mapping/SKILL.md`.

Every step names the CLI command and the MCP tool. On the CLI, put options
before `--` and coordinates after it: `arianectl place 1345 -- -420 1320 0 90`.

## 1. Preflight (2 minutes, do it every first session)

| # | Check | CLI | MCP | Good result |
| --- | --- | --- | --- | --- |
| 1 | Bridge is up | `arianectl ping` | `ariane_status` | `"ok": true`; "connection refused" = Ariane isn't running (`run_ariane_samp.bat`) |
| 2 | Window not minimised | `scripts/show_ariane.ps1` | - | `minimised=False`; otherwise captures are empty |
| 3 | Server folder set | `arianectl custom-models list` | `custom_models_list` | `dir` ends in `<server>/models` |
| 4 | Layer loaded | `arianectl list-page --limit 1` | `list_scene_objects(limit=1)` | `page.total` matches the server map. If 0 but the server has a map, use SA-MP > Reload from server - **never sync an empty layer** |
| 5 | Hazards tagged (once per install) | `arianectl assets mark-hazards` | `asset_mark_hazards` | explosive props drop out of search |
| 6 | Safety copy | `arianectl checkpoint save before-<task>` | `save_checkpoint` | object count reported |

Edits need a session: `arianectl session begin` / `begin_edit_session`, and
`session commit` / `finish_edit_session(commit=True)` when done.

### Mapping styles

When the user asks for a style ("wasteland", "in the farm style"), call
`style_show` (`arianectl styles show NAME`) first and choose models from it:
its models by role (weight = how often), its texture presets and its rules.
Never use its avoid list. `map_pack(..., style=NAME, style_role="shelter")`
packs straight from the style; `style_pick(NAME, role, count)` picks models for
hand placement. No style named yet? `styles_list`. The user curates styles in
the editor (Window > Mapping Styles); add models with `style_add_models` only
when asked.

## 2. The build loop

```
look  -> plan -> place -> settle -> lint -> look -> checkpoint -> sync
```

1. **Look**: `ground-scan --bbox=x0,y0,x1,y1 --out site/scan.json --png site/scan.png`
   (`map_ground_scan`) once per site, then `plan-map` (`map_plan`) and
   `capture-ortho` (`map_capture_ortho`). Keep the scan; most tools reuse it.
2. **Plan** the layout in your head or in a site JSON (copy one from
   `tools/agent/examples/`). Gates and fronts go on real dirt tracks: check
   with `track-mask scan.json --at X Y` (`map_track_query`).
3. **Place** in batches (patches, `wall-run`, `pack`, `prefab place`). Avoid
   one-object-per-call loops.
4. **Settle** after every batch: `settle --bbox=...` (`map_settle`). Snapping
   alone floats things on slopes.
5. **Lint**: `lint --site site.json` (`map_lint`). Fix until 0 issues.
6. **Look again** from above and at player height (`capture-pose` /
   `capture_from_pose`). Lint cannot see ugliness, open-backed models or
   gaps under objects.
7. **Checkpoint**, then **sync**: `samp-sync` (`samp_sync`). A server running
   the bundled `tools/samp/ariane_live.pwn` filterscript reloads within ~3 s.
   For a static map: `samp-export --format streamer|native|wrapper` (`samp_export`).
   In the editor all SA-MP actions have Ctrl+Shift shortcuts (Window > Keyboard Shortcuts > SA-MP).

## 3. Recipes

### A walled settlement
```
ground-scan            map_ground_scan(bbox, "site/scan.json", png_path=...)
track-mask --at        map_track_query(scan, [[gx, gy], ...])      # gate spots on tracks
wall-run --dry-run     map_wall_run(points, gates=[{x,y,opening:8}], dry_run=True)
wall-run               map_wall_run(points, gates=...)
prefab place gatehouse_main X Y HEADING                            # at each main gate
pack                   map_pack(zone, [houses...], count=20, scan_path=...)
pack                   map_pack(zone, [workshop, water tank, stalls...], count=8, ...)
settle                 map_settle(bbox)
lint                   map_lint("site/site.json")
```
Mix housing with functional buildings (workshop, water, kitchen, clinic,
armory, market, storage, power, radio). Dense, but every structure on free
ground.

### Damage an existing town
```
world-items            map_world_items(bbox, name_filter="hus")    # find houses
assets size-match      asset_size_match(width, depth, query="ruin")
replace-building X Y NEW --wire-poles                              # building_replace
prefab place ruin_lot X Y HEADING                                  # debris for the lot
facade X Y 1426 --scan scan.json                                   # building_facade: scaffolding (DYN_SCAFFOLD) on real walls
assets sides MODEL     asset_open_sides(model)                     # cover any open side
settle --visual --ids ...                                          # ruins drawn above their collision
```

### Retexture or add a sign
```
material model MODEL          model_materials(model)               # slot -> texture
material presets              texture_presets(check=True)
material preset ID SLOT rusty_metal     material_preset(...)
material set ID SLOT MODEL TXD TEX --color 0xFF5A4A3A              # material_set
material text ID SLOT "KEEP OUT\nRADIATION" --back-color 0xFF202020  # material_text
material copy FROM TO...      material_copy(from_id, [to_ids])
```
Retexturing is kept in checkpoints, prefabs, sync and Pawn export. Use dark,
desaturated colours outdoors: a tint removes the model's vertex lighting.

### Add a custom model
```
img_extract.py gta3.img out name.dff name.txd   # optional: start from a stock model
custom-models add <server> my.dff my.txd --base <similar stock id>   # custom_model_add
custom-models list                              # samp_id -> model_id to place
```
Place the internal `model_id` (30000+); sync writes the negative SA-MP id.
Only simple single-mesh models work, and collision comes from the base model.

### Remove world clutter
`suppress-world-models X Y RADIUS MODEL...` (`suppress_native_models`) only
removes the listed models. Get the ids from `world-items` first. Removals
are kept in checkpoints and sync as `RemoveBuildingForPlayer`.

### Build a custom object from blocks
```
blocks list                 blocks_list()                              # shapes and exact sizes
blocks box --texture barn_wood -- X Y Z 4.4 4.4 0.25                   # block_box: a deck
blocks pole --texture rusty_pole -- X0 Y0 Zground X0 Y0 Ztop           # block_pole: legs / posts
blocks panel --thickness 0.05 --texture corrugated_metal -- X Y Z 4.4 1 # block_panel: railings
blocks roof --texture corrugated_tile -- X Y Zeave 4.4 4.4 1.0         # block_roof
blocks ramp --texture barn_wood_dark -- X0 Y0 Z0 X1 Y1 Z1 1.2          # block_ramp
blocks column --sides 8 --texture rusty_metal -- X Y Z 1.1 2.2         # block_column: tank
prefab save my_tower --ids ... --origin X Y 0 --rigid                  # prefab_save(rigid=True)
```
Blank presets (`white`, `grey`...) plus `--color 0xFF8A6A4A` give any plain
colour. Example to copy: `prefab place watchtower_blocks X Y HEADING`.

## 4. Habits that save time

* **Look at a model before using it**: `model-view MODEL --out m.png --clay --collision --human`
  (`model_view`) shows its real shape, scale and collision from any angle - catch open-backed
  models, mismatched collision and the wrong size before placing dozens.

* **Group what belongs together**: after building a house, a camp or a checkpoint, name it with
  `editor-groups create "north gate" ID ID ...` (`editor_group_create`). The user sees it in
  Window > Groups and can move, rotate (around its centre), duplicate or delete it in one go.
* **Point the user at the editor's own checks**: SA-MP > Checks lists floating objects, objects on
  roads and dirt tracks, explosive props and duplicates; SA-MP > Removed buildings lists every removal.

* **Dry-run first**: `wall-run`, `pack` and `settle` take `--dry-run`. Read the
  counts before committing hundreds of objects.
* **Batch**: bulk delete (`delete ID ID ...` / `map_delete_many`) and
  `transform-batch` / `map_transform_many` cost a few round trips, not hundreds.
* **Ids are not stable** across Ariane restarts or checkpoint restores. Look
  objects up again by position (`list-page --x --y --radius`).
* **Checkpoint before anything big**, with a descriptive name. Restoring is the
  undo for whole passes.
* **After rebuilding Ariane**, run `scripts/restart_ariane.ps1 -Checkpoint name`.
  It saves, restarts, rebinds and restores.
* **Long calls** (lint, scans, settle over a town) take 5-60 s. The MCP tools
  already use a long timeout, so don't retry them in a loop.
* **Keep site files** (`site.json` + `scan.json`) next to your work. They are
  your memory of perimeter, gates, zones and which models may stand where.

## 5. Never

* Sync an empty or half-loaded layer over a live server map.
* Leave gaps between wall pieces, or put pieces inside each other.
* Put anything on dirt tracks or roads except gate leaves and barriers.
* Use explosive props (barrel 1225, flame drum 1370; `mark-hazards` hides
  them). Avoid tyre piles by wrecks and shipping containers in town streets.
* Line a bounding box with scaffolding. Use `facade`, which finds the real walls.
* Declare a visual task done without looking at a capture.

## 6. When something is off

| You see | Do |
| --- | --- |
| Capture is black, grey or 1 px | `scripts/show_ariane.ps1`, then capture again |
| "instance does not belong to the active agent scene" | stale id after a restart: list by position |
| "begin a scratch session" | `session begin` / `begin_edit_session` |
| Object hovers after settle | `settle --visual --ids ID` (or `--edge-stat max` for mounds) |
| Lint flags props you meant to stack | add their models to `debris_models` / `shelter_models` / `floor_models` in the site file |
| Texture not found | `material txd NAME` to confirm the texture name; the txd name decides, not the model id |
| Custom model `ok: false` | read its `error` (missing file, unknown base, multi-mesh dff) |
