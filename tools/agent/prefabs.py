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


def prefab_dir(service) -> Path:
	return Path(service.state_dir) / "prefabs"


def list_prefabs(service) -> list[dict]:
	out = []
	for path in sorted(prefab_dir(service).glob("*.json")):
		try:
			data = json.loads(path.read_text(encoding="utf-8"))
			out.append({"name": data.get("name", path.stem), "parts": len(data.get("parts", [])),
			            "notes": data.get("notes", "")})
		except ValueError:
			pass
	return out


def load_prefab(service, name_or_path: str) -> dict:
	path = Path(name_or_path)
	if not path.suffix:
		path = prefab_dir(service) / f"{name_or_path}.json"
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


def save_prefab(service, name: str, instance_ids: list[int], origin: tuple[float, float, float] | None = None,
                notes: str = "") -> dict:
	scene = {int(i["instance_id"]): i for i in service.enumerate_scene()}
	items = [scene[i] for i in instance_ids if i in scene]
	if not items:
		raise ValueError("none of the instance ids are in the scene")
	if origin is None:
		first = items[0]
		origin = (first["position"][0], first["position"][1], first["rotation"][2])
	ox, oy, oh = origin
	h = math.radians(oh); c, s = math.cos(h), math.sin(h)
	items.sort(key=lambda i: i["position"][2])
	bounds = {}
	for it in items:
		if it["model_id"] not in bounds:
			bounds[it["model_id"]] = service.probe_asset(it["model_id"], ensure_renderable=True).get("collision_bounds")
	parts = []
	for idx, it in enumerate(items):
		x, y, z = it["position"]
		dx, dy = x - ox, y - oy
		part = {"model": it["model_id"], "dx": round(dx * c + dy * s, 3), "dy": round(-dx * s + dy * c, 3),
		        "heading": round((it["rotation"][2] - oh) % 360, 2), "on": None}
		for j in range(idx):                       # resting on an earlier (lower) part?
			base = items[j]
			b = bounds.get(base["model_id"])
			if b and z > base["position"][2] + 0.5 and _inside(x, y, _footprint(b, base["position"][0], base["position"][1], base["rotation"][2])):
				part["on"] = j
				part["dz"] = round(z - base["position"][2], 3)
		parts.append(part)
	data = {"name": name, "notes": notes, "parts": parts}
	prefab_dir(service).mkdir(parents=True, exist_ok=True)
	path = prefab_dir(service) / f"{name}.json"
	path.write_text(json.dumps(data, indent=1), encoding="utf-8")
	return {"ok": True, "path": str(path), "parts": len(parts), "stacked": sum(p["on"] is not None for p in parts)}


def place_prefab(service, prefab: dict, x: float, y: float, heading: float, *, snap: bool = True,
                 base_z: float | None = None) -> dict:
	"""base_z: put ground parts with their bottom exactly at this height instead of
	snapping (snapping only finds world ground, not placed slabs/floors)."""
	if base_z is not None:
		snap = False
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	parts = prefab["parts"]
	tag = uuid.uuid4().hex[:8]
	keys = [f"prefab:{prefab.get('name', 'p')}:{tag}:{i:03d}" for i in range(len(parts))]
	def world(p):
		return x + p["dx"] * c - p["dy"] * s, y + p["dx"] * s + p["dy"] * c, (heading + p.get("heading", 0.0)) % 360
	ground = [i for i, p in enumerate(parts) if p.get("on") is None]
	ops = []
	for i in ground:
		wx, wy, wh = world(parts[i])
		z = parts[i].get("z", 0.0)
		if base_z is not None:
			b = service.probe_asset(parts[i]["model"], ensure_renderable=True).get("collision_bounds")
			z = base_z - (b[0][2] if b else 0.0) + 0.02
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
	ids = [placed[i]["instance_id"] for i in sorted(placed) if placed[i]]
	return {"ok": True, "prefab": prefab.get("name"), "placed": len(ids), "instance_ids": ids,
	        "unplaced_stacked": len(remaining)}
