"""World-building helpers for agent mapping.

* find_world_building(x, y)      - the world (non-agent) building nearest a point
* replace_building(...)          - suppress a world building (and its LOD, so the
                                   intact version doesn't show from far away) and
                                   place a replacement at the same transform
* facade(...)                    - line one side of a building with repeated pieces
                                   (scaffolding stacked to roof height, rubble and
                                   broken-wall chunks at the base...)
* size_match(...)                - catalogue models with a similar footprint
"""

from __future__ import annotations

import math
import uuid


def _bounds(service, model):
	return service.probe_asset(model, ensure_renderable=True).get("collision_bounds")


def find_world_building(service, x, y, radius=30.0, model=None):
	"""Nearest non-agent, non-LOD instance whose footprint contains / is closest to (x, y)."""
	best = None
	offset = 0
	while offset is not None:
		page = service.engine("inspect_zone_page", [x, y, radius, offset, 256])
		for it in page["items"]:
			if it.get("agent_scene") or str(it.get("name", "")).lower().startswith("lod"):
				continue
			if model is not None and it["model_id"] != model:
				continue
			b = _bounds(service, it["model_id"])
			if not b:
				continue
			(ax, ay, _), (bx, by, _) = b
			area = (bx - ax) * (by - ay)
			name = str(it.get("name", "")).lower()
			if model is None and (area < 20 or area > 6000 or bx - ax > 90 or by - ay > 90
			                      or any(k in name for k in ("bit", "road", "ground", "grnd", "land", "terrain"))):
				continue                              # props, and terrain tiles / roads that contain everything
			# distance from (x, y) to the footprint (0 inside)
			h = math.radians(it["rotation"][2]); c, s = math.cos(h), math.sin(h)
			dx, dy = x - it["position"][0], y - it["position"][1]
			lx, ly = dx * c + dy * s, -dx * s + dy * c
			ex = max(ax - lx, 0, lx - bx); ey = max(ay - ly, 0, ly - by)
			d = math.hypot(ex, ey)
			if best is None or (d, area) < (best[0], best[1]):     # the smallest building at the point
				best = (d, area, it)
		offset = page["page"]["next_offset"]
	return best[2] if best else None


def _lod_instances(service, it):
	"""World LOD instances standing at the same spot as `it` (big low-detail stand-ins)."""
	x, y, _ = it["position"]
	page = service.engine("inspect_zone_page", [x, y, 3.0, 0, 256])
	return [i for i in page["items"] if not i.get("agent_scene") and str(i.get("name", "")).lower().startswith("lod")
	        and math.hypot(i["position"][0] - x, i["position"][1] - y) < 2.0]


def suppress_building(service, it) -> dict:
	"""Suppress a world building plus its LOD at the same spot (else the intact
	building still shows from far away)."""
	x, y, _ = it["position"]
	lods = _lod_instances(service, it)
	models = [it["model_id"]] + [l["model_id"] for l in lods]
	result = service.suppress_native_models(x, y, 1.5, sorted(set(models)))
	return {"suppressed": [s["model_id"] for s in result.get("suppressed", [])],
	        "lod_models": [l["model_id"] for l in lods]}


def replace_building(service, x, y, new_model, *, old_model=None, heading_offset=0.0, dx=0.0, dy=0.0,
                     dz=None, snap=True, keep_old=False) -> dict:
	it = find_world_building(service, x, y, model=old_model)
	if not it:
		raise ValueError("no world building found there")
	px, py, pz = it["position"]
	h = it["rotation"][2]
	c, s = math.cos(math.radians(h)), math.sin(math.radians(h))
	# line up the footprint centres (models put their origin in different places)
	ob, nb = _bounds(service, it["model_id"]), _bounds(service, new_model)
	if ob and nb:
		ocx, ocy = (ob[0][0] + ob[1][0]) / 2, (ob[0][1] + ob[1][1]) / 2
		hr = math.radians(heading_offset); cr, sr = math.cos(hr), math.sin(hr)
		ncx, ncy = (nb[0][0] + nb[1][0]) / 2, (nb[0][1] + nb[1][1]) / 2
		ncx, ncy = ncx * cr - ncy * sr, ncx * sr + ncy * cr      # new centre in the old building's frame
		dx += ocx - ncx; dy += ocy - ncy
	wx, wy = px + dx * c - dy * s, py + dx * s + dy * c
	removed = None if keep_old else suppress_building(service, it)
	slabs = []
	z_fixed = None if dz is None else pz + dz
	if removed and ob:
		# Some buildings (e.g. des_bighus houses) carry their own lot as ground:
		# removing them leaves a hole. Detect it and lay concrete slabs level with
		# the old building's base so the replacement has something to stand on.
		(ax, ay, az), (bx, by, _) = ob
		probes = [((ax + bx) / 2, (ay + by) / 2)] + [(ax + (bx - ax) * fx, ay + (by - ay) * fy)
		                                             for fx in (0.2, 0.8) for fy in (0.2, 0.8)]
		misses = 0
		for lx, ly in probes:
			gx, gy = px + lx * c - ly * s, py + lx * s + ly * c
			hit = service.client.raycast_segment([gx, gy, pz + 60.0], [gx, gy, pz - 60.0]).get("hit")
			if not hit:
				misses += 1
		if misses >= 3:
			ground = pz + az
			slabs = fill_ground(service, px, py, h, ob, ground)
			nb_ = nb or ((0, 0, 0), (0, 0, 0))
			z_fixed = ground - nb_[0][2] + 0.02 if dz is None else pz + dz
	op = {"action": "place", "key": f"replace:{uuid.uuid4().hex[:10]}", "model": new_model, "x": wx, "y": wy,
	      "z": z_fixed if z_fixed is not None else pz, "heading": (h + heading_offset) % 360,
	      "snap": snap and z_fixed is None}
	service.apply_scene_patch([op])
	placed = next((i for i in service.enumerate_scene() if i.get("object_key") == op["key"]), None)
	return {"ok": True, "replaced": {"model_id": it["model_id"], "name": it.get("name"), "position": it["position"],
	                                 "heading": h}, "removed": removed, "ground_slabs": len(slabs), "placed": placed}


