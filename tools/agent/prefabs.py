"""Prefabs: reusable multi-part structures (a gatehouse, a workshop, a campfire ring).

A prefab is JSON in <state>/prefabs/<name>.json:
    {"name": "...", "parts": [
        {"model": 2934, "dx": 0.0, "dy": 0.0, "heading": 90.0, "on": null},
        {"model": 2932, "dx": 0.0, "dy": 0.0, "heading": 90.0, "on": 0, "dz": 2.9},
        ...]}
Offsets are in the prefab frame: +X points along the placement heading
(degrees from world +X, counter-clockwise), +Y to its left. Parts with
"on": null are snapped to the ground; parts with "on": i are stacked on part i
at "dz" above that part's origin (a container on a container, a hut on a post).

``save`` builds a prefab from placed instances (riders are detected from
footprints and heights); ``place`` puts one down with one patch for the ground
parts and one for everything stacked on them.
"""

from __future__ import annotations

import json
import math
import uuid
from pathlib import Path


BUNDLED_DIR = Path(__file__).resolve().parent / "prefab_library"


def prefab_dir(service) -> Path:
	return Path(service.state_dir) / "prefabs"


def list_prefabs(service) -> list[dict]:
	"""Saved prefabs (<state>/prefabs) plus the library bundled with the fork;
	a saved prefab with the same name wins."""
	out, seen = [], set()
	for source, folder in (("saved", prefab_dir(service)), ("bundled", BUNDLED_DIR)):
		for path in sorted(folder.glob("*.json")):
			try:
				data = json.loads(path.read_text(encoding="utf-8"))
			except ValueError:
				continue
			name = data.get("name", path.stem)
			if name in seen:
				continue
			seen.add(name)
			out.append({"name": name, "parts": len(data.get("parts", [])), "source": source,
			            "notes": data.get("notes", "")})
	return out


def load_prefab(service, name_or_path: str) -> dict:
	path = Path(name_or_path)
	if not path.suffix:
		path = prefab_dir(service) / f"{name_or_path}.json"
		if not path.exists():
			path = BUNDLED_DIR / f"{name_or_path}.json"
	return json.loads(path.read_text(encoding="utf-8"))


def _footprint(bounds, x, y, heading):
	(ax, ay, _), (bx, by, _) = bounds
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	return [(x + lx * c - ly * s, y + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by))]


def _inside(px, py, quad) -> bool:
	sign = None
	for i in range(4):
		(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % 4]
		cross = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
		if abs(cross) < 1e-9:
			continue
		if sign is None:
			sign = cross > 0
		elif (cross > 0) != sign:
			return False
	return True


def _ground_z(service, x, y, z_from=600.0):
	"""World ground (not placed objects) under (x, y), or None."""
	z = z_from
	for _ in range(12):
		hit = service.client.raycast_segment([x, y, z], [x, y, -100.0]).get("hit")
		if not hit:
			return None
		if not hit.get("agent_scene"):
			return hit["point"][2]
		z = hit["point"][2] - 0.02
	return None


def save_prefab(service, name: str, instance_ids: list[int], origin: tuple[float, float, float] | None = None,
                notes: str = "", *, rigid: bool = False, origin_z: float | None = None) -> dict:
	"""rigid=True for objects composed from building blocks: every part keeps
	its exact 3D offset (z relative to the ground under the origin, or to
	origin_z) instead of being snapped to the terrain on its own."""
	scene = {int(i["instance_id"]): i for i in service.enumerate_scene()}
	items = [scene[i] for i in instance_ids if i in scene]
	if not items:
		raise ValueError("none of the instance ids are in the scene")
	if origin is None:
		first = items[0]
		origin = (first["position"][0], first["position"][1], first["rotation"][2])
	ox, oy, oh = origin
	h = math.radians(oh); c, s = math.cos(h), math.sin(h)
	if rigid and origin_z is None:
		origin_z = _ground_z(service, ox, oy)
		if origin_z is None:
			raise ValueError("no ground under the prefab origin: pass origin_z")
	items.sort(key=lambda i: i["position"][2])
	bounds = {}
	for it in items:
		if it["model_id"] not in bounds:
			bounds[it["model_id"]] = service.probe_asset(it["model_id"], ensure_renderable=True).get("collision_bounds")
	parts = []
	for idx, it in enumerate(items):
		x, y, z = it["position"]
		dx, dy = x - ox, y - oy
		part = {"model": it["model_id"], "dx": round(dx * c + dy * s, 4), "dy": round(-dx * s + dy * c, 4),
		        "heading": round((it["rotation"][2] - oh) % 360, 3), "on": None}
		pitch, roll = float(it["rotation"][0]), float(it["rotation"][1])
		if abs(pitch) > 1e-3 or abs(roll) > 1e-3:
			part["pitch"], part["roll"] = round(pitch, 3), round(roll, 3)
		if it.get("materials"):
			part["materials"] = it["materials"]
		if rigid:
			part["z"] = round(z - origin_z, 4)
			parts.append(part)
			continue
		for j in range(idx):                       # resting on an earlier (lower) part?
			base = items[j]
			b = bounds.get(base["model_id"])
			if b and z > base["position"][2] + 0.5 and _inside(x, y, _footprint(b, base["position"][0], base["position"][1], base["rotation"][2])):
				part["on"] = j
				part["dz"] = round(z - base["position"][2], 3)
		parts.append(part)
	data = {"name": name, "notes": notes, "parts": parts}
	if rigid:
		data["rigid"] = True
	prefab_dir(service).mkdir(parents=True, exist_ok=True)
	path = prefab_dir(service) / f"{name}.json"
	path.write_text(json.dumps(data, indent=1), encoding="utf-8")
	return {"ok": True, "path": str(path), "parts": len(parts), "stacked": sum(p["on"] is not None for p in parts)}


