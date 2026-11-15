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


def _visual_bottom(service, model):
	"""Lowest point that is actually drawn: the higher of the collision and mesh
	bottoms (some models' walls start well above their collision box)."""
	a = service.probe_asset(model, ensure_renderable=True)
	cb, rb = a.get("collision_bounds"), a.get("render_bounds")
	vals = [v for v in ((cb[0][2] if cb else None), (rb[0][2] if rb else None)) if v is not None]
	return max(vals) if vals else 0.0


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
                     dz=None, snap=True, keep_old=False, wire_poles=False) -> dict:
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
			fill_report = {}
			slabs = fill_ground(service, px, py, h, ob, ground, report=fill_report)
			fill_top = fill_report.get("top", ground - 0.12)
			z_fixed = fill_top - _visual_bottom(service, new_model) + 0.02 if dz is None else pz + dz
	op = {"action": "place", "key": f"replace:{uuid.uuid4().hex[:10]}", "model": new_model, "x": wx, "y": wy,
	      "z": z_fixed if z_fixed is not None else pz, "heading": (h + heading_offset) % 360,
	      "snap": snap and z_fixed is None}
	service.apply_scene_patch([op])
	placed = next((i for i in service.enumerate_scene() if i.get("object_key") == op["key"]), None)
	result = {"ok": True, "replaced": {"model_id": it["model_id"], "name": it.get("name"), "position": it["position"],
	                                   "heading": h}, "removed": removed, "ground_slabs": len(slabs), "placed": placed}
	if removed and ob:
		(ax, ay, az), (bx, by, _) = ob
		quad = [(px + lx * c - ly * s, py + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by))]
		ends = wire_ends(service, quad, pz + az)
		result["dangling_wire_ends"] = ends
		if ends and wire_poles:
			pb = _visual_bottom(service, WIRE_POLE)
			ground = fill_top if slabs else pz + az
			service.apply_scene_patch([{"action": "place", "key": f"wirepole:{uuid.uuid4().hex[:10]}", "model": WIRE_POLE,
			                            "x": e[0], "y": e[1], "z": ground - pb, "heading": 0.0, "snap": False} for e in ends])
			result["wire_poles"] = len(ends)
	return result


DETAIL_KEYS = ("alpha", "bit", "wire", "teline", "cable")
WIRE_POLE = 1308       # telgrphpole02, the wooden pole used along the desert towns' streets


def wire_ends(service, quad, ground_z, low=2.0, high=12.0, search=90.0) -> list[list[float]]:
	"""Wire / cable ends left hanging over a footprint: drawn vertices of nearby
	detail models (alpha bits, wire meshes) inside the footprint, 2-12 m up.
	A removed building's service drop is drawn in such shared detail models, so
	it can't be removed on its own - give its end something to land on."""
	xs = [p[0] for p in quad]; ys = [p[1] for p in quad]
	cx, cy = sum(xs) / 4, sum(ys) / 4
	page = service.engine("inspect_zone_page", [cx, cy, search, 0, 256])
	pts = []
	for it in page["items"]:
		if it.get("agent_scene") or not any(k in str(it.get("name", "")).lower() for k in DETAIL_KEYS):
			continue
		r = service.engine("mesh_points", [it["instance_id"], min(xs), min(ys), ground_z + low,
		                                   max(xs), max(ys), ground_z + high, 4000])
		for x, y, z in r.get("points", []):
			if _inside_quad(x, y, quad):
				pts.append((x, y, z))
	ends = []                                   # cluster ribbon vertices (same x,y, z and z+1)
	for x, y, z in sorted(pts):
		for e in ends:
			if math.hypot(e[0] - x, e[1] - y) < 0.8:
				e[2] = min(e[2], z); break
		else:
			ends.append([x, y, z])
	return [[round(e[0], 2), round(e[1], 2), round(e[2], 2)] for e in ends]


def _inside_quad(px, py, quad):
	sign = None
	for i in range(4):
		(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % 4]
		cr = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
		if abs(cr) < 1e-9: continue
		if sign is None: sign = cr > 0
		elif (cr > 0) != sign: return False
	return True


