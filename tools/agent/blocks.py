"""Building blocks: plain SA-MP shapes to compose and retexture into custom objects.

Every block here is a stock SA-MP object with real collision and exactly one
material slot (index 0), so one material call retextures the whole piece and
the result works in game with no downloads. Sizes and pivots were measured in
the editor (render bounds); ``refresh_catalogue`` re-measures them.

Kinds
  cube           1 / 5 / 10 m cubes (pivot at the bottom face)
  block          concrete 1x1x5, 3x3x5, 5x5x5, 10x10x5, 10x1x5
  beam           concrete 1x1x25
  slab           concrete 1x25x25 (stands upright; the builders turn it)
  panel          solid 0.18 m panels 1.61x3.5, 3.21x3.5, 9.63x3.5, 9.63x10.5; 0.26 m 5.94x5.1
  panel_door     3.21x3.5 panel with a door opening
  panel_window   3.21x3.5 panel with a window; 5.94x5.1 with windows
  plate          1x1x0.05 metal plate (pivot at the bottom face)
  sheet          flat 6.25 / 12.5 / 25 m sheets (0.125 / 0.25 / 0.5 thick)
  pole           steel pole, 10.9 m (pivot at the bottom): a 0.24 m square sleeve for the
                 bottom 5 m, a thin 0.12 m shaft for the top 5.9 m - posts, rails, pipes

Smallest square section is 1 x 1 m (cube / concrete); thinner members are a
panel on edge (0.18 x 1.61 m), a plate (0.05 m) or the round pole.

Builders (all return instance ids, all sizes exact):
  place_block   one block, centred where you say, any heading/pitch/roll
  build_box     any W x D x H solid box, tiled from the fewest fitting pieces;
                the last piece of a row is pulled back flush with the edge and
                nudged 1 cm so overlapping faces never flicker
  build_panel   a flat wall / floor / roof plate of exact size
  build_room    hollow shell: walls (+ floor, + ceiling) from panels
  build_beam    a square beam between two 3D points
  build_ramp    a sloped plate from a low edge to a high edge
  build_roof    pitched (gable) roof over a rectangle
  build_column  n-sided round column (ring of panels)
  build_pole    round 0.19 m pole between two 3D points (any length)

Rotation convention (measured): M = Rz(heading) * Ry(roll) * Rx(pitch), degrees,
pitch about local X, roll about local Y, heading about Z.

Textures: ``texture`` is a preset name from materials.TEXTURE_PRESETS ("white",
"grey", "concrete", "rusty_metal"...), a (model, txd, texture) tuple, or None to
keep the block's own look; ``color`` is an ARGB tint ("0xFF8A6A4A"). The blank
presets (white, grey, black...) take any colour cleanly.
"""

from __future__ import annotations

import itertools
import math
import uuid

try:
	from . import materials as material_mod
except ImportError:
	import materials as material_mod


