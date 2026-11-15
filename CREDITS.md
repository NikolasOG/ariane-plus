# Credits

Ariane Plus is an **unofficial fork** of [Ariane](https://github.com/Dryxio/ariane) by Dryxio. It is not
endorsed by Dryxio. All credit for Ariane itself stays with its creators; this file lists everyone whose
work is in Ariane Plus, and the licenses that apply.

## Ariane and its roots

- **Dryxio** — [Ariane](https://github.com/Dryxio/ariane): the map viewer and editor Ariane Plus is built on.
- **aap** — the original [euryopa / librwgta](https://github.com/aap/librwgta) project Ariane grew out of,
  and [librw](https://github.com/aap/librw), the RenderWare engine reimplementation it renders with.
- **Southland-FR** — the [`ariane` branch of librw](https://github.com/Southland-FR/librw/tree/ariane)
  that Ariane builds against.

### Contributors (from the git history)

aap · Dryxio · Martin Turski · Anonymous Maarten · spicybung · dkluin · NikolasOG

## Ariane Plus changes — NikolasOG

- **SA-MP edition**: SA-MP mode that protects the game files, a separate SA-MP layer, SA-MP/open.mp models.
- **Pawn import / export**: import SA-MP, Texture Studio and MTA maps; export as streamer, native
  open.mp / SA-MP or your own wrapper functions; copy a selection as Pawn.
- **Live sync**: Sync to server / Reload from server through `scriptfiles/ariane_live.txt`.
- **Materials**: SA-MP material and material-text overrides, rendered in the editor, editable in Object Info.
- **Custom models**: `AddSimpleModel` models from `models/artconfig.txt`.
- **World-building removal**: remove game buildings as `RemoveBuildingForPlayer`, with confirmation.
- **Removed Buildings panel**: every removal, with Go to / Restore.
- **SA-MP Checks**: floating objects, objects on roads and dirt tracks, explosive props, duplicates,
  object limits and streamer density.
- **Show in game**: sync, then move chosen players to the editor's view.
- **Model Viewer**: an orbitable close-up of any model with collision, materials and textures.
- **Mapping styles**: curated model sets for consistent maps.
- **Building blocks**: exact-size plain shapes to compose custom objects.
- **Groups**: named object groups that select, move, rotate, duplicate and save as prefabs together.
- **Server SDK** (`tools/samp/ariane.inc`): live map, Show in game, focus the editor from the server,
  freecam, callbacks; plus the ready-made `ariane_live.pwn` filterscript.
- **Agent toolkit**: mapping helpers, CLI and MCP tools for AI-assisted mapping, and `SKILLS.md`.
- Input fixes (quick clicks and key taps), panel layout, thumbnail orientation and more — see
  `SAMP-EDITION.md` and the git history for the full list of changes.

## Licenses

Ariane Plus as a whole is licensed under the **GNU General Public License, version 2 or any later
version** (see [`LICENSE`](LICENSE)), because it includes LZO, as the upstream README states. These parts
keep their own licenses; their texts are in [`licenses/`](licenses):

| Component | Authors | License | Text |
|---|---|---|---|
| librw | aap | MIT | [`licenses/librw-MIT.txt`](licenses/librw-MIT.txt) |
| Dear ImGui | Omar Cornut | MIT | [`licenses/imgui-MIT.txt`](licenses/imgui-MIT.txt) |
| ImGuizmo | Cedric Guillemet | MIT | [`licenses/imguizmo-MIT.txt`](licenses/imguizmo-MIT.txt) |
| LodePNG | Lode Vandevenne | zlib | [`licenses/lodepng-zlib.txt`](licenses/lodepng-zlib.txt) |
| miniLZO | Markus F.X.J. Oberhumer | GPL-2.0-or-later | [`licenses/minilzo-GPL-2.0.txt`](licenses/minilzo-GPL-2.0.txt) |
| Inter font (`fonts/Inter-Regular.ttf`) | The Inter Project Authors | SIL OFL 1.1 | [`licenses/Inter-OFL-1.1.txt`](licenses/Inter-OFL-1.1.txt) |
| Font Awesome Free (`fonts/fa-solid-900.ttf`) | Fonticons, Inc. | SIL OFL 1.1 (fonts) | [`licenses/FontAwesome-Free.txt`](licenses/FontAwesome-Free.txt) |
| SA-MP server scripts (`tools/samp/ariane.inc`, `ariane_live.pwn`) | NikolasOG | MIT | header of each file |

The two font license texts were downloaded unchanged from their upstream repositories
([rsms/inter](https://github.com/rsms/inter/blob/master/LICENSE.txt),
[FortAwesome/Font-Awesome](https://github.com/FortAwesome/Font-Awesome/blob/6.x/LICENSE.txt)).

The server scripts are MIT rather than GPL so gamemodes and filterscripts can include them freely,
whether or not those are open source.

## Trademarks

Grand Theft Auto III, Vice City and San Andreas are trademarks of Rockstar Games / Take-Two Interactive.
Ariane Plus is not affiliated with them and ships no game files; you need your own copy of the game.
SA-MP and open.mp belong to their respective teams.