SLAB = 18981          # SA-MP Concrete1mx25mx25m: modelled upright (1 m thick along X) -> roll 90 to lie flat
GROUND_PLANES = {"sand": 19544, "grass": 19543, "concrete": 19545}   # flat 15 x 62.5 m planes (floor models for settle/lint)
# Lot fill: flat SA-MP panels tiled over the footprint only (the 62.5 m planes
# overhang the lot by up to 31 m and show through lower roads next to it),
# retextured with the stock ground planes' own textures.
GROUND_TILES = ((12.5, 19700, 0.125), (6.25, 19741, 0.0625))      # (size, model, half thickness)
GROUND_TEXTURES = {"sand": (19544, "beach_sfs", "ws_drysand"), "grass": (19543, "beach_sfs", "Grass_128HV"),
                   "concrete": (19545, "cs_ebridge", "pavea256")}


def _world_ground(service, x, y, z_from, depth=12.0):
	"""Height of the first world (non-placed) surface below (x, y, z_from), or None."""
	z = z_from
	for _ in range(8):
		hit = service.client.raycast_segment([x, y, z], [x, y, z_from - depth]).get("hit")
		if not hit:
			return None
		if not hit.get("agent_scene"):
			return hit["point"][2]
		z = hit["point"][2] - 0.02
	return None