# --------------------------------------------------------------- catalogue
# (model, name, kind, render bounds min, max) - measured in the editor
_RAW = [
	(19789, "Cube1mx1m", "cube", (-0.5, -0.5, 0.0), (0.5, 0.5, 1.0)),
	(19790, "Cube5mx5m", "cube", (-2.5, -2.5, 0.0), (2.5, 2.5, 5.0)),
	(19791, "Cube10mx10m", "cube", (-5.0, -5.0, 0.0), (5.0, 5.0, 10.0)),
	(18762, "Concrete1mx1mx5m", "block", (-0.5, -0.5, -2.5), (0.5, 0.5, 2.5)),
	(18763, "Concrete3mx3mx5m", "block", (-1.5, -1.5, -2.5), (1.5, 1.5, 2.5)),
	(18764, "Concrete5mx5mx5m", "block", (-2.5, -2.5, -2.5), (2.5, 2.5, 2.5)),
	(18765, "Concrete10mx10mx5m", "block", (-5.0, -5.0, -2.5), (5.0, 5.0, 2.5)),
	(18766, "Concrete10mx1mx5m", "block", (-5.0, -0.5, -2.5), (5.0, 0.5, 2.5)),
	(18980, "Concrete1mx1mx25m", "beam", (-0.5, -0.5, -12.5), (0.5, 0.5, 12.5)),
	(18981, "Concrete1mx25mx25m", "slab", (-0.5, -12.5, -12.5), (0.5, 12.5, 12.5)),
	(19426, "wall066", "panel", (-0.0894, -0.8029, -1.7502), (0.0894, 0.8029, 1.7502)),
	(19353, "wall001", "panel", (-0.0894, -1.6058, -1.7502), (0.0894, 1.6058, 1.7502)),
	(19445, "wall085", "panel", (-0.0894, -4.8173, -1.7502), (0.0894, 4.8173, 1.7502)),
	(19375, "wall023", "panel", (-0.0894, -4.8173, -5.2507), (0.0894, 4.8173, 5.2507)),
	(19464, "wall104", "panel", (-0.1279, -2.9684, -2.552), (0.1279, 2.9684, 2.552)),
	(19383, "wall031", "panel_door", (-0.0894, -1.6058, -1.7502), (0.0894, 1.6058, 1.7502)),
	(19399, "wall047", "panel_window", (-0.0894, -1.6058, -1.7502), (0.0894, 1.6058, 1.7502)),
	(19465, "wall105", "panel_window", (-0.1279, -2.9684, -2.552), (0.1279, 2.9684, 2.552)),
	(19843, "MetalPanel1", "plate", (-0.5, -0.5, 0.0), (0.5, 0.5, 0.05)),
	(19741, "STubeFlat6_25m1", "sheet", (-3.125, -3.125, -0.0625), (3.125, 3.125, 0.0625)),
	(19700, "MTubeFlt12_5x12_5m1", "sheet", (-6.25, -6.25, -0.125), (6.25, 6.25, 0.125)),
	(19650, "TubeFlat25x25m1", "sheet", (-12.5, -12.5, -0.25), (12.5, 12.5, 0.25)),
	(16101, "des_windsockpole", "pole", (-0.0688, -0.0922, 0.0066), (0.1173, 0.1035, 10.9294)),
]

# Same shape, other stock texture (retexturing makes them interchangeable)
VARIANTS = {
	19353: list(range(19353, 19375)), 19375: list(range(19375, 19383)), 19426: list(range(19426, 19445)),
	19445: list(range(19445, 19464)), 19383: list(range(19383, 19399)), 19399: list(range(19399, 19418)),
	19843: [19843, 19844, 19845, 19846],
}

# kinds a solid box may be tiled from (door/window panels have holes)
SOLID_KINDS = ("cube", "block", "beam", "slab", "panel", "plate", "sheet")


def _block(model, name, kind, mn, mx):
	size = tuple(round(mx[i] - mn[i], 4) for i in range(3))
	centre = tuple(round((mx[i] + mn[i]) / 2, 4) for i in range(3))
	return {"model": model, "name": name, "kind": kind, "size": size, "centre": centre,
	        "bounds": [list(mn), list(mx)], "slot": 0}


CATALOGUE = [_block(*r) for r in _RAW]
BY_MODEL = {b["model"]: b for b in CATALOGUE}


def refresh_catalogue(service) -> list[dict]:
	"""Re-measure every block's render bounds in the running editor."""
	for b in CATALOGUE:
		rb = service.probe_asset(b["model"], ensure_renderable=True).get("render_bounds")
		if rb:
			b.update(_block(b["model"], b["name"], b["kind"], rb[0], rb[1]))
	return CATALOGUE


def list_blocks(kind: str | None = None, max_size: tuple | None = None) -> list[dict]:
	out = [b for b in CATALOGUE if kind is None or b["kind"] == kind]
	if max_size:
		out = [b for b in out if all(s <= m + 1e-6 for s, m in zip(sorted(b["size"]), sorted(max_size)))]
	return [{**b, "variants": VARIANTS.get(b["model"], [b["model"]])} for b in out]


def resolve_block(block) -> dict:
	"""A catalogue entry from a model id, a name or a kind (smallest of that kind)."""
	if isinstance(block, dict):
		return block
	if isinstance(block, int) or (isinstance(block, str) and block.lstrip("-").isdigit()):
		m = int(block)
		for base, models in VARIANTS.items():
			if m in models:
				return {**BY_MODEL[base], "model": m}
		if m in BY_MODEL:
			return BY_MODEL[m]
		raise KeyError(f"model {m} is not a building block (see list_blocks)")
	for b in CATALOGUE:
		if b["name"].lower() == str(block).lower():
			return b
	kind = [b for b in CATALOGUE if b["kind"] == block]
	if kind:
		return min(kind, key=lambda b: b["size"][0] * b["size"][1] * b["size"][2])
	raise KeyError(f"unknown block {block!r}")


