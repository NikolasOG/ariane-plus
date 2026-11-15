"""Mapping toolkit shared by arianectl and the MCP server.

Higher-level operations an agent needs when building real maps, on top of
the scene patch API:

  patch_in_chunks   apply hundreds of simple operations in a few round trips
  settle            dig placed objects into the terrain (stack aware, visual mode)
  world_items       dump world (native) objects over a rectangle
  wall_run          lay a continuous wall/fence along a polyline: pieces end to
                    end at their real length, closed corners, gate openings with
                    container posts, stacked levels
  pack              fill a zone with structures on free ground: avoids world
                    buildings, placed objects, dirt tracks and roads, aligns to
                    the nearest track (street frontage)
  plan_map          top-down plan PNG: world footprints, placed objects, tracks,
                    roads, optional lint issues

Geometry helpers work on 2D footprints from collision bounds (model space,
rotated by heading). See MAPPING_TOOLKIT.md for the workflow.
"""

from __future__ import annotations

import json
import math
import random
import uuid
from pathlib import Path

try:
	from . import ground as ground_mod
except ImportError:  # run as a script
	import ground as ground_mod


# ------------------------------------------------------------------ geometry
def footprint(bounds, x, y, heading, grow=0.0):
	(ax, ay, _), (bx, by, _) = bounds
	h = math.radians(heading); c, s = math.cos(h), math.sin(h)
	return [(x + lx * c - ly * s, y + lx * s + ly * c)
	        for lx, ly in ((ax - grow, ay - grow), (bx + grow, ay - grow), (bx + grow, by + grow), (ax - grow, by + grow))]


def quad_area(q):
	return abs(sum(q[i][0] * q[(i + 1) % len(q)][1] - q[(i + 1) % len(q)][0] * q[i][1] for i in range(len(q)))) / 2


def point_in_convex(px, py, quad):
	sign = None
	for i in range(len(quad)):
		(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % len(quad)]
		cross = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
		if abs(cross) < 1e-9: continue
		if sign is None: sign = cross > 0
		elif (cross > 0) != sign: return False
	return True


def point_in_poly(px, py, poly):
	inside = False
	for i in range(len(poly)):
		x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % len(poly)]
		if (y1 > py) != (y2 > py) and px < x1 + (py - y1) * (x2 - x1) / (y2 - y1):
			inside = not inside
	return inside


def sat_overlap(p, q, margin=0.0):
	for poly in (p, q):
		for i in range(len(poly)):
			x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % len(poly)]
			nx, ny = y1 - y2, x2 - x1
			L = math.hypot(nx, ny) or 1
			nx, ny = nx / L, ny / L
			pa = [nx * x + ny * y for x, y in p]; qa = [nx * x + ny * y for x, y in q]
			if max(pa) + margin <= min(qa) or max(qa) + margin <= min(pa):
				return False
	return True


def seg_dist(px, py, ax, ay, bx, by):
	dx, dy = bx - ax, by - ay
	L2 = dx * dx + dy * dy
	t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
	return math.hypot(ax + t * dx - px, ay + t * dy - py)


class Bounds:
	"""Cached collision / render bounds per model."""

	def __init__(self, service):
		self.service = service
		self.cache: dict = {}

	def asset(self, model):
		if model not in self.cache:
			try:
				self.cache[model] = self.service.probe_asset(model, ensure_renderable=True)
			except Exception:
				self.cache[model] = {}
		return self.cache[model]

	def collision(self, model):
		return self.asset(model).get("collision_bounds")

	def visual_bottom(self, model):
		a = self.asset(model)
		cb, rb = a.get("collision_bounds"), a.get("render_bounds")
		vals = [v for v in ((cb[0][2] if cb else None), (rb[0][2] if rb else None)) if v is not None]
		return max(vals) if vals else 0.0


# ------------------------------------------------------------------ bulk edits
def patch_in_chunks(service, ops: list[dict], size: int = 400) -> dict:
	"""Apply many simple operations as a few patches (one bridge round trip each)."""
	done = 0
	for i in range(0, len(ops), size):
		service.apply_scene_patch(ops[i:i + size])
		done += len(ops[i:i + size])
	return {"ok": True, "applied": done}


