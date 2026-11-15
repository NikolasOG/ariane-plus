# Ariane SA-MP mapping toolkit (agents)

Everything here works the same from the CLI (`arianectl ...`) and from the MCP
connector (`ariane-mcp`, tool names in the second column). The connector's
`mapping_toolkit_guide` tool returns this file.

First time? Start with `SKILLS.md` at the repository root: a preflight check,
the build loop and ready-made recipes. This file is the full reference.

## 0. Setup checklist

| Need | How |
| --- | --- |
| Ariane running with the agent bridge | `run_ariane_samp.bat` (templates in `scripts/`): sets `ARIANE_GAME_DIR`, `ARIANE_ENGINE_TCP_PORT`, `ARIANE_ENGINE_TOKEN` |
| SA-MP server folder set | in Ariane: SA-MP > Server folder... (sync, custom models and the live file use it) |
| CLI / MCP wired to the same state | `arianectl.cmd` / `ariane-mcp.cmd` templates: same port, token, `ARIANE_ASSET_DB`, `ARIANE_AGENT_STATE_DIR`, `ARIANE_DISCOVERY_DIR`, `ARIANE_GAME_DIR` |
| Asset hazards tagged (once) | `arianectl assets mark-hazards` / `asset_mark_hazards` - search then hides explosive models |
| Ariane window visible | captures of a minimised window are empty: `scripts/show_ariane.ps1` |
| Restart after a rebuild | `scripts/restart_ariane.ps1 -Checkpoint name` - saves, restarts, restores objects + world removals |

Edits need a session: `arianectl session begin` ... `session commit`.
Instance ids change after an Ariane restart or a checkpoint restore - re-list
objects by position (`list-page --x --y --radius`) instead of reusing old ids.
On the CLI, options go **before** `--` and negative coordinates after it:
`arianectl place 1345 -- -420 1320 0 90`.

## 1. Workflow that produces clean maps