SLAB = 18981          # SA-MP Concrete1mx25mx25m: modelled upright (1 m thick along X) -> roll 90 to lie flat
GROUND_PLANES = {"sand": 19544, "grass": 19543, "concrete": 19545}   # flat 15 x 62.5 m planes


def fill_ground(service, px, py, heading, bounds, ground_z, surface="sand", sink=0.12) -> list[int]:
	"""Cover a building footprint whose ground went with it.

	Default: flat 15 x 62.5 m sand/grass/concrete planes laid side by side a few
	cm below the old ground level - inside the hole they read as a cleared lot,
	elsewhere they sit hidden under the existing ground. surface="slab" tiles
	tilted 25 m concrete slabs instead (renders dark in some lighting)."""
	(ax, ay, _), (bx, by, _) = bounds
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	if surface == "slab":
		return _fill_slabs(service, px, py, heading, bounds, ground_z)
	model = GROUND_PLANES.get(surface, 19544)
	w = bx - ax; d = by - ay
	# planes are 15 wide (local X) and 62.5 long (local Y): lay strips across the longer side
	along_y = d >= w
	span = w if along_y else d
	strips = max(1, math.ceil(span / 15.0))
	ops, keys = [], []
	for i in range(strips):
		t = (i + 0.5) / strips
		lx, ly = (ax + w * t, (ay + by) / 2) if along_y else ((ax + bx) / 2, ay + d * t)
		hh = heading if along_y else heading + 90
		key = f"ground:{uuid.uuid4().hex[:10]}"
		keys.append(key)
		ops.append({"action": "place", "key": key, "model": model, "x": px + lx * c - ly * s,
		            "y": py + lx * s + ly * c, "z": ground_z - sink, "heading": hh % 360, "snap": False})
	service.apply_scene_patch(ops)
	scene = {i.get("object_key"): i for i in service.enumerate_scene()}
	return [scene[k]["instance_id"] for k in keys if k in scene]


def _fill_slabs(service, px, py, heading, bounds, ground_z, slab=SLAB, size=25.0) -> list[int]:
	"""Tile flat 25 m concrete slabs over a building footprint, top face at ground_z."""
	(ax, ay, _), (bx, by, _) = bounds
	nx_, ny_ = max(1, math.ceil((bx - ax - 0.5) / size)), max(1, math.ceil((by - ay - 0.5) / size))
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	ops, keys = [], []
	for i in range(nx_):
		for j in range(ny_):
			lx = ax + (bx - ax) * (i + 0.5) / nx_
			ly = ay + (by - ay) * (j + 0.5) / ny_
			key = f"slab:{uuid.uuid4().hex[:10]}"
			keys.append(key)
			ops.append({"action": "place", "key": key, "model": slab, "x": px + lx * c - ly * s,
			            "y": py + lx * s + ly * c, "z": ground_z - 0.5, "heading": heading % 360, "snap": False})
	service.apply_scene_patch(ops)
	scene = {i.get("object_key"): i for i in service.enumerate_scene()}
	ids = []
	tilt = []
	for key in keys:
		it = scene.get(key)
		if not it:
			continue
		ids.append(it["instance_id"])
		x, y, z = it["position"]
		tilt.append({"action": "transform3d", "instance_id": it["instance_id"], "x": x, "y": y, "z": z,
		             "pitch": 0.0, "roll": 90.0, "heading": heading % 360, "snap": False})
	if tilt:
		service.apply_scene_patch(tilt)
	return ids