# --------------------------------------------------------------- rotation maths
def _rx(a):
	c, s = math.cos(a), math.sin(a)
	return [[1, 0, 0], [0, c, -s], [0, s, c]]


def _ry(a):
	c, s = math.cos(a), math.sin(a)
	return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def _rz(a):
	c, s = math.cos(a), math.sin(a)
	return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def _mul(a, b):
	return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _apply(m, v):
	return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def rotation(heading=0.0, pitch=0.0, roll=0.0):
	"""Engine rotation matrix: Rz(heading) * Ry(roll) * Rx(pitch)."""
	return _mul(_mul(_rz(math.radians(heading)), _ry(math.radians(roll))), _rx(math.radians(pitch)))


def euler(m) -> tuple[float, float, float]:
	"""(heading, pitch, roll) in degrees for a rotation matrix (inverse of rotation())."""
	sr = max(-1.0, min(1.0, -m[2][0]))
	roll = math.asin(sr)
	if abs(math.cos(roll)) > 1e-6:
		pitch = math.atan2(m[2][1], m[2][2])
		heading = math.atan2(m[1][0], m[0][0])
	else:                                   # gimbal lock: put everything in heading
		pitch = 0.0
		heading = math.atan2(-m[0][1], m[1][1])
	return (round(math.degrees(heading) % 360, 4), round(math.degrees(pitch), 4), round(math.degrees(roll), 4))


def _perm_matrices():
	"""Proper rotations that map block axes onto box axes (6 axis permutations)."""
	out = []
	for perm in itertools.permutations(range(3)):
		m = [[0.0] * 3 for _ in range(3)]
		for block_axis, box_axis in enumerate(perm):
			m[box_axis][block_axis] = 1.0
		det = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
		       + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
		if det < 0:                          # flip block axis 0: same box (centred), proper rotation
			m[perm[0]][0] *= -1
		out.append((perm, m))
	return out


PERMS = _perm_matrices()


# --------------------------------------------------------------- placing
GIMBAL_NUDGE = 0.05     # degrees kept away from roll = +-90 (see place_pieces)


def _texture_args(texture):
	if texture is None:
		return None
	if isinstance(texture, str):
		p = material_mod.TEXTURE_PRESETS[texture]
		return p["model"], p["txd"], p["texture"]
	if isinstance(texture, dict):
		return texture["model"], texture["txd"], texture["texture"]
	return tuple(texture)


def place_pieces(service, pieces: list[dict], *, texture=None, color=None, key_prefix: str = "block") -> list[int]:
	"""pieces: {model, centre (x,y,z) of the piece's box, matrix (3x3) or heading/pitch/roll}.
	Places everything in two patches, then applies the material to slot 0."""
	tag = uuid.uuid4().hex[:8]
	places, tilts, keys = [], [], []
	for i, p in enumerate(pieces):
		b = resolve_block(p["model"])
		m = p.get("matrix") or rotation(p.get("heading", 0.0), p.get("pitch", 0.0), p.get("roll", 0.0))
		heading, pitch, roll = euler(m)
		if abs(abs(roll) - 90.0) < GIMBAL_NUDGE:
			# the editor applies roll = +-90 fine but reports it back wrongly (gimbal
			# lock), so checkpoints/prefabs would restore a turned piece; 0.05 deg off
			# (3 mm over a 3.5 m panel) reads back exactly
			roll = math.copysign(90.0 - GIMBAL_NUDGE, roll)
			m = rotation(heading, pitch, roll)
		off = _apply(m, b["centre"])         # pivot -> box centre, in world axes
		x, y, z = (p["centre"][k] - off[k] for k in range(3))
		key = f"{key_prefix}:{tag}:{i:04d}"
		keys.append(key)
		places.append({"action": "place", "key": key, "model": b["model"], "x": x, "y": y, "z": z,
		               "heading": heading, "snap": False})
		if abs(pitch) > 1e-3 or abs(roll) > 1e-3:
			tilts.append((key, x, y, z, heading, pitch, roll))
	for i in range(0, len(places), 300):
		service.apply_scene_patch(places[i:i + 300])
	scene = {it.get("object_key"): it for it in service.enumerate_scene()}
	ops = [{"action": "transform3d", "instance_id": scene[k]["instance_id"], "x": x, "y": y, "z": z,
	        "pitch": pitch, "roll": roll, "heading": heading, "snap": False}
	       for k, x, y, z, heading, pitch, roll in tilts if k in scene]
	for i in range(0, len(ops), 300):
		service.apply_scene_patch(ops[i:i + 300])
	ids = [scene[k]["instance_id"] for k in keys if k in scene]
	tex = _texture_args(texture)
	if tex or color not in (None, 0, "0"):
		model, txd, name = tex or (-1, "none", "none")
		col = color if color is not None else "0"
		for iid in ids:
			material_mod.set_material(service, iid, 0, model, txd, name, col)
	return ids