0. **Style**: when the user asks for a style ("wasteland", "make it look like
   the farm"), call `style_show` (`styles show NAME`) first and choose models
   from it - see section 9. `styles_list` shows what exists.
1. **Survey** the site: `ground-scan --bbox=x0,y0,x1,y1 --out scan.json --png scan.png`
   (`map_ground_scan`). The scan holds ground height + surface per metre: dirt
   tracks (`DIRTTRACK`), roads (tarmac), pavements, sand, grass. `plan-map`
   (`map_plan`) draws world buildings, trees, placed objects and tracks.
2. **Plan** on paper: perimeter polygon, gates centred on real dirt tracks
   (`track-mask scan.json --at X Y` / `map_track_query`), zones for housing and
   for functional buildings (workshop, water, kitchen, clinic, armory, market,
   storage, power, radio...). A settlement needs both.
3. **Clear** blocking world objects: `suppress-world-models` (journalled, kept in
   checkpoints, exported as RemoveBuildingForPlayer), or replace them
   (`replace-building` / `building_replace`).
4. **Walls**: `wall-run` (`map_wall_run`) - gap-free containers + fence sections,
   gate posts. Gatehouses: `prefab place gatehouse_main X Y HEADING`.
5. **Fill**: `pack` (`map_pack`) puts structures on free ground facing the
   tracks; prefabs for multi-part buildings; individual placements for detail.
6. **Ground everything**: `settle` (`map_settle`) after every batch. Snapping
   alone leaves objects floating on slopes. Buildings modelled for sloped sites
   (mesh above collision): `settle --visual --ids ...`; mounds/heaps: `--edge-stat max`.
7. **Check**: `lint --site site.json` (`map_lint`) until clean, then look:
   `capture-ortho` (`map_capture_ortho`) from above and `capture-pose` at player
   height. Fix what you see - lint cannot judge looks.
8. **Checkpoint** (`checkpoint save name`) and **sync** (`samp-sync` /
   `samp_sync`): the running gamemode reloads within seconds.

## 2. Rules (learned the hard way)

* No gaps between wall pieces; no pieces inside each other. `wall-run` hides
  slack as overlap behind containers and staggers fence laps; lint checks it.
* Nothing on dirt tracks or roads except gate leaves/barriers (`track_ok_models`).
* Nothing floating: settle, then check from the side. Cover visible gaps under
  open-backed models (`assets sides` / `asset_open_sides` tells which side is
  not drawn) with other debris or turn that side to a wall.
* No explosive props (barrel 1225, flame drum 1370...) - they blow up in game.
* No props perched on fences, roofs or other props by accident (lint `perched`).
* Removing a house that carries its own lot ground leaves a hole -
  `replace-building` fills it with ground planes; power lines drawn into shared
  detail models stay hanging - `--wire-poles` plants poles at the ends
  (`wire-ends` / `building_wire_ends` finds them).
* Scaffolding/boarding only on real walls: `facade` probes the wall at two
  heights and rejects galleries and overhangs - do not line bounding boxes.
* Night: SA-MP objects lack the prelighting of stock buildings, so painted
  props glow at night. A gamemode can tint outdoor map objects by hour with a
  material colour (modelid -1, "none", "none", ARGB) - skip objects that carry
  their own materials, the tint would replace them.

## 3. Tool map

| CLI | MCP | What |
| --- | --- | --- |
| `ground-scan` | `map_ground_scan` | height + surface grid, PNG |
| `track-mask` | `map_track_query` | distance/direction to tracks or roads |
| `plan-map` | `map_plan` | top-down plan PNG (+ lint issues) |
| `capture-ortho` | `map_capture_ortho` | orthographic screenshot, known scale |
| `world-items` | `map_world_items` | stock objects in a rectangle |
| `settle` | `map_settle` | ground placed objects (stack aware) |
| `delete` (many ids), `transform-batch` | `map_delete_many`, `map_transform_many` | bulk edits |
| `wall-run` | `map_wall_run` | gap-free perimeter wall with gates |
| `pack` | `map_pack` | fill a zone on free ground, facing tracks (`--style`) |
| `styles list/show/add/remove/pick` | `styles_list`, `style_show`, `style_pick`, `style_add_models`, `style_remove_models` | curated model sets (section 9) |
| `lint` | `map_lint` | rule checks for a site |
| `prefab list/save/place` | `prefab_list/save/place` | multi-part structures (bundled: `gatehouse_main`, `ruin_lot`) |
| `replace-building` | `building_replace` | swap a world building (and LOD), fill the hole |
| `facade` | `building_facade` | scaffolding etc. on real walls |
| `wire-ends` | `building_wire_ends` | hanging power-line ends |
| `assets sides` / `size-match` / `mark-hazards` | `asset_open_sides` / `asset_size_match` / `asset_mark_hazards` | asset facts |
| `material set/preset/text/clear/get/copy/model/txd/presets` | `material_*`, `model_materials`, `txd_textures`, `texture_presets` | retexturing |
| `custom-models list/reload/add` | `custom_models_list/reload`, `custom_model_add` | AddSimpleModel models |
| `samp-sync` | `samp_sync` | push to the server |
| `samp-export --format streamer\|native\|wrapper` | `samp_export` | Pawn include file (open.mp and SA-MP share the native API) |
| `blocks list/place/box/panel/room/beam/pole/ramp/roof/column` | `blocks_list`, `block_*` | building blocks: exact-size shapes to compose custom objects |
| `prefab save --rigid` | `prefab_save(rigid=True)` | keep a composed object's exact 3D layout |
| `checkpoint save/restore/list` | (service calls) | full snapshots incl. removals + materials |

Python: the same functions live in `mapping_tools`, `ground`, `lint`,
`prefabs`, `building_tools`, `materials`, `custom_models`, `styles` (import them with an
`ArianeService`).

## Building blocks (custom objects)

Compose your own objects (stalls, towers, shacks, tanks, signs, barricades)
from plain stock SA-MP shapes, then retexture them. Every block has real
collision and ONE material slot, so one material call retextures it and the
result works in game without downloads. Module `blocks.py`.

| Kind | Sizes (m) | Notes |
| --- | --- | --- |
| cube | 1, 5, 10 | pivot at the bottom face |
| block | 1x1x5, 3x3x5, 5x5x5, 10x10x5, 10x1x5 | concrete |
| beam / slab | 1x1x25 / 1x25x25 | |
| panel | 1.61x3.5, 3.21x3.5, 9.63x3.5, 9.63x10.5 (0.18 thick), 5.94x5.1 (0.26) | many stock textures: `blocks list` shows `variants` |
| panel_door / panel_window | 3.21x3.5 with a door / window hole; 5.94x5.1 with windows | |
| plate | 1x1x0.05 | finest piece: thin sheets, signs, railings |
| sheet | 6.25 (0.125), 12.5 (0.25), 25 (0.5) | big floors |
| pole | 10.9 long | 0.24 m sleeve (bottom 5 m) + 0.12 m shaft (top 5.9 m) |

Builders - every size is exact, pieces never stick out (except where noted):

* `block_box` / `blocks box X Y Z W D H`: solid box centred on X Y Z, tiled from
  the fewest pieces that fit; the last piece of a row is pulled back flush and
  nudged 1 cm so overlapping faces never flicker. Thinnest square section is
  1 m: a smaller box reports `overshoot` (use a panel, plate or pole instead).
* `block_panel`: flat plate W x H (thickness along local Y), any rotation.
* `block_room`: hollow shell standing on a floor height, optional ceiling and
  door opening (`--door -y:1.4:2.2`).
* `block_beam` (square) / `block_pole` (round): along a 3D line.
* `block_ramp`: sloped plate between a low and a high edge (top surface).
* `block_roof`: gable roof over a rectangle (eaves height, rise, overhang).
* `block_column`: n-sided ring of panels (tanks, silos, towers).
* `block_place`: one block centred where you say.

Rotation: heading about Z, pitch about local X, roll about local Y
(M = Rz(heading) Ry(roll) Rx(pitch)). Textures: a preset name or
[model, txd, texture]; blank presets `white`, `grey`, `grey_light`,
`grey_dark`, `black_flat`, `red`, `blue`, `green`, `yellow`, `orange` take any
`color` cleanly, plus `samp_white`, `block_plain`, `concrete_plain`,
`metal_plain`, `metal_scratched`, `steel_sheet`, `wood_panel`, `wood_plain` and
all the wasteland presets. Plates and panels are the cheap pieces: think in
1 m plates and 1.61/3.21/9.63 m panels when choosing sizes.

Save the result with `prefab save NAME --ids ... --origin X Y H --rigid`
(`prefab_save(rigid=True)`): rigid prefabs keep every part's exact offset and
tilt and are placed as one piece on the ground under the new origin.
Bundled example: `watchtower_blocks` (78 parts: pole legs, plank deck, railings,
pitched roof, ramp, crates, water tank).

## 4. Site file (lint)

```json
{"name": "My camp",
 "bbox": [x0, y0, x1, y1],
 "perimeter": [[x, y], ...],
 "gates": [{"x": 0, "y": 0, "half_cut": 4.5}],
 "wall_models": [2932, 2934, 2935, 19641],
 "track_ok_models": [1423, 976, 988],
 "ignore_models": [19632, 1463],
 "shelter_models": [3414, 3418],
 "debris_models": [879, 880, 3097, 10984, 12957],
 "floor_models": [18981, 19543, 19544, 19545],
 "dressing_models": [],
 "fixed_zones": [], "ignore_zones": [],
 "scan": "scan.json"}
```

Every key is optional except what the checks need (`scan` for track/road
checks, `perimeter` for wall checks). Paths are relative to the site file. See
`examples/` for a real one.

## 5. Retexturing (SetDynamicObjectMaterial)

* `material model <id>` / `model_materials` lists a model's slots (index ->
  texture) and every texture in its txd. `material txd <name>` lists any txd.
