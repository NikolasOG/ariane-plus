"""Layout lint for agent-built maps.

Everything comes from the running editor plus a small site file:
  * placed objects + their collision footprints  (enumerate_scene / probe_asset)
  * world buildings around the site               (inspect_zone pages)
  * dirt tracks, roads and pavements              (a ground scan: ground.py)

Site file (JSON):
  {"perimeter": [[x, y], ...],               # wall line (closed ring), optional
   "gates": [{"x":..,"y":..,"half_cut":..}],  # openings in the wall line
   "wall_models": [2932, 2934, 2935, 19641],
   "fixed_zones": [[[x, y], ...], ...],       # e.g. an older compound: never auto-deleted
   "track_ok_models": [1423, 976, 988],       # may stand on a track (gate leaves, barriers)
   "ignore_models": [19632, 1463],            # tiny decor
   "shelter_models": [3414, 3418],            # roofs props may stand under
   "scan": "scan.json",                       # ground scan (track + road masks)
   "bbox": [x0, y0, x1, y1]}                  # area to lint (default: perimeter bounds + 20 m)

Checks: perimeter closure, wall end gaps, overlaps (height aware), props buried
inside structures, clashes with world buildings, objects on dirt tracks, objects
on roads / pavements.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

if __package__:
	from . import ground
else:
	import ground


# ------------------------------------------------------------------ 2D helpers
def _seg_dist(px, py, ax, ay, bx, by):
	dx, dy = bx - ax, by - ay
	L2 = dx * dx + dy * dy
	t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
	return math.hypot(ax + t * dx - px, ay + t * dy - py)


def _pip(px, py, poly):
	inside = False
	n = len(poly)
	for i in range(n):
		x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
		if (y1 > py) != (y2 > py) and px < x1 + (py - y1) * (x2 - x1) / (y2 - y1):
			inside = not inside
	return inside


def _area(poly):
	return abs(sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
	               for i in range(len(poly)))) / 2 if len(poly) > 2 else 0.0


def _sat(p, q, margin=0.0):
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


def _clip(subject, a, b):
	out = []
	inside = lambda p: (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]) >= 0
	def cut(p, q):
		x1, y1 = p; x2, y2 = q; x3, y3 = a; x4, y4 = b
		d = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
		if abs(d) < 1e-12: return q
		t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / d
		return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))
	for i in range(len(subject)):
		cur, prev = subject[i], subject[i - 1]
		if inside(cur):
			if not inside(prev): out.append(cut(prev, cur))
			out.append(cur)
		elif inside(prev):
			out.append(cut(prev, cur))
	return out


def _ccw(poly):
	s = sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))
	return poly if s > 0 else poly[::-1]


def _inter_area(p, q):
	out = _ccw(list(p)); q = _ccw(list(q))
	for i in range(len(q)):
		if not out: return 0.0
		out = _clip(out, q[i], q[(i + 1) % len(q)])
	return _area(out)


def _ppdist(px, py, poly):
	if _pip(px, py, poly): return 0.0
	return min(_seg_dist(px, py, *poly[i], *poly[(i + 1) % len(poly)]) for i in range(len(poly)))


TRUNK_NAMES = ("veg", "tree", "palm", "elm", "locust", "pine", "cypress", "cedar", "oak", "bush", "shrub",
               "telgrph", "lamppost", "mlamp", "pole")


# ------------------------------------------------------------------ lint
def lint(service, site: dict, *, base_dir: Path | None = None) -> dict:
	wall_models = set(site.get("wall_models", [2932, 2934, 2935, 19641]))
	ignore = set(site.get("ignore_models", [19632, 1463]))
	shelters = set(site.get("shelter_models", [3414, 3418, 3861, 3862, 3863, 19129, 3626]))
	track_ok = set(site.get("track_ok_models", [1423, 976, 988, 969, 975]))
	perimeter = [tuple(p) for p in site.get("perimeter", [])]
	gates = site.get("gates", [])
	fixed_zones = [[tuple(p) for p in z] for z in site.get("fixed_zones", [])]
	if site.get("bbox"):
		x0, y0, x1, y1 = site["bbox"]
	elif perimeter:
		x0 = min(p[0] for p in perimeter) - 20; x1 = max(p[0] for p in perimeter) + 20
		y0 = min(p[1] for p in perimeter) - 20; y1 = max(p[1] for p in perimeter) + 20
	else:
		raise ValueError("site needs a perimeter or a bbox")
	cx, cy, radius = (x0 + x1) / 2, (y0 + y1) / 2, math.hypot(x1 - x0, y1 - y0) / 2

	bounds_cache: dict = {}
	def bounds(model):
		if model not in bounds_cache:
			try: bounds_cache[model] = service.probe_asset(model, ensure_renderable=False).get("collision_bounds")
			except Exception: bounds_cache[model] = None
		return bounds_cache[model]
	def quad(model, x, y, h):
		b = bounds(model)
		(ax, ay, az), (bx, by, bz) = b if b else ((-0.5, -0.5, 0.0), (0.5, 0.5, 1.0))
		c, s = math.cos(math.radians(h)), math.sin(math.radians(h))
		return [(x + lx * c - ly * s, y + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by))], az, bz

	objs, world, trunks = [], [], []
	for it in service.enumerate_scene():
		x, y, z = it["position"]
		if not (x0 <= x <= x1 and y0 <= y <= y1) or str(it.get("name", "")).lower().startswith("lod"):
			continue
		q, az, bz = quad(it["model_id"], x, y, it["rotation"][2])
		objs.append({"id": it["instance_id"], "m": it["model_id"], "name": it.get("name", str(it["model_id"])),
		             "x": x, "y": y, "z": z + az, "ztop": z + bz, "q": q, "a": _area(q)})
	offset = 0
	while offset is not None:
		page = service.engine("inspect_zone_page", [cx, cy, min(radius, 250.0), offset, 256])
		for it in page["items"]:
			if it.get("agent_scene") or str(it.get("name", "")).lower().startswith("lod"):
				continue
			name = str(it.get("name", "")).lower()
			x, y, _ = it["position"]
			if any(k in name for k in TRUNK_NAMES):
				# trees / poles: their bounds are the crown or cross-arm - only the trunk matters
				trunks.append({"name": it.get("name", ""), "x": x, "y": y, "model": it["model_id"],
				               "kind": "pole" if any(k in name for k in ("telgrph", "lamppost", "mlamp", "pole")) else "tree"})
				continue
			b = bounds(it["model_id"])
			if not b: continue
			(ax, ay, _), (bx, by, _) = b
			if bx - ax > 40 or by - ay > 40 or (bx - ax) * (by - ay) < 1.0:
				continue                    # terrain tiles / tiny things
			q, _, _ = quad(it["model_id"], x, y, it["rotation"][2])
			world.append({"name": it.get("name", ""), "q": q, "x": x, "y": y})
		offset = page["page"]["next_offset"]

	ring = perimeter + perimeter[:1] if perimeter else []
	def perim_dist(x, y):
		return min(_seg_dist(x, y, *a, *b) for a, b in zip(ring, ring[1:])) if ring else 1e9
	def in_fixed(x, y):
		return any(_pip(x, y, z) for z in fixed_zones)
	walls = [o for o in objs if o["m"] in wall_models and (perim_dist(o["x"], o["y"]) < 3.0 or in_fixed(o["x"], o["y"]))]
	wall_ids = {id(o) for o in walls}
	riders = set()
	for o in objs:
		if id(o) in wall_ids: continue
		for w in walls:
			if o["z"] >= w["ztop"] - 0.3 and _pip(o["x"], o["y"], w["q"]):
				riders.add(id(o)); break
	issues = []
	def issue(kind, msg, x, y, ids=()):
		issues.append({"kind": kind, "msg": msg, "x": round(x, 1), "y": round(y, 1), "ids": list(ids)})

	# 1. perimeter closure
	for (ax, ay), (bx, by) in zip(ring, ring[1:]):
		L = math.hypot(bx - ax, by - ay); ux, uy = (bx - ax) / L, (by - ay) / L
		near = [w for w in walls if _seg_dist(w["x"], w["y"], ax, ay, bx, by) < 8]
		start = None; t = 0.0
		while t <= L + 1e-6:
			px, py = ax + ux * t, ay + uy * t
			gate = any(math.hypot(px - g["x"], py - g["y"]) < g.get("half_cut", 5.0) + 0.3 for g in gates)
			covered = gate or any(_pip(px - uy * k * 0.1, py + ux * k * 0.1, w["q"]) for w in near for k in range(-25, 26))
			if not covered and start is None: start = t
			if covered and start is not None:
				issue("perimeter-gap", "wall line open for %.1f m" % (t - start), ax + ux * (start + t) / 2, ay + uy * (start + t) / 2)
				start = None
			t += 0.5
		if start is not None:
			issue("perimeter-gap", "wall line open for %.1f m" % (L - start), ax + ux * (start + L) / 2, ay + uy * (start + L) / 2)

	# 2. wall end gaps
	for o in walls:
		q = o["q"]
		edges = sorted(((q[i], q[(i + 1) % 4]) for i in range(4)), key=lambda e: math.hypot(e[1][0] - e[0][0], e[1][1] - e[0][1]))
		for a, b in edges[:2]:
			ex, ey = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
			if any(math.hypot(ex - g["x"], ey - g["y"]) < g.get("half_cut", 5.0) + 1.5 for g in gates):
				continue
			L = math.hypot(ex - o["x"], ey - o["y"]) or 1
			bx_, by_ = ex - (ex - o["x"]) / L * 0.4, ey - (ey - o["y"]) / L * 0.4
			others = [w for w in walls if w is not o and abs(w["z"] - o["z"]) < 2.0 and math.hypot(w["x"] - ex, w["y"] - ey) < 12]
			best = min(((_ppdist(ex, ey, w["q"]), w) for w in others), default=(99, None), key=lambda t: t[0])
			if 0.12 < best[0] < 3.0 and not (best[1] and _ppdist(bx_, by_, best[1]["q"]) <= best[0] + 0.05):
				issue("wall-gap", "%s end is %.2f m from the next wall piece" % (o["name"], best[0]), ex, ey, [o["id"]])

	# 3. overlaps / buried props
	def shrink(q, k):
		mx = sum(p[0] for p in q) / 4; my = sum(p[1] for p in q) / 4
		return [(px - (px - mx) / (math.hypot(px - mx, py - my) or 1) * k, py - (py - my) / (math.hypot(px - mx, py - my) or 1) * k) for px, py in q]
	for i, a in enumerate(objs):
		if a["m"] in ignore: continue
		for b in objs[i + 1:]:
			if b["m"] in ignore or abs(a["x"] - b["x"]) > 30 or abs(a["y"] - b["y"]) > 30: continue
			if (id(a) in wall_ids and id(b) in wall_ids) or a["m"] in shelters or b["m"] in shelters: continue
			if a["z"] >= b["ztop"] - 0.3 or b["z"] >= a["ztop"] - 0.3: continue
			if not _sat(a["q"], b["q"]): continue
			big, small = (a, b) if a["a"] >= b["a"] else (b, a)
			if small["a"] >= 3.0:
				ov = _inter_area(a["q"], b["q"])
				if ov > 0.3:
					issue("overlap", "%s and %s overlap by %.1f m2" % (a["name"], b["name"], ov), (a["x"] + b["x"]) / 2, (a["y"] + b["y"]) / 2, [a["id"], b["id"]])
			elif big["a"] >= 3.0 and _pip(small["x"], small["y"], shrink(big["q"], 0.4)):
				issue("inside", "%s sits inside %s" % (small["name"], big["name"]), small["x"], small["y"], [small["id"], big["id"]])

	# 4. world clashes
	for w in world:
		for o in objs:
			if o["a"] < 1.0 or abs(o["x"] - w["x"]) > 30 or abs(o["y"] - w["y"]) > 30: continue
			if _sat(o["q"], w["q"]):
				ov = _inter_area(o["q"], w["q"])
				if ov > 0.3:
					issue("world", "%s overlaps world %s by %.1f m2" % (o["name"], w["name"], ov), o["x"], o["y"], [o["id"]])

	# 4b. tree trunks / poles standing inside placed objects
	for t in trunks:
		for o in objs:
			if o["a"] >= 1.0 and abs(o["x"] - t["x"]) < 20 and abs(o["y"] - t["y"]) < 20 and _pip(t["x"], t["y"], shrink(o["q"], 0.1)):
				issue(t["kind"], "world %s (model %d) stands inside %s - suppress it or move the object" % (t["name"], t["model"], o["name"]),
				      t["x"], t["y"], [o["id"]])
				break

	# 5./6. tracks and roads from the ground scan
	scan_path = site.get("scan")
	if scan_path:
		path = Path(scan_path)
		if base_dir and not path.is_absolute(): path = base_dir / path
		data = ground.load(path)
		tracks = ground.TrackMask(data, ("DIRTTRACK",))
		names = [n.upper() for n in data["surface_names"]]
		road_names = tuple(n for n in names if ground.surface_class(n) in ("road",))
		roads = ground.TrackMask(data, road_names, min_cells=30) if road_names else None
		for o in objs:
			if id(o) in wall_ids or id(o) in riders or o["m"] in track_ok: continue
			if perimeter and not _pip(o["x"], o["y"], perimeter) and perim_dist(o["x"], o["y"]) > 3:
				pass
			hits = tracks.footprint_hits(o["q"], margin=-0.25)
			if hits >= max(1, int(0.15 * o["a"])) or (o["a"] < 1.5 and tracks.on_track(o["x"], o["y"])):
				issue("track", "%s is on a dirt track (%d m2)" % (o["name"], hits), o["x"], o["y"], [o["id"]])
			if roads and roads.footprint_hits(o["q"], margin=0.3) > 0:
				issue("road", "%s is on or against a road" % o["name"], o["x"], o["y"], [o["id"]])

	seen, uniq = set(), []
	for it in issues:
		k = (it["kind"], it["msg"], it["x"], it["y"])
		if k not in seen:
			seen.add(k); uniq.append(it)
	from collections import Counter
	return {"ok": not uniq, "objects": len(objs), "wall_pieces": len(walls), "world_buildings": len(world),
	        "counts": dict(Counter(i["kind"] for i in uniq)), "issues": uniq,
	        "_internal": {"objs": objs, "wall_ids": wall_ids, "riders": riders, "fixed": in_fixed, "track_ok": track_ok}}


def fix_ids(result: dict, protect: set[int]) -> tuple[list[int], list[int]]:
	"""Interior objects to delete to resolve issues (never walls, riders, gate parts, fixed zones, protected)."""
	inner = result["_internal"]
	by_id = {o["id"]: o for o in inner["objs"]}
	kill, flagged_protected = set(), set()
	for it in result["issues"]:
		if it["kind"] in ("perimeter-gap", "wall-gap", "tree", "pole"): continue
		cands = [by_id[i] for i in it["ids"] if i in by_id]
		cands = [o for o in cands if id(o) not in inner["wall_ids"] and id(o) not in inner["riders"]
		         and o["m"] not in inner["track_ok"] and not inner["fixed"](o["x"], o["y"])]
		if it["kind"] in ("overlap", "inside") and len(cands) == 2:
			cands = [min(cands, key=lambda o: o["a"])]
		for o in cands:
			(flagged_protected if o["id"] in protect else kill).add(o["id"])
	return sorted(kill), sorted(flagged_protected)