def place_block(service, block, x, y, z, *, heading=0.0, pitch=0.0, roll=0.0, texture=None, color=None) -> int:
	"""One block with its box centred on (x, y, z)."""
	ids = place_pieces(service, [{"model": resolve_block(block)["model"], "centre": (x, y, z),
	                              "heading": heading, "pitch": pitch, "roll": roll}], texture=texture, color=color)
	return ids[0] if ids else None


# --------------------------------------------------------------- box tiling
THIN_AXIS = 0.3     # a box axis this thin is covered by thin pieces, counted by rounding
FIT_TOL = 0.015     # a piece this much shorter than the box still counts as a full fit


def _count(length, size):
	if length <= THIN_AXIS and size <= THIN_AXIS:
		return 1                                    # one layer of the closest thin piece, never a stack
	return max(1, math.ceil((length - FIT_TOL) / size - 1e-9))


def _axis_tiles(length, size, n):
	"""n centres along one axis (from -length/2); the last one is pulled back flush
	with the end when the pieces overrun it (flag True)."""
	if n == 1:
		return [(0.0, False)]
	if length <= THIN_AXIS and size <= THIN_AXIS:          # stacked thin layers, centred
		return [((i - (n - 1) / 2) * size, False) for i in range(n)]
	out = [(-length / 2 + size / 2 + i * size, False) for i in range(n)]
	if out[-1][0] + size / 2 > length / 2 + 1e-6:
		out[-1] = (length / 2 - size / 2, True)
	return out


def plan_box(width, depth, height, *, kinds=SOLID_KINDS, models=None, allow_overshoot=True) -> dict:
	"""Cheapest tiling of a W x D x H box: block + orientation + counts.

	A piece never sticks out of the box (each piece axis <= the box axis it lies
	along) except across a thin axis (<= 0.3 m), which takes the closest stack of
	thin pieces (a 0.1 m plate is one 0.18 m panel, not two plates). When no
	piece fits at all (e.g. a 0.5 x 0.5 m post: every block is >= 1 m or a thin
	sheet), the least oversized piece is used, centred, and "overshoot" says by
	how much (or pass allow_overshoot=False to get an error)."""
	dims = (width, depth, height)
	best = fallback = None
	cands = [b for b in CATALOGUE if b["kind"] in kinds and (models is None or b["model"] in models)]
	for b in cands:
		for perm, m in PERMS:
			# box axis j gets block axis perm.index(j)
			along = [b["size"][perm.index(j)] for j in range(3)]
			counts = [_count(dims[j], along[j]) for j in range(3)]
			over = [max(0.0, along[j] * (1 if counts[j] == 1 else 0) - dims[j]) for j in range(3)]
			thin_ok = [dims[j] <= THIN_AXIS and along[j] <= THIN_AXIS for j in range(3)]
			fits = all(along[j] <= dims[j] + 0.005 or thin_ok[j] for j in range(3))
			n = counts[0] * counts[1] * counts[2]
			mismatch = sum(abs(c * a - d) for c, a, d in zip(counts, along, dims))
			# across a thin axis, a piece much thinner/thicker than asked costs extra
			thin_off = sum(abs(along[j] - dims[j]) / max(dims[j], 1e-3) for j in range(3) if thin_ok[j])
			if fits:
				score = (round(n * (1.0 + thin_off), 3), mismatch)
				if best is None or score < best[0]:
					best = (score, b, perm, m, along, counts)
			else:
				excess = sum(o for j, o in enumerate(over) if not thin_ok[j])
				score = n + 10.0 * excess              # few pieces, little sticking out
				if fallback is None or score < fallback[0]:
					fallback = (score, b, perm, m, along, counts)
	overshoot = None
	if best is None:
		if fallback is None or not allow_overshoot:
			raise ValueError(f"no block fits inside {width} x {depth} x {height}")
		best = fallback
		_, b, perm, m, along, counts = best
		overshoot = [round(max(0.0, along[j] - dims[j]), 3) if counts[j] == 1 else 0.0 for j in range(3)]
	_, b, perm, m, along, counts = best
	return {"block": b, "perm": perm, "matrix": m, "piece_size": along, "counts": counts,
	        "pieces": counts[0] * counts[1] * counts[2], "overshoot": overshoot}