* `material set ID SLOT MODEL TXD TEXTURE --color 0xAARRGGBB`; `--color` 0 keeps
  the colour. TXD/TEXTURE `none none` keeps the texture and only recolours.
* `material preset ID SLOT rusty_metal` - presets (all read from real models):
  invisible, black, rusty_metal, rusty_pole, corrugated_metal, corrugated_tile,
  corrugated_grey, old_paint, sheet_metal, mesh_fence, barn_wood,
  barn_wood_dark, slatted_wood, boardwalk, demolished_wall, blistered_paint,
  burnt_wall, peeling_ceiling, concrete, roof_tarmac, rubble, adobe, cardboard,
  dirt, container_red/blue/yellow.
* `material text ID SLOT "KEEP OUT\nRADIATION" --back-color 0xFF202020` - signs.
  Ariane draws text slots as their background colour; the text shows in game.
* Overrides are drawn in the editor, kept in checkpoints and prefabs, written
  to the live file (`M`/`T` lines) and to Pawn exports (Map_/streamer/native
  API, Texture Studio style `tmpobjid = ...`). Importing Texture Studio / Pawn
  maps reads `Set(Dynamic)ObjectMaterial(Text)` calls too.
* A colour also removes the model's vertex lighting in game - prefer dark,
  desaturated colours outdoors.

## 6. Custom models (open.mp artwork)

open.mp serves `<server>/models` to players; `models/artconfig.txt` lists
`AddSimpleModel(world, baseid, newid, "x.dff", "x.txd");` with `newid` in
-1000 .. -30000 and `baseid` the stock model whose collision the client uses.

* `custom-models add <server> x.dff x.txd --base 19353` (`custom_model_add`)
  copies the files, appends the line and loads it. `custom-models list` maps
  each SA-MP id to the internal `model_id` to place in Ariane (30000+).