def delete_objects(service, instance_ids: list[int]) -> dict:
	return patch_in_chunks(service, [{"action": "delete", "instance_id": int(i)} for i in instance_ids])


def transform_objects(service, rows: list[dict]) -> dict:
	"""rows: {instance_id, x, y, z, heading[, snap]}"""
	return patch_in_chunks(service, [{"action": "transform", "instance_id": int(r["instance_id"]), "x": r["x"], "y": r["y"],
	                                  "z": r["z"], "heading": r["heading"], "snap": bool(r.get("snap", False))} for r in rows])


def place_rows(service, rows: list[dict], key_prefix: str = "place") -> list[dict]:
	"""rows: {model, x, y, z, heading, snap} -> placed scene items (in order)."""
	tag = uuid.uuid4().hex[:8]
	ops = []
	for k, r in enumerate(rows):
		ops.append({"action": "place", "key": f"{key_prefix}:{tag}:{k:04d}", "model": r["model"], "x": r["x"], "y": r["y"],
		            "z": r.get("z", 0.0), "heading": r.get("heading", 0.0) % 360, "snap": bool(r.get("snap", True))})
	patch_in_chunks(service, ops)
	scene = {i.get("object_key"): i for i in service.enumerate_scene()}
	return [scene.get(op["key"]) for op in ops]