def box_pieces(cx, cy, cz, width, depth, height, *, heading=0.0, pitch=0.0, roll=0.0, kinds=SOLID_KINDS,
               models=None, nudge=0.01, plan=None) -> list[dict]:
	plan = plan or plan_box(width, depth, height, kinds=kinds, models=models)
	frame = rotation(heading, pitch, roll)
	piece_rot = _mul(frame, plan["matrix"])
	dims = (width, depth, height)
	axes = [_axis_tiles(dims[j], plan["piece_size"][j], plan["counts"][j]) for j in range(3)]
	pieces = []
	for (lx, sx), (ly, sy), (lz, sz) in itertools.product(*axes):
		local = [lx, ly, lz]
		shifted = [sx, sy, sz]
		if any(shifted) and nudge:
			# move a pulled-back piece 1 cm inward along the thinnest unshifted axis:
			# its faces then sit just behind its neighbour's instead of flickering
			free = [j for j in range(3) if not shifted[j]] or [2]
			j = min(free, key=lambda k: dims[k])
			local[j] -= nudge if local[j] >= 0 else -nudge
		w = _apply(frame, local)
		pieces.append({"model": plan["block"]["model"], "centre": (cx + w[0], cy + w[1], cz + w[2]), "matrix": piece_rot})
	return pieces


def build_box(service, cx, cy, cz, width, depth, height, *, heading=0.0, pitch=0.0, roll=0.0, texture=None,
              color=None, kinds=SOLID_KINDS, models=None, dry_run=False) -> dict:
	"""Solid box of exactly width (local X) x depth (local Y) x height (local Z),
	centred on (cx, cy, cz). Tiled from the fewest catalogue pieces that fit."""
	plan = plan_box(width, depth, height, kinds=kinds, models=models)
	pieces = box_pieces(cx, cy, cz, width, depth, height, heading=heading, pitch=pitch, roll=roll, plan=plan)
	result = {"ok": True, "block": plan["block"]["name"], "model": plan["block"]["model"], "pieces": len(pieces),
	          "counts": plan["counts"]}
	if plan["overshoot"]:
		result["overshoot"] = plan["overshoot"]
	if dry_run:
		return {**result, "dry_run": True}
	return {**result, "instance_ids": place_pieces(service, pieces, texture=texture, color=color)}


THIN = ("panel", "plate", "sheet")


def build_panel(service, cx, cy, cz, width, height, *, thickness=0.18, heading=0.0, pitch=0.0, roll=0.0,
                texture=None, color=None, dry_run=False) -> dict:
	"""Flat plate width (local X) x height (local Z), `thickness` thick (local Y):
	a wall when upright, a floor/roof with pitch=90 (or use build_box)."""
	return build_box(service, cx, cy, cz, width, thickness, height, heading=heading, pitch=pitch, roll=roll,
	                 texture=texture, color=color, kinds=THIN + ("block", "cube"), dry_run=dry_run)