def place_prefab(service, prefab: dict, x: float, y: float, heading: float, *, snap: bool = True,
                 base_z: float | None = None) -> dict:
	"""base_z: put ground parts with their bottom exactly at this height instead of
	snapping (snapping only finds world ground, not placed slabs/floors).
	Rigid prefabs (built from blocks) are placed as one piece on the ground
	under (x, y) - or at base_z - with every part's 3D offset and tilt kept."""
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	parts = prefab["parts"]
	tag = uuid.uuid4().hex[:8]
	keys = [f"prefab:{prefab.get('name', 'p')}:{tag}:{i:03d}" for i in range(len(parts))]
	def world(p):
		return x + p["dx"] * c - p["dy"] * s, y + p["dx"] * s + p["dy"] * c, (heading + p.get("heading", 0.0)) % 360
	if prefab.get("rigid"):
		return _place_rigid(service, prefab, keys, world, x, y, base_z)
	if base_z is not None:
		snap = False
	ground = [i for i, p in enumerate(parts) if p.get("on") is None]
	ops = []
	for i in ground:
		wx, wy, wh = world(parts[i])
		z = parts[i].get("z", 0.0)
		if base_z is not None:
			b = service.probe_asset(parts[i]["model"], ensure_renderable=True).get("collision_bounds")
			z = base_z - (b[0][2] if b else 0.0) + 0.02 - float(parts[i].get("z_sink", 0.0))
		ops.append({"action": "place", "key": keys[i], "model": parts[i]["model"], "x": wx, "y": wy,
		            "z": z, "heading": wh, "snap": snap})
	service.apply_scene_patch(ops)
	placed = {}
	by_key = {}
	for item in service.enumerate_scene():
		by_key[item.get("object_key")] = item
	for i in ground:
		placed[i] = by_key.get(keys[i])
	# stacked parts, level by level (a lamp on a container on a container...)
	remaining = [i for i, p in enumerate(parts) if p.get("on") is not None]
	while remaining:
		level = [i for i in remaining if parts[i]["on"] in placed and placed[parts[i]["on"]]]
		if not level:
			break
		ops = []
		for i in level:
			base = placed[parts[i]["on"]]
			wx, wy, wh = world(parts[i])
			ops.append({"action": "place", "key": keys[i], "model": parts[i]["model"], "x": wx, "y": wy,
			            "z": base["position"][2] + parts[i].get("dz", 0.0), "heading": wh, "snap": False})
		service.apply_scene_patch(ops)
		for item in service.enumerate_scene():
			if item.get("object_key") in keys:
				by_key[item["object_key"]] = item
		for i in level:
			placed[i] = by_key.get(keys[i])
		remaining = [i for i in remaining if i not in level]
	_apply_tilts(service, parts, placed)
	for i, item in placed.items():
		if item and parts[i].get("materials"):
			try:
				from . import materials as _materials
			except ImportError:
				import materials as _materials
			_materials.apply_materials(service, item["instance_id"], parts[i]["materials"])
	ids = [placed[i]["instance_id"] for i in sorted(placed) if placed[i]]
	return {"ok": True, "prefab": prefab.get("name"), "placed": len(ids), "instance_ids": ids,
	        "unplaced_stacked": len(remaining)}


def _apply_tilts(service, parts, placed):
	"""Parts saved with pitch/roll: tilt them in place (placing only takes a heading)."""
	ops = []
	for i, item in placed.items():
		p = parts[i]
		if item and (p.get("pitch") or p.get("roll")):
			x, y, z = item["position"]
			ops.append({"action": "transform3d", "instance_id": item["instance_id"], "x": x, "y": y, "z": z,
			            "pitch": p.get("pitch", 0.0), "roll": p.get("roll", 0.0), "heading": item["rotation"][2],
			            "snap": False})
	for k in range(0, len(ops), 300):
		service.apply_scene_patch(ops[k:k + 300])


def _place_rigid(service, prefab, keys, world, x, y, base_z):
	parts = prefab["parts"]
	gz = base_z if base_z is not None else _ground_z(service, x, y)
	if gz is None:
		raise ValueError("no ground under the placement point: pass base_z")
	ops = []
	for i, p in enumerate(parts):
		wx, wy, wh = world(p)
		ops.append({"action": "place", "key": keys[i], "model": p["model"], "x": wx, "y": wy,
		            "z": gz + p.get("z", 0.0), "heading": wh, "snap": False})
	for k in range(0, len(ops), 300):
		service.apply_scene_patch(ops[k:k + 300])
	by_key = {item.get("object_key"): item for item in service.enumerate_scene()}
	placed = {i: by_key.get(keys[i]) for i in range(len(parts))}
	_apply_tilts(service, parts, placed)
	try:
		from . import materials as _materials
	except ImportError:
		import materials as _materials
	for i, item in placed.items():
		if item and parts[i].get("materials"):
			_materials.apply_materials(service, item["instance_id"], parts[i]["materials"])
	ids = [placed[i]["instance_id"] for i in sorted(placed) if placed[i]]
	return {"ok": True, "prefab": prefab.get("name"), "placed": len(ids), "instance_ids": ids, "rigid": True,
	        "ground_z": round(gz, 3)}
