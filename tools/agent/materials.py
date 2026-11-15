"""SA-MP material overrides: retexture / recolour / put text on placed objects.

Mirrors SetDynamicObjectMaterial and SetDynamicObjectMaterialText. Overrides
are drawn in the editor (text materials show as their background colour),
saved in checkpoints and prefabs, exported to Pawn in the selected format and
written to the live sync file (the ariane_live filterscript applies them).

Colours are ARGB ints or strings ("0xFF808080"); 0 keeps the model's colour.
A material index is the slot in the model's material list - see
``model_materials`` for what each slot of a model currently shows.

Useful facts:
  * the txd name decides the texture; ``model`` is any model that uses that txd
    (SA-MP loads the txd through it) - ``model_materials(model)`` lists a model's
    txd and every texture in it, ``txd_textures(name)`` any txd's textures
  * texture "none" with txd "none" keeps the texture and only changes colour
  * setting a colour drops the model's vertex lighting in game, so a recoloured
    object looks flat/bright at night - prefer dark colours outdoors
"""

from __future__ import annotations

from typing import Any

# Retexture sources that suit a wasteland map. Every entry was read from the
# model's own material list (model_materials); ``check_presets`` re-checks them
# against the running install.
TEXTURE_PRESETS: dict[str, dict[str, Any]] = {
	# utility
	"invisible":          {"model": 19297, "txd": "matlights", "texture": "invisible", "note": "hide a slot / whole object"},
	"black":              {"model": 19297, "txd": "matlights", "texture": "black"},
	# post-apocalyptic surfaces
	"rusty_metal":        {"model": 12957, "txd": "sw_oldshack", "texture": "rustc256128"},
	"rusty_pole":         {"model": 12957, "txd": "sw_oldshack", "texture": "Gen_Rusty_Poll"},
	"corrugated_metal":   {"model": 16337, "txd": "des_quarrycrane", "texture": "ws_corr_metal3"},
	"corrugated_tile":    {"model": 16337, "txd": "des_quarrycrane", "texture": "sw_corrugtile"},
	"corrugated_grey":    {"model": 3275, "txd": "country_breakable", "texture": "ws_corr_metal1"},
	"old_paint":          {"model": 16337, "txd": "des_quarrycrane", "texture": "ws_oldpainted2"},
	"sheet_metal":        {"model": 1358, "txd": "BREAK_S_bins", "texture": "cj_sheetmetal"},
	"mesh_fence":         {"model": 3275, "txd": "country_breakable", "texture": "Gen_Meshfencing"},
	"barn_wood":          {"model": 12921, "txd": "sw_farm1", "texture": "sw_barnwood2"},
	"barn_wood_dark":     {"model": 12921, "txd": "sw_farm1", "texture": "sw_barnwood3"},
	"slatted_wood":       {"model": 1407, "txd": "BREAK_F_w", "texture": "CJ_SLATEDWOOD"},
	"boardwalk":          {"model": 13367, "txd": "sw_farm1", "texture": "boardwalk_la"},
	"demolished_wall":    {"model": 3866, "txd": "dem1_SFXRF", "texture": "ws_demolishwall2"},
	"blistered_paint":    {"model": 3866, "txd": "dem1_SFXRF", "texture": "ws_blistered1"},
	"burnt_wall":         {"model": 3887, "txd": "dem4_SFXRF", "texture": "ws_blistered1smoked"},
	"peeling_ceiling":    {"model": 3866, "txd": "dem1_SFXRF", "texture": "ws_peeling_ceiling1"},
	"concrete":           {"model": 3866, "txd": "dem1_SFXRF", "texture": "concretenewb256"},
	"roof_tarmac":        {"model": 3887, "txd": "dem4_SFXRF", "texture": "ws_rooftarmac1"},
	"rubble":             {"model": 10984, "txd": "demolished1_sfse", "texture": "ws_rubble1b"},
	"adobe":              {"model": 11426, "txd": "des_clifftown", "texture": "des_adobewall1"},
	"cardboard":          {"model": 5262, "txd": "imstuff_las2", "texture": "cardbrdirty128"},
	"dirt":               {"model": 1358, "txd": "BREAK_S_bins", "texture": "dirt64b"},
	"container_red":      {"model": 2934, "txd": "continX", "texture": "frate64_red"},
	"container_blue":     {"model": 2934, "txd": "continX", "texture": "frate64_blue"},
	"container_yellow":   {"model": 2934, "txd": "continX", "texture": "frate64_yellow"},
}


def _colour(value) -> str:
	if value is None:
		return "0"
	if isinstance(value, str):
		return value
	return "0x%08X" % (int(value) & 0xFFFFFFFF)


def set_material(service, instance_id: int, index: int, model: int, txd: str, texture: str, color=0) -> dict:
	return service.engine("set_material", [int(instance_id), int(index), int(model), txd, texture, _colour(color)])


def set_preset(service, instance_id: int, index: int, preset: str, color=0) -> dict:
	p = TEXTURE_PRESETS[preset]
	return set_material(service, instance_id, index, p["model"], p["txd"], p["texture"], color)


def set_material_text(service, instance_id: int, index: int, text: str, *, size: int = 90, font: str = "Arial",
                      font_size: int = 24, bold: bool = True, font_color=0xFFFFFFFF, back_color=0, align: int = 1) -> dict:
	"""size: SA-MP OBJECT_MATERIAL_SIZE_* value (90 = 256x128); align 0 left, 1 centre, 2 right."""
	return service.engine("set_material_text", [int(instance_id), int(index), int(size), int(font_size), int(bool(bold)),
	                                            _colour(font_color), _colour(back_color), int(align), font,
	                                            text.replace("\\", "\\\\").replace("\n", "\\n")])


def clear_materials(service, instance_id: int, index: int = -1) -> dict:
	return service.engine("clear_materials", [int(instance_id), int(index)])


def get_materials(service, instance_id: int) -> dict:
	return service.engine("get_materials", [int(instance_id)])


def copy_materials(service, from_id: int, to_id: int) -> dict:
	return service.engine("copy_materials", [int(from_id), int(to_id)])


def model_materials(service, model: int) -> dict:
	return service.engine("model_materials", [int(model)])


def txd_textures(service, txd: str) -> dict:
	return service.engine("txd_textures", [txd])


def apply_materials(service, instance_id: int, materials: list[dict]) -> int:
	"""Re-apply a ``materials`` list as returned by list/get (checkpoints, prefabs)."""
	n = 0
	for m in materials or []:
		if m.get("kind") == "text":
			set_material_text(service, instance_id, m["index"], m.get("text", ""), size=m.get("size", 90),
			                  font=m.get("font", "Arial"), font_size=m.get("font_size", 24), bold=m.get("bold", 1),
			                  font_color=m.get("font_color", "0xFFFFFFFF"), back_color=m.get("back_color", "0"),
			                  align=m.get("align", 1))
		else:
			set_material(service, instance_id, m["index"], m.get("model", -1), m.get("txd", "none"),
			             m.get("texture", "none"), m.get("color", "0"))
		n += 1
	return n


def check_presets(service) -> dict:
	"""Which presets resolve to a real texture in this game install."""
	out = {}
	for name, p in TEXTURE_PRESETS.items():
		try:
			tex = txd_textures(service, p["txd"]).get("textures") or []
			out[name] = any(t.lower() == p["texture"].lower() for t in tex)
		except Exception as error:  # unknown txd
			out[name] = f"error: {error}"
	return out