def build_room(service, cx, cy, z_floor, width, depth, height, *, heading=0.0, thickness=0.18, floor=True,
               ceiling=False, door: dict | None = None, texture=None, color=None) -> dict:
	"""Hollow shell standing on z_floor: four walls (outer size width x depth),
	optional floor and ceiling plates. door={"side": "-y"|"+y"|"-x"|"+x",
	"width": 1.4, "height": 2.2, "offset": 0} leaves an opening."""
	frame = rotation(heading)
	t = thickness
	ids, parts = [], []

	def world(lx, ly, lz):
		w = _apply(frame, [lx, ly, lz])
		return cx + w[0], cy + w[1], z_floor + lz

	def wall(side, length, along_x, centre_local):
		segs = [(0.0, length)]
		if door and door.get("side") == side:
			dw, dh, off = door.get("width", 1.4), door.get("height", 2.2), door.get("offset", 0.0)
			a, b = length / 2 + off - dw / 2, length / 2 + off + dw / 2
			segs = [(0.0, a), (b, length)]
			lintel = height - dh
			if lintel > 0.05:
				parts.append(("lintel", side, along_x, centre_local, (a + b) / 2 - length / 2, dw, lintel, dh + lintel / 2))
		for s0, s1 in segs:
			if s1 - s0 > 0.05:
				parts.append(("wall", side, along_x, centre_local, (s0 + s1) / 2 - length / 2, s1 - s0, height, height / 2))

	wall("-y", width, True, (0.0, -depth / 2 + t / 2))
	wall("+y", width, True, (0.0, depth / 2 - t / 2))
	wall("-x", depth - 2 * t, False, (-width / 2 + t / 2, 0.0))
	wall("+x", depth - 2 * t, False, (width / 2 - t / 2, 0.0))
	pieces = []
	for _, side, along_x, (bx, by), off, length, h, zc in parts:
		lx, ly = (bx + off, by) if along_x else (bx, by + off)
		w, d = (length, t) if along_x else (t, length)
		pieces += box_pieces(*world(lx, ly, zc), w, d, h, heading=heading, kinds=THIN + ("block", "cube"))
	if floor:
		pieces += box_pieces(*world(0, 0, t / 2), width, depth, t, heading=heading, kinds=THIN + ("block", "cube"))
	if ceiling:
		pieces += box_pieces(*world(0, 0, height - t / 2), width - 2 * t, depth - 2 * t, t, heading=heading,
		                     kinds=THIN + ("block", "cube"))
	ids = place_pieces(service, pieces, texture=texture, color=color)
	return {"ok": True, "pieces": len(pieces), "instance_ids": ids}