def _sides(quad):
	"""Footprint edges as (start, end, outward normal, length)."""
	cx = sum(p[0] for p in quad) / 4; cy = sum(p[1] for p in quad) / 4
	out = []
	for i in range(4):
		(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % 4]
		L = math.hypot(x2 - x1, y2 - y1) or 1.0
		nx, ny = (y2 - y1) / L, -(x2 - x1) / L
		mx, my = (x1 + x2) / 2, (y1 + y2) / 2
		if (mx - cx) * nx + (my - cy) * ny < 0:
			nx, ny = -nx, -ny
		out.append(((x1, y1), (x2, y2), (nx, ny), L))
	return out


COMPASS = {"N": (0, 1), "S": (0, -1), "E": (1, 0), "W": (-1, 0)}


def facade(service, x, y, model, *, side="auto", spacing=None, offset=0.8, levels="auto", level_height=None,
           heading_offset=0.0, margin=1.0, max_levels=6, road_mask=None, building_model=None) -> dict:
	"""Line one side of the building at (x, y) with `model`.

	side: auto (the side facing the nearest road / open space), N/S/E/W, or 0-3.
	levels: 'auto' stacks up to the building's collision height, or an int.
	The first level is snapped to the ground; upper levels sit exactly on top.
	"""
	it = find_world_building(service, x, y, model=building_model)
	if not it:
		raise ValueError("no world building found there")
	b = _bounds(service, it["model_id"])
	(ax, ay, az), (bx, by, bz) = b
	h = math.radians(it["rotation"][2]); c, s = math.cos(h), math.sin(h)
	px, py, pz = it["position"]
	quad = [(px + lx * c - ly * s, py + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by))]
	sides = _sides(quad)
	if side == "auto":
		def openness(sd):
			(x1, y1), (x2, y2), (nx, ny), L = sd
			mx, my = (x1 + x2) / 2 + nx * 6, (y1 + y2) / 2 + ny * 6
			if road_mask is not None:
				return -road_mask.distance(mx, my) + L * 0.05
			return L
		chosen = max(sides, key=openness)
	elif side.upper() in COMPASS:
		vx, vy = COMPASS[side.upper()]
		chosen = max(sides, key=lambda sd: sd[2][0] * vx + sd[2][1] * vy)
	else:
		chosen = sides[int(side)]
	(x1, y1), (x2, y2), (nx, ny), L = chosen
	mb = _bounds(service, model)
	(max_, may_, maz), (mbx, mby, mbz) = mb if mb else ((-1, -1, 0), (1, 1, 2))
	width = mbx - max_
	depth = mby - may_
	module_h = level_height or (mbz - maz)
	spacing = spacing or width
	ux, uy = (x2 - x1) / L, (y2 - y1) / L
	count = max(1, int((L - 2 * margin) // spacing))
	start = (L - count * spacing) / 2 + spacing / 2
	building_h = (bz - az)
	n_levels = levels if isinstance(levels, int) else max(1, min(max_levels, int((building_h - 0.5) // module_h)))
	heading = math.degrees(math.atan2(uy, ux)) + heading_offset
	tag = uuid.uuid4().hex[:8]
	ground_ops, keys = [], []
	for k in range(count):
		t = start + k * spacing
		wx = x1 + ux * t + nx * (offset + depth / 2)
		wy = y1 + uy * t + ny * (offset + depth / 2)
		key = f"facade:{tag}:{k:03d}:0"
		keys.append((key, wx, wy))
		ground_ops.append({"action": "place", "key": key, "model": model, "x": wx, "y": wy, "z": pz, "heading": heading % 360, "snap": True})
	service.apply_scene_patch(ground_ops)
	scene = {i.get("object_key"): i for i in service.enumerate_scene()}
	upper = []
	for key, wx, wy in keys:
		base = scene.get(key)
		if not base:
			continue
		for lvl in range(1, n_levels):
			upper.append({"action": "place", "key": key[:-1] + str(lvl), "model": model, "x": wx, "y": wy,
			              "z": base["position"][2] + lvl * module_h, "heading": heading % 360, "snap": False})
	if upper:
		service.apply_scene_patch(upper)
	return {"ok": True, "building": {"name": it.get("name"), "model_id": it["model_id"], "height": round(building_h, 1)},
	        "side_normal": [round(nx, 2), round(ny, 2)], "side_length": round(L, 1), "columns": count, "levels": n_levels,
	        "placed": len(ground_ops) + len(upper)}


def size_match(index, width, depth, *, query="", tolerance=0.25, limit=20, include_hazards=False) -> list[dict]:
	"""Catalogue models whose footprint is within `tolerance` (fraction) of width x depth (either orientation)."""
	results = index.search(query, limit=3000, include_hazards=include_hazards) if query else index.catalog()
	out = []
	for a in results:
		w, d = a.get("width"), a.get("depth")
		if not w or not d:
			continue
		for ww, dd, rot in ((w, d, 0), (d, w, 90)):
			err = max(abs(ww - width) / width, abs(dd - depth) / depth)
			if err <= tolerance:
				out.append({"id": a["id"], "name": a["name"], "width": w, "depth": d, "height": a.get("height"),
				            "rotate": rot, "error": round(err, 3)})
				break
	out.sort(key=lambda r: r["error"])
	return out[:limit]