* Ariane reloads artconfig.txt whenever it loads the server map or imports a
  file; scene items show `samp_model_id`; sync/export write the SA-MP id.
* Collision in the editor comes from the base model - pick one with a similar
  shape (`assets size-match`).
* Only simple (single-atomic) models are supported - like AddSimpleModel.
* `img_extract.py <gta3.img> <out> name.dff name.txd` pulls stock files to start
  from or to test with.

## 7. Server side

`scriptfiles/ariane_live.txt` is written on every sync. Load the bundled
filterscript `tools/samp/ariane_live.pwn` (streamer plugin) and the server
rebuilds the map within ~3 s; or parse the file in your own gamemode:

```
rev <n>
R <model> <x> <y> <z> <radius>                     RemoveBuildingForPlayer
O <model> <x> <y> <z> <rx> <ry> <rz> <world> <int>  object (model < 0 = custom)
M <index> <model> <txd> <texture> <ARGB>           material of the object above
T <index> <size> <fontsize> <bold> <fontARGB> <backARGB> <align> <font_with_underscores> <text, \n escapes>
```

For a static map use SA-MP > Export Pawn (streamer, native open.mp / SA-MP,
or wrapper functions with your own prefix). The sync can also write that
include on every sync (SA-MP > Server folder...).

## 8. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| capture is empty / 1 px | Ariane minimised - `scripts/show_ariane.ps1` |
| ortho capture is all sky | old build - fog/sky are skipped in ortho now |
| delete/transform fails after restart | ids changed - list by position again |
| settle does nothing | `ARIANE_GAME_DIR` missing, or the object is a rider on something bigger |
| object floats although settled | mesh drawn above its collision - `settle --visual` |
| "refusing to sync an empty layer" | the layer was not reloaded after a restart - SA-MP > Load from server, or pass force only if you mean it |
| custom model not listed | server folder not set, artconfig syntax, missing file - `custom-models list` shows the error |

## 9. Mapping styles

A style is a user-curated set of models ("wasteland": wrecks, ruins, rubble,
huts, container walls) with a role per model, a weight, notes, preferred
texture presets and an avoid list. **When asked to map in a style, call
`style_show` first and choose models from it**; go outside it only for a role
the style has no model for (say so). Never use its avoid list.

* Files: one JSON per style in Ariane's data folder,
  `<ariane.exe dir>/ariane/styles/<name>.json` (next to `samp_settings.ini`).
  The editor (Window > Mapping Styles, Ctrl+Shift+M) and the tools share them;
  each picks up the other's changes. Override the folder with
  `ARIANE_STYLES_DIR` (or `ARIANE_DATA_DIR`). Styles shipped in
  `tools/agent/styles/` (`wasteland`) are copied there once.
* Roles: `wall`, `shelter`, `ruin`, `prop`, `debris`, `vehicle`, `vegetation`,
  `ground`, `light`, `sign`, `other` (free text is allowed). `weight` = how often
  a model is picked (1 normal, 0 never).
* Editor: Object Browser > "Add to style" button or right-click a row/tile >
  Add to style > STYLE > role; SA-MP > Add selection to style (models of the
  selected world objects). The window edits roles, weights, notes, textures
  and the avoid list, and places a model with one click.

| CLI | MCP | What |
| --- | --- | --- |
| `styles list` | `styles_list` | styles, model counts per role |
| `styles show NAME` | `style_show` | models by role, texture presets (resolved), avoid list, rules |
| `styles pick NAME --role shelter,ruin --count 4` | `style_pick` | weighted pick (distinct while the pool lasts) |
| `styles add NAME IDS --role wall --weight 1 --notes ..` | `style_add_models` | add / update models (creates the style); ids are probed, explosive props refused |
| `styles remove NAME IDS` / `--delete` | `style_remove_models` | remove models / the style |
| `pack --style NAME --style-role shelter` | `map_pack(style=, style_role=)` | no models: the style's models by role, with their weights; with models: `--style-mode prefer` (weights style models up) or `limit` (only style models) |
| `assets search Q --style NAME` | `search_assets(style=, style_role=, style_mode=)` | style models first (tagged `style_role`), `limit` = only them |

```
style_show("wasteland")
map_pack(zone, style="wasteland", style_role="shelter", count=12, scan_path=...)
map_pack(zone, style="wasteland", style_role="vehicle,debris", count=20, gap=2.5, scan_path=...)
material_preset(instance_id, 0, "rusty_metal")       # from the style's textures
```