def build_beam(service, start, end, *, thickness=0.25, width=None, texture=None, color=None, dry_run=False) -> dict:
	"""Square (thickness x width) beam from 3D point start to end (centre line)."""
	(x0, y0, z0), (x1, y1, z1) = start, end
	dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
	length = math.sqrt(dx * dx + dy * dy + dz * dz)
	if length < 0.05:
		raise ValueError("beam start and end are the same point")
	heading = math.degrees(math.atan2(dy, dx))
	elevation = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
	# local X along the beam: Ry(roll) tips +X down for positive roll, so roll = -elevation
	return build_box(service, (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2, length, width or thickness, thickness,
	                 heading=heading, roll=-elevation, texture=texture, color=color, dry_run=dry_run)


def build_ramp(service, low, high, width, *, thickness=0.18, texture=None, color=None, dry_run=False) -> dict:
	"""Sloped plate: `low` and `high` are the centres of its lower and upper
	edges on the TOP surface; width across the slope."""
	(x0, y0, z0), (x1, y1, z1) = low, high
	dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
	run = math.hypot(dx, dy)
	length = math.sqrt(run * run + dz * dz)
	heading = math.degrees(math.atan2(dy, dx))
	elevation = math.atan2(dz, run)
	# move the centre down along the plate normal so the top face passes through the edges
	n = (-math.sin(elevation) * dx / max(run, 1e-9), -math.sin(elevation) * dy / max(run, 1e-9), math.cos(elevation))
	c = [(x0 + x1) / 2 - n[0] * thickness / 2, (y0 + y1) / 2 - n[1] * thickness / 2, (z0 + z1) / 2 - n[2] * thickness / 2]
	return build_box(service, *c, length, width, thickness, heading=heading, roll=-math.degrees(elevation),
	                 texture=texture, color=color, kinds=THIN + ("block", "cube"), dry_run=dry_run)


def build_roof(service, cx, cy, z_eave, length, span, rise, *, heading=0.0, thickness=0.18, overhang=0.3,
               texture=None, color=None) -> dict:
	"""Gable roof over a length (local X) x span (local Y) rectangle: two slopes
	from the eaves at z_eave up to a ridge `rise` higher, overhanging by `overhang`."""
	frame = rotation(heading)
	half = span / 2 + overhang
	ids = []
	for side in (-1, 1):
		lo = _apply(frame, [0, side * half, 0])
		hi = _apply(frame, [0, 0, 0])
		drop = rise * overhang / (span / 2) if span > 0 else 0.0
		low = (cx + lo[0], cy + lo[1], z_eave - drop)
		high = (cx + hi[0], cy + hi[1], z_eave + rise)
		# a ramp's width runs across its slope: here the roof length
		r = build_ramp(service, low, high, length + 2 * overhang, thickness=thickness, texture=texture, color=color)
		ids += r["instance_ids"]
	return {"ok": True, "pieces": len(ids), "instance_ids": ids}


def build_column(service, cx, cy, z_base, radius, height, *, sides=8, thickness=0.18, texture=None, color=None) -> dict:
	"""Round column: a ring of `sides` panels (outer radius `radius`) from z_base up."""
	if sides < 3:
		raise ValueError("a column needs at least 3 sides")
	apothem = radius * math.cos(math.pi / sides)
	face = 2 * radius * math.sin(math.pi / sides) + 0.02     # a hair wider: closes the corner seams
	pieces = []
	for i in range(sides):
		a = 2 * math.pi * i / sides
		r = apothem - thickness / 2
		px, py = cx + r * math.cos(a), cy + r * math.sin(a)
		# panel width along the tangent (local X), thickness along the radius (local Y)
		pieces += box_pieces(px, py, z_base + height / 2, face, thickness, height,
		                     heading=math.degrees(a) + 90, kinds=THIN + ("block", "cube"))
	ids = place_pieces(service, pieces, texture=texture, color=color)
	return {"ok": True, "pieces": len(pieces), "instance_ids": ids}


def build_pole(service, start, end, *, texture=None, color=None, dry_run=False) -> dict:
	"""Round 0.19 m pole (stock windsock pole) along start -> end: posts,
	railings, pipes, ladder rails. The pole is 10.9 m long and can't be cut: a
	shorter pole ends exactly at `end` and its surplus runs on past `start` -
	put `start` in the ground or inside another block. Longer runs are tiled.
	The stock pole has a 0.24 m square sleeve on its bottom 5 m and a thin
	0.12 m shaft above: up to 5.9 m only the shaft shows (the sleeve is past
	`start`); longer poles show the sleeve as a collar at the `start` end - a
	footing for posts, odd for rails (keep rails <= 5.9 m)."""
	(x0, y0, z0), (x1, y1, z1) = start, end
	dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
	length = math.sqrt(dx * dx + dy * dy + dz * dz)
	if length < 0.05:
		raise ValueError("pole start and end are the same point")
	u = (dx / length, dy / length, dz / length)
	heading = math.degrees(math.atan2(dy, dx)) if math.hypot(dx, dy) > 1e-6 else 0.0
	elevation = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
	# Ry(roll) turns local +Z (the pole's length) toward +X: roll = 90 - elevation
	m = rotation(heading, 0.0, 90.0 - elevation)
	piece = BY_MODEL[16101]["size"][2]
	n = max(1, math.ceil((length - FIT_TOL) / piece - 1e-9))
	surplus = max(0.0, n * piece - length) if n == 1 else 0.0
	pieces = []
	# the shaft runs along the model's own Z axis (its box is off-centre: the top fitting
	# sticks out sideways), so anchor each piece by its origin, not its box centre
	c_local = _apply(m, BY_MODEL[16101]["centre"])
	for i in range(n):
		# piece i covers [a, a + piece] measured from start; the last one ends at `end`
		a = min(i * piece, length - piece) if n > 1 else length - piece
		base = (x0 + u[0] * a, y0 + u[1] * a, z0 + u[2] * a)
		pieces.append({"model": 16101, "centre": tuple(base[k] + c_local[k] for k in range(3)), "matrix": m})
	r = {"ok": True, "block": "des_windsockpole", "model": 16101, "pieces": n}
	if surplus:
		r["surplus_past_start"] = round(surplus, 3)
	if dry_run:
		return {**r, "dry_run": True}
	return {**r, "instance_ids": place_pieces(service, pieces, texture=texture, color=color)}