def lot_rim(service, px, py, heading, bounds, ground_z, out=0.6, samples=9) -> dict:
	"""World ground height just outside each side of a footprint (median of a row
	of raycasts): what a lot fill has to meet - sidewalk, yard, road."""
	(ax, ay, _), (bx, by, _) = bounds
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	sides = {"-x": [(ax - out, ay + (by - ay) * (k + 0.5) / samples) for k in range(samples)],
	         "+x": [(bx + out, ay + (by - ay) * (k + 0.5) / samples) for k in range(samples)],
	         "-y": [(ax + (bx - ax) * (k + 0.5) / samples, ay - out) for k in range(samples)],
	         "+y": [(ax + (bx - ax) * (k + 0.5) / samples, by + out) for k in range(samples)]}
	rim = {}
	for side, pts in sides.items():
		zs = sorted(z for z in (_world_ground(service, px + lx * c - ly * s, py + lx * s + ly * c, ground_z + 6.0)
		                        for lx, ly in pts) if z is not None and abs(z - ground_z) < 1.5)
		rim[side] = zs[len(zs) // 2] if len(zs) >= 3 else None
	return rim


def fill_ground(service, px, py, heading, bounds, ground_z, surface="sand", sink=0.12, *, match_rim=True,
                tuck=0.5, report: dict | None = None) -> list[int]:
	"""Cover a building footprint whose ground went with it.

	Default: flat 12.5 m SA-MP panels (6.25 m for small lots) tiled over the
	footprint, retextured with the stock sand/grass/concrete ground texture.
	match_rim: the fill's top is set 2 cm under the surrounding ground measured
	just outside the lot (lot_rim), and the tiles slide `tuck` m under every
	side whose ground is at least that high (a sidewalk or yard), so no slit is
	left at the edge to see through. Sides that drop away (a lower road) get no
	overhang. Without a usable rim the top sits `sink` m below ground_z. The
	last row/column is pulled back flush with the edge (2 cm lower where it
	overlaps its neighbour). surface="slab" tiles tilted 25 m concrete slabs
	instead (renders dark in some lighting)."""
	(ax, ay, _), (bx, by, _) = bounds
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	if surface == "slab":
		return _fill_slabs(service, px, py, heading, bounds, ground_z)
	top = ground_z - sink
	if match_rim:
		rim = lot_rim(service, px, py, heading, bounds, ground_z)
		levels = sorted(v for v in rim.values() if v is not None)
		if levels:
			top = levels[len(levels) // 2] - 0.02
			ext = {k: (tuck if v is not None and v >= top - 0.03 else 0.0) for k, v in rim.items()}
			ax -= ext["-x"]; bx += ext["+x"]; ay -= ext["-y"]; by += ext["+y"]
			if report is not None:
				report.update({"rim": {k: (round(v, 3) if v is not None else None) for k, v in rim.items()},
				               "top": round(top, 3), "tucked_sides": [k for k, v in ext.items() if v]})
	tex_model, txd, texture = GROUND_TEXTURES.get(surface, GROUND_TEXTURES["sand"])
	w = bx - ax; d = by - ay
	size, model, half = GROUND_TILES[0] if min(w, d) >= GROUND_TILES[0][0] else GROUND_TILES[1]

	def axis(lo, hi):
		"""Tile centres from lo to hi; the last tile is pulled back flush with hi (overlapping its neighbour)."""
		n = max(1, math.ceil((hi - lo) / size - 1e-6))
		if n == 1:
			return [((lo + hi) / 2, False)]
		centres = [(lo + size / 2 + i * size, False) for i in range(n)]
		if centres[-1][0] + size / 2 > hi + 1e-6:
			centres[-1] = (hi - size / 2, True)
		return centres

	ops, keys = [], []
	for lx, shifted_x in axis(ax, bx):
		for ly, shifted_y in axis(ay, by):
			key = f"ground:{uuid.uuid4().hex[:10]}"
			keys.append(key)
			# overlapping tiles sit 2 cm lower so the two never z-fight
			z = top - half - (0.02 if (shifted_x or shifted_y) else 0.0)
			ops.append({"action": "place", "key": key, "model": model, "x": px + lx * c - ly * s,
			            "y": py + lx * s + ly * c, "z": z, "heading": heading % 360, "snap": False})
	service.apply_scene_patch(ops)
	scene = {i.get("object_key"): i for i in service.enumerate_scene()}
	ids = [scene[k]["instance_id"] for k in keys if k in scene]
	for iid in ids:   # same texture as the stock ground planes
		service.engine("set_material", [iid, 0, tex_model, txd, texture, "0"])
	return ids


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


def _ray(service, start, end):
	return service.client.raycast_segment(list(start), list(end), target_tolerance=0.0).get("hit")


def facade(service, x, y, model, *, side="auto", spacing=None, offset=0.3, levels="auto", level_height=None,
           heading_offset=0.0, margin=1.0, max_levels=6, road_mask=None, building_model=None,
           min_run=3, min_levels=2) -> dict:
	"""Dress a real wall of the building at (x, y) with `model` (scaffolding, rubble...).

	The building's collision box only tells which side to work on; the actual
	wall is probed per column with raycasts:
	  * a horizontal ray at chest height from outside must hit *this* building
	    (not a tree / fence / pole / nothing) - that gives the wall face here;
	  * a vertical ray just inside the face gives the roof height there.
	Columns are grouped into runs where the wall face is continuous; runs
	shorter than `min_run` bays are dropped. Each column is stacked up to the
	roof height at that spot (levels='auto'), so lower wings get shorter stacks
	and nothing towers above the roofline; walls lower than `min_levels` levels
	are skipped. levels=1 puts one row at the foot of the wall (rubble...).
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
	depth_box = max(bx - ax, by - ay)
	mb = _bounds(service, model)
	(max_, may_, maz), (mbx, mby, mbz) = mb if mb else ((-1, -1, 0), (1, 1, 2))
	width, depth = mbx - max_, mby - may_
	module_h = level_height or (mbz - maz)
	spacing = spacing or width
	ux, uy = (x2 - x1) / L, (y2 - y1) / L
	count = max(1, int((L - 2 * margin) // spacing))
	start = (L - count * spacing) / 2 + spacing / 2
	ground_z = pz + az
	probe_z = ground_z + 1.6
	columns = []                  # (t, face_distance_inward, ground_z_here, roof_z)
	for k in range(count):
		t = start + k * spacing
		ox, oy = x1 + ux * t + nx * 10.0, y1 + uy * t + ny * 10.0          # 10 m outside the box side
		far = 10.0 + depth_box
		hit = _ray(service, (ox, oy, probe_z), (ox - nx * far, oy - ny * far, probe_z))
		if not hit or hit.get("agent_scene") or hit.get("instance_id") != it["instance_id"]:
			columns.append(None); continue
		hx, hy, _ = hit["point"]
		d_in = (ox - hx) * nx + (oy - hy) * ny - 10.0                     # wall face, metres inside the box side
		if levels == "auto" or (isinstance(levels, int) and levels > 1):
			# the face must be at the same place one level up too: galleries,
			# porches and overhangs (open fronts) are not walls to scaffold
			up_z = probe_z + module_h
			hit2 = _ray(service, (ox, oy, up_z), (ox - nx * far, oy - ny * far, up_z))
			if not hit2 or hit2.get("instance_id") != it["instance_id"]:
				columns.append(None); continue
			d2 = (ox - hit2["point"][0]) * nx + (oy - hit2["point"][1]) * ny - 10.0
			if abs(d2 - d_in) > 0.5:
				columns.append(None); continue
		# roof height just inside the face
		rx, ry = hx - nx * 0.4, hy - ny * 0.4
		roof = _ray(service, (rx, ry, ground_z + 80.0), (rx, ry, ground_z - 5.0))
		roof_z = roof["point"][2] if roof and roof.get("instance_id") == it["instance_id"] else None
		# the scaffold's own spot must be open sky: no walkway, balcony or roof
		# overhang of this building above it (open galleries are not walls)
		fx, fy = hx + nx * (offset + depth / 2), hy + ny * (offset + depth / 2)
		over = _ray(service, (fx, fy, ground_z + 60.0), (fx, fy, ground_z + 0.3))
		if over and over.get("instance_id") == it["instance_id"]:
			columns.append(None); continue
		# ground in front of the face (world only)
		g = _ray(service, (fx, fy, probe_z + 1.0), (fx, fy, ground_z - 10.0))
		gz = g["point"][2] if g and not g.get("agent_scene") else ground_z
		columns.append((t, d_in, gz, roof_z))
	# continuous runs of wall
	runs, cur = [], []
	for col in columns:
		if col and cur and abs(col[1] - cur[-1][1]) <= 0.6:
			cur.append(col)
		else:
			if len(cur) >= min_run: runs.append(cur)
			cur = [col] if col else []
	if len(cur) >= min_run: runs.append(cur)
	heading = math.degrees(math.atan2(uy, ux)) + heading_offset
	tag = uuid.uuid4().hex[:8]
	ops, stacks = [], []
	for r_i, run in enumerate(runs):
		face = sorted(col[1] for col in run)[len(run) // 2]                 # one straight line per run
		for k, (t, d_in, gz, roof_z) in enumerate(run):
			if levels == "auto":
				if roof_z is None:
					continue
				n = int((roof_z - gz + 0.6) // module_h)                      # top may reach ~0.6 m above the roof edge
				n = min(max_levels, n)
				if n < min_levels:
					continue
			else:
				n = int(levels)
			wx = x1 + ux * t + nx * (offset + depth / 2 - face)
			wy = y1 + uy * t + ny * (offset + depth / 2 - face)
			key = f"facade:{tag}:{r_i:02d}:{k:03d}"
			ops.append({"action": "place", "key": key, "model": model, "x": wx, "y": wy,
			            "z": gz - maz + 0.02, "heading": heading % 360, "snap": False})
			stacks.append((key, wx, wy, gz - maz + 0.02, n))
	if ops:
		service.apply_scene_patch(ops)
	upper = []
	for key, wx, wy, z0, n in stacks:
		for lvl in range(1, n):
			upper.append({"action": "place", "key": f"{key}:{lvl}", "model": model, "x": wx, "y": wy,
			              "z": z0 + lvl * module_h, "heading": heading % 360, "snap": False})
	if upper:
		service.apply_scene_patch(upper)
	return {"ok": True, "building": {"name": it.get("name"), "model_id": it["model_id"]},
	        "side_normal": [round(nx, 2), round(ny, 2)], "side_length": round(L, 1),
	        "probed": count, "wall_columns": sum(1 for c_ in columns if c_), "runs": [len(r) for r in runs],
	        "stacks": len(stacks), "placed": len(ops) + len(upper)}


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