# ------------------------------------------------------------------ settle
def settle(service, client, *, bbox=None, models=None, ids=None, min_gap=0.05, dry_run=False, visual=False,
           max_sink=3.0, edge_stat="p25", floor_models=(18981, 19543, 19544, 19545)) -> dict:
	"""Dig placed objects into the terrain: lower each stack until its lowest
	footprint point touches the ground (raycasts skip placed objects except
	floor_models). visual=True grounds drawn walls instead (mesh profile per
	side: p25 for buildings, max for mounds)."""
	items = [i for i in service.enumerate_scene() if i.get("agent_scene", True)]
	items = [i for i in items if not str(i.get("name", "")).lower().startswith("lod")]
	box = [float(v) for v in bbox.split(",")] if isinstance(bbox, str) else bbox
	want_models = set(models or []); want_ids = set(ids or [])
	floors = set(floor_models or [])
	B = Bounds(service)
	profiles: dict = {}

	def ground(x, y, z):
		for _ in range(10):
			hit = client.raycast_segment([x, y, z], [x, y, z - 60.0]).get("hit")
			if not hit: return None
			if not hit.get("agent_scene") or hit.get("model_id") in floors: return hit["point"][2]
			z = hit["point"][2] - 0.02
		return None

	def visual_gap(base, x, y, z):
		m = base["model_id"]
		if m not in profiles:
			try: profiles[m] = client.command("mesh_profile", m, 16)
			except Exception: profiles[m] = None
		prof = profiles[m]
		if not prof: return None
		n = prof["n"]; (mnx, mny, _), (mxx, mxy, _) = prof["bounds"]
		sx, sy = (mxx - mnx) / n, (mxy - mny) / n
		h = math.radians(base["rotation"][2]); c, s = math.cos(h), math.sin(h)
		sides = {"s": [], "n": [], "w": [], "e": []}
		for r in range(n):
			for col in range(n):
				bz = prof["bottom"][r * n + col]
				if bz is None: continue
				edge = [k for k, on in (("s", r == 0), ("n", r == n - 1), ("w", col == 0), ("e", col == n - 1)) if on]
				if not edge: continue
				lx, ly = mnx + (col + 0.5) * sx, mny + (r + 0.5) * sy
				g = ground(x + lx * c - ly * s, y + lx * s + ly * c, z + bz + 1.0)
				if g is None: continue
				for k in edge: sides[k].append(z + bz - g)
		vals = [(max(v) if edge_stat == "max" else sorted(v)[len(v) // 4]) for v in sides.values() if v]
		return max(vals) if vals else None

	def fp(it, grow=0.0):
		b = B.collision(it["model_id"])
		return footprint(b, it["position"][0], it["position"][1], it["rotation"][2], grow) if b else None

	items.sort(key=lambda i: i["position"][2])
	rider_of: dict = {}
	stacks: dict = {}
	for it in items:
		if it["instance_id"] in rider_of: continue
		quad = fp(it, 0.2)
		group = [it]
		if quad:
			bz = it["position"][2]
			for other in items:
				if other is it or other["instance_id"] in rider_of: continue
				oq = fp(other)
				if oq and quad_area(oq) > quad_area(quad) * 1.5:
					continue                     # something bigger can't rest on this
				if other["position"][2] > bz + 0.5 and point_in_convex(other["position"][0], other["position"][1], quad):
					rider_of[other["instance_id"]] = it["instance_id"]; group.append(other)
		stacks[it["instance_id"]] = group
	ops, report = [], []
	for stack in stacks.values():
		stack.sort(key=lambda i: i["position"][2])
		base = stack[0]
		x, y, z = base["position"]
		if box and not (box[0] <= x <= box[2] and box[1] <= y <= box[3]): continue
		if want_models and base["model_id"] not in want_models: continue
		if want_ids and base["instance_id"] not in want_ids: continue
		b = B.collision(base["model_id"])
		if not b: continue
		(ax, ay, az), (bx, by, _) = b
		az = max(az, B.visual_bottom(base["model_id"]))
		ax += 0.3; ay += 0.3; bx -= 0.3; by -= 0.3
		h = math.radians(base["rotation"][2]); c, s = math.cos(h), math.sin(h)
		pts = [(x + lx * c - ly * s, y + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by), (0, 0))]
		bottom = z + az
		gs = [g for g in (ground(px, py, bottom + 1.0) for px, py in pts) if g is not None]
		if len(gs) < 3:
			report.append({"instance_id": base["instance_id"], "skipped": "no ground under footprint"}); continue
		gap = bottom - min(gs)
		relief = max(gs) - min(gs)
		if visual:
			vg = visual_gap(base, x, y, z)
			if vg is not None:
				gap = min(vg, max_sink)
		if gap <= min_gap: continue
		for it in stack:
			ix, iy, iz = it["position"]
			ops.append({"action": "transform", "instance_id": it["instance_id"], "x": ix, "y": iy,
			            "z": iz - gap - 0.03, "heading": it["rotation"][2], "snap": False})
		if gap > 0.5 or relief > 1.5:
			report.append({"instance_id": base["instance_id"], "name": base.get("name"), "lowered": round(gap, 2),
			               "relief": round(relief, 2), "position": [round(x, 1), round(y, 1)]})
	if ops and not dry_run:
		patch_in_chunks(service, ops)
	return {"ok": True, "transforms": len(ops), "dry_run": dry_run, "notes": report}


# ------------------------------------------------------------------ world objects
TRUNK_NAMES = ("veg", "tree", "palm", "elm", "locust", "pine", "cypress", "bush", "shrub", "telgrph", "lamppost",
               "mlamp", "pole")
TERRAIN_NAMES = ("bit", "road", "ground", "grnd", "land", "terrain", "lod")


def world_items(service, bbox) -> list[dict]:
	"""World (non-agent) objects whose origin is inside bbox (x0,y0,x1,y1)."""
	x0, y0, x1, y1 = bbox
	seen: dict = {}
	R = 100.0
	y = y0
	while y <= y1 + R:
		x = x0
		while x <= x1 + R:
			offset = 0
			while offset is not None:
				page = service.engine("inspect_zone_page", [x, y, R * 0.75, offset, 256])
				for it in page.get("items", []):
					px, py = it["position"][:2]
					if not it.get("agent_scene") and x0 <= px <= x1 and y0 <= py <= y1:
						seen[it["instance_id"]] = it
				offset = page.get("page", {}).get("next_offset")
			x += R
		y += R
	return list(seen.values())


def world_blockers(service, bbox, B: Bounds | None = None):
	"""(building footprints, trunk points) of world objects in bbox."""
	B = B or Bounds(service)
	quads, trunks = [], []
	for it in world_items(service, bbox):
		name = str(it.get("name", "")).lower()
		x, y = it["position"][:2]
		if any(k in name for k in TRUNK_NAMES):
			trunks.append((x, y)); continue
		if any(k in name for k in TERRAIN_NAMES):
			continue
		b = B.collision(it["model_id"])
		if not b: continue
		(ax, ay, _), (bx, by, _) = b
		if bx - ax > 60 or by - ay > 60 or (bx - ax) * (by - ay) < 1.0:
			continue
		quads.append(footprint(b, x, y, it["rotation"][2]))
	return quads, trunks


# ------------------------------------------------------------------ wall run
def wall_run(service, points: list, *, closed: bool = True, gates: list | None = None,
             containers=(2932, 2934, 2935), container_levels: int = 2, fence: int | None = 19641,
             mix: float = 0.65, seed: int = 1, gate_posts: bool = True, post_inset: float = 2.0,
             dry_run: bool = False) -> dict:
	"""Lay a wall along a polyline (or closed polygon).

	Pieces (containers stacked `container_levels` high, palisade `fence`
	sections) are laid end to end at their real length; leftover length is
	absorbed as overlap where a fence slides behind a container (hidden), or as
	staggered fence laps - never by gaps. Corners are closed (each stretch that
	starts/ends at a corner starts/ends with a fence pushed 0.1 m past it, or
	with a container when no fence model is given). gates: [{x, y, opening}] -
	the wall stops at the outer face of two container posts set across the
	wall line, `post_inset` m towards the inside.
	"""
	rng = random.Random(seed)
	B = Bounds(service)
	pts = [tuple(p[:2]) for p in points]
	ring = pts + [pts[0]] if closed else pts

	def piece(model):
		b = B.collision(model)
		(ax, ay, az), (bx, by, bz) = b
		w, d = bx - ax, by - ay
		if d >= w:   # long axis local Y -> heading = theta - 90
			return {"model": model, "len": d, "half": w / 2, "hoff": -90.0, "height": bz - az}
		return {"model": model, "len": w, "half": d / 2, "hoff": 0.0, "height": bz - az}

	C = [piece(m) for m in containers] if containers else []
	F = piece(fence) if fence else None
	clen = C[0]["len"] if C else None
	cut_half = {}
	gates = gates or []
	for g in gates:
		cut_half[id(g)] = g.get("opening", 8.0) / 2 + ((C[0]["half"] * 2) if (C and gate_posts) else 0.0) - 0.02

	def inward(ax, ay, bx, by):
		L = math.hypot(bx - ax, by - ay); ux, uy = (bx - ax) / L, (by - ay) / L
		nx, ny = -uy, ux
		if closed and not point_in_poly((ax + bx) / 2 + nx * 2, (ay + by) / 2 + ny * 2, pts):
			nx, ny = -nx, -ny
		return ux, uy, nx, ny

	def lay(seq, L, start_vertex):
		lens = [F["len"] if k == "fence" else clen for k in seq]
		excess = sum(lens) - L
		joints = list(range(len(seq) - 1))
		hidden = [j for j in joints if {seq[j], seq[j + 1]} == {"fence", "container"}]
		laps = [j for j in joints if seq[j] == seq[j + 1] == "fence"]
		ov = {j: 0.02 for j in joints}
		rest = excess - 0.02 * len(joints)
		for group, cap in ((hidden, 6.5), (laps, 99.0)):
			if rest > 0 and group:
				per = min(cap, rest / len(group) + 0.02)
				for j in group: ov[j] = per
				rest -= (per - 0.02) * len(group)
		if rest > 0.01 and joints:
			for j in joints: ov[j] += rest / len(joints)
		pos, s = [], 0.0
		for i, ln in enumerate(lens):
			pos.append((s, s + ln))
			if i < len(lens) - 1: s += ln - ov[i]
		shift = 0.0
		if len(seq) == 1 and excess > 0:
			shift = -excess if not start_vertex else 0.0
		visible = max([ov[j] for j in laps] + [0.0])
		return [(a + shift, b + shift) for a, b in pos], visible

	def plan(L, sv, ev):
		kinds = (["container"] if C else []) + (["fence"] if F else [])
		best = None
		for _ in range(120):
			if F and L <= F["len"] + 1.0:
				seq = ["fence"]
			elif not F and L <= clen + 1.0:
				seq = ["container"]
			else:
				seq = ["fence"] if (sv and F) else []
				tail = ["fence"] if (ev and F) else [kinds[0]]
				tl = F["len"] if tail == ["fence"] else clen
				total = sum(F["len"] if k == "fence" else clen for k in seq)
				while total + tl < L - 0.01:
					k = rng.choices(kinds, [mix, 1 - mix] if len(kinds) == 2 else [1])[0]
					for _ in range(rng.randint(2, 4) if k == "container" else rng.randint(1, 2)):
						if total + tl >= L - 0.01: break
						seq.append(k); total += F["len"] if k == "fence" else clen
				if rng.random() < 0.35: seq.append(rng.choice(kinds))
				seq += tail
			lens = [F["len"] if k == "fence" else clen for k in seq]
			_, vis = lay(seq, L, sv)
			soft = sum(1 for a, b in zip(seq, seq[1:]) if "fence" in (a, b))
			per = (sum(lens) - L) / soft if soft else (sum(lens) - L) * 10
			score = vis + (100 if per < 0.25 and len(seq) > 1 and soft else 0) + (50 if vis > 3.0 else 0) + 0.05 * len(seq)
			if best is None or score < best[0]: best = (score, seq)
		return best[1]

	pieces = []
	for ei, ((ax, ay), (bx, by)) in enumerate(zip(ring, ring[1:])):
		L = math.hypot(bx - ax, by - ay)
		if L < 0.5: continue
		ux, uy, nx, ny = inward(ax, ay, bx, by)
		theta = math.degrees(math.atan2(uy, ux))
		cuts = []
		for g in gates:
			t = (g["x"] - ax) * ux + (g["y"] - ay) * uy
			d = abs((g["x"] - ax) * -uy + (g["y"] - ay) * ux)
			if d < 0.6 and -cut_half[id(g)] < t < L + cut_half[id(g)]:
				cuts.append((t - cut_half[id(g)], t + cut_half[id(g)]))
		cuts.sort()
		ext = 0.1
		stretches, s0, sv = [], (-ext if (closed or ei > 0) else 0.0), True
		for c0, c1 in cuts:
			stretches.append((s0, c0, sv, False)); s0, sv = c1, False
		end_ext = ext if (closed or ei < len(ring) - 2) else 0.0
		stretches.append((s0, L + end_ext, sv, True))
		for s0, s1, sv, ev in stretches:
			length = s1 - s0
			if length < 0.5: continue
			seq = plan(length, sv, ev)
			spans, _ = lay(seq, length, sv)
			prev = None
			for kind, (p0, p1) in zip(seq, spans):
				mid = s0 + (p0 + p1) / 2
				cx, cy = ax + ux * mid, ay + uy * mid
				off = 0.0
				if kind == "fence" and prev == "fence":
					off = 0.1 if pieces[-1].get("off", 0) <= 0 else -0.1
					cx, cy = cx - uy * off, cy + ux * off
				prev = kind
				if kind == "fence":
					pieces.append({"kind": "fence", "model": F["model"], "x": cx, "y": cy, "h": theta + F["hoff"], "off": off, "levels": 1})
				else:
					p = rng.choice(C)
					pieces.append({"kind": "container", "model": p["model"], "x": cx, "y": cy, "h": theta + p["hoff"], "levels": container_levels})
	# gate posts: containers across the wall line, pushed inward
	posts = []
	if gate_posts and C:
		for g in gates:
			best = None
			for (ax, ay), (bx, by) in zip(ring, ring[1:]):
				d = seg_dist(g["x"], g["y"], ax, ay, bx, by)
				if best is None or d < best[0]: best = (d, ax, ay, bx, by)
			_, ax, ay, bx, by = best
			ux, uy, nx, ny = inward(ax, ay, bx, by)
			W = g.get("opening", 8.0)
			for side in (-1, 1):
				px = g["x"] + ux * side * (W / 2 + C[0]["half"]) + nx * post_inset
				py = g["y"] + uy * side * (W / 2 + C[0]["half"]) + ny * post_inset
				p = rng.choice(C)
				posts.append({"kind": "post", "model": p["model"], "x": px, "y": py,
				              "h": math.degrees(math.atan2(ny, nx)) + p["hoff"], "levels": container_levels})
	allp = pieces + posts
	if dry_run:
		return {"ok": True, "dry_run": True, "pieces": len(pieces), "posts": len(posts),
		        "containers": sum(p["kind"] != "fence" for p in allp), "fences": sum(p["kind"] == "fence" for p in allp)}
	placed = place_rows(service, [{"model": p["model"], "x": p["x"], "y": p["y"], "heading": p["h"], "snap": True} for p in allp],
	                    key_prefix="wall")
	upper = []
	for p, it in zip(allp, placed):
		if not it or p["levels"] <= 1: continue
		h = piece(p["model"])["height"]
		for lvl in range(1, p["levels"]):
			upper.append({"model": rng.choice(containers) if p["kind"] != "fence" else p["model"],
			              "x": p["x"], "y": p["y"], "z": it["position"][2] + lvl * h, "heading": p["h"], "snap": False})
	if upper:
		place_rows(service, upper, key_prefix="wall-upper")
	return {"ok": True, "pieces": len(pieces), "posts": len(posts), "stacked": len(upper),
	        "containers": sum(p["kind"] != "fence" for p in allp), "fences": sum(p["kind"] == "fence" for p in allp)}


# ------------------------------------------------------------------ pack
def pack(service, zone: list, models: list, *, count: int = 20, weights: list | None = None, gap: float = 1.5,
         step: float = 2.0, scan: str | None = None, align: str = "track", seed: int = 1,
         keep_off_tracks: float = 0.5, keep_off_roads: float = 1.0, avoid_world: bool = True,
         dry_run: bool = False) -> dict:
	"""Place up to `count` structures from `models` on free ground inside a
	polygon zone. Candidates nearest the dirt tracks come first (street
	frontage) and face them (align='track'); align may also be a heading in
	degrees or 'random'. Free = inside the zone, clear of world buildings and
	tree trunks, placed objects (gap m), dirt tracks and roads (ground scan)."""
	rng = random.Random(seed)
	B = Bounds(service)
	zone = [tuple(p[:2]) for p in zone]
	xs = [p[0] for p in zone]; ys = [p[1] for p in zone]
	bbox = (min(xs), min(ys), max(xs), max(ys))
	tracks = roads = None
	if scan:
		data = ground_mod.load(scan)
		names = [n.upper() for n in data["surface_names"]]
		tracks = ground_mod.TrackMask(data, ("DIRTTRACK",))
		road_names = tuple(n for n in names if ground_mod.surface_class(n) == "road")
		roads = ground_mod.TrackMask(data, road_names, min_cells=20) if road_names else None
	world_q, trunks = world_blockers(service, (bbox[0] - 20, bbox[1] - 20, bbox[2] + 20, bbox[3] + 20), B) if avoid_world else ([], [])
	placed_q = []
	for it in service.enumerate_scene():
		x, y = it["position"][:2]
		if bbox[0] - 30 <= x <= bbox[2] + 30 and bbox[1] - 30 <= y <= bbox[3] + 30:
			b = B.collision(it["model_id"])
			if b: placed_q.append(footprint(b, x, y, it["rotation"][2]))

	def free(q):
		if not all(point_in_poly(x, y, zone) for x, y in q): return False
		if tracks and tracks.footprint_hits(q, margin=keep_off_tracks) > 0: return False
		if roads and roads.footprint_hits(q, margin=keep_off_roads) > 0: return False
		cx = sum(p[0] for p in q) / 4; cy = sum(p[1] for p in q) / 4
		for tx, ty in trunks:
			if abs(tx - cx) < 20 and abs(ty - cy) < 20 and point_in_convex(tx, ty, footprint_grow(q, 1.0)): return False
		for b in world_q:
			if abs(b[0][0] - cx) < 50 and abs(b[0][1] - cy) < 50 and sat_overlap(q, b, 0.5): return False
		for b in placed_q:
			if abs(b[0][0] - cx) < 50 and abs(b[0][1] - cy) < 50 and sat_overlap(q, b, gap): return False
		return True

	cells = [(bbox[0] + i * step, bbox[1] + j * step) for i in range(int((bbox[2] - bbox[0]) / step) + 1)
	         for j in range(int((bbox[3] - bbox[1]) / step) + 1)]
	cells = [c for c in cells if point_in_poly(c[0], c[1], zone)]
	rng.shuffle(cells)
	if tracks: cells.sort(key=lambda c: tracks.distance(*c))
	rows = []
	weights = weights or [1] * len(models)
	for cx, cy in cells:
		if len(rows) >= count: break
		m = rng.choices(models, weights)[0]
		b = B.collision(m)
		if not b: continue
		(ax, ay, _), (bx, by, _) = b
		long_y = (by - ay) >= (bx - ax)
		if align == "track" and tracks:
			d = tracks.direction(cx, cy)
			along = math.degrees(math.atan2(d[2], d[1])) if d else rng.uniform(0, 360)
			h = along - 90 if long_y else along
			# face the track: flip so the model's -Y (front) side points at it
			if d and ((d[3] - cx) * -math.sin(math.radians(h)) + (d[4] - cy) * math.cos(math.radians(h))) > 0:
				h += 180
		elif align == "random":
			h = rng.uniform(0, 360)
		else:
			h = float(align) if align not in ("track",) else 0.0
		q = footprint(b, cx, cy, h)
		if free(q):
			rows.append({"model": m, "x": cx, "y": cy, "heading": h % 360, "snap": True})
			placed_q.append(q)
	if dry_run:
		return {"ok": True, "dry_run": True, "planned": len(rows), "rows": rows}
	placed = place_rows(service, rows, key_prefix="pack") if rows else []
	return {"ok": True, "placed": len([p for p in placed if p]), "instance_ids": [p["instance_id"] for p in placed if p]}


def footprint_grow(q, k):
	cx = sum(p[0] for p in q) / len(q); cy = sum(p[1] for p in q) / len(q)
	out = []
	for x, y in q:
		dx, dy = x - cx, y - cy; L = math.hypot(dx, dy) or 1
		out.append((x + dx / L * k, y + dy / L * k))
	return out


# ------------------------------------------------------------------ plan map
def plan_map(service, bbox, out_path, *, scan: str | None = None, issues: str | None = None, scale: float = 3.0) -> dict:
	"""Top-down plan: world building footprints (grey), placed objects (blue),
	dirt tracks (orange) / roads (dark) from a ground scan, lint issues (red)."""
	from PIL import Image, ImageDraw
	x0, y0, x1, y1 = bbox
	W, H = int((x1 - x0) * scale), int((y1 - y0) * scale)
	P = lambda x, y: ((x - x0) * scale, (y1 - y) * scale)
	img = Image.new("RGB", (W, H), (226, 206, 168))
	d = ImageDraw.Draw(img, "RGBA")
	if scan:
		data = ground_mod.load(scan)
		ox, oy = data["origin"]; st = data["step"]
		for r in range(data["ny"]):
			for c in range(data["nx"]):
				cls = ground_mod.class_at(data, r * data["nx"] + c)
				col = {"track": (196, 130, 60, 200), "road": (90, 90, 95, 200), "pavement": (160, 160, 165, 160),
				       "grass": (120, 160, 80, 120)}.get(cls)
				if col:
					x, y = ox + c * st, oy + r * st
					if x0 <= x <= x1 and y0 <= y <= y1:
						d.rectangle([P(x - st / 2, y + st / 2), P(x + st / 2, y - st / 2)], fill=col)
	B = Bounds(service)
	wq, trunks = world_blockers(service, bbox, B)
	for q in wq:
		d.polygon([P(*p) for p in q], fill=(150, 150, 150, 200), outline=(90, 90, 90))
	for tx, ty in trunks:
		cx, cy = P(tx, ty); d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=(40, 110, 40))
	n = 0
	for it in service.enumerate_scene():
		x, y = it["position"][:2]
		if not (x0 <= x <= x1 and y0 <= y <= y1) or str(it.get("name", "")).lower().startswith("lod"): continue
		b = B.collision(it["model_id"])
		if not b: continue
		q = footprint(b, x, y, it["rotation"][2])
		area = quad_area(q)
		if area > 400:      # ground planes / slabs: outline only
			d.polygon([P(*p) for p in q], outline=(60, 110, 200, 160))
		else:
			d.polygon([P(*p) for p in q], fill=(60, 110, 200, 200) if area >= 3 else (160, 40, 160, 220))
		n += 1
	if issues:
		for it in json.loads(Path(issues).read_text(encoding="utf-8")):
			cx, cy = P(it["x"], it["y"]); d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], outline=(255, 0, 0), width=3)
	img.save(out_path)
	return {"ok": True, "path": str(out_path), "size": [W, H], "placed_objects": n, "world_buildings": len(wq)}
