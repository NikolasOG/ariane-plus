#!/usr/bin/env python3
"""Canonical CLI for Ariane's local agent API and asset index."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import uuid

if __package__:
	from .ariane_ipc import ArianeClient, ArianeError, DEFAULT_ENGINE_SOCKET
else:
	from ariane_ipc import ArianeClient, ArianeError, DEFAULT_ENGINE_SOCKET
if __package__:
	from .asset_index import AssetIndex, DEFAULT_DATABASE, DEFAULT_GTASTUFF, build_index
else:
	from asset_index import AssetIndex, DEFAULT_DATABASE, DEFAULT_GTASTUFF, build_index
if __package__:
	from .asset_discovery import AssetDiscovery, DEFAULT_SESSION_DIR
else:
	from asset_discovery import AssetDiscovery, DEFAULT_SESSION_DIR
if __package__:
	from .service import ArianeService, ScenePatchError, DEFAULT_STATE_DIR
else:
	from service import ArianeService, ScenePatchError, DEFAULT_STATE_DIR


def emit(payload: object) -> None:
	print(json.dumps(payload, indent=2, ensure_ascii=False))


def patch_in_chunks(service, ops: list[dict], size: int = 400) -> dict:
	"""Apply many simple operations as a few patches (one bridge round trip each)."""
	done = 0
	for i in range(0, len(ops), size):
		service.apply_scene_patch(ops[i:i + size])
		done += len(ops[i:i + size])
	return {"ok": True, "applied": done}


def settle_objects(service, client, args) -> dict:
	"""Dig-in settling for placed objects (stack aware, terrain-only raycasts)."""
	import math
	items = [i for i in service.enumerate_scene() if i.get("agent_scene", True)]
	items = [i for i in items if not str(i.get("name", "")).lower().startswith("lod")]
	box = [float(v) for v in args.bbox.split(",")] if args.bbox else None
	want_models = set(args.models or []); want_ids = set(args.ids or [])
	floor_models = set(getattr(args, "floor_models", []) or [])
	bounds_cache: dict = {}
	def bounds(model):
		if model not in bounds_cache:
			bounds_cache[model] = service.probe_asset(model, ensure_renderable=True).get("collision_bounds")
		return bounds_cache[model]
	render_cache: dict = {}
	def render_bottom(model):
		if model not in render_cache:
			rb = service.probe_asset(model, ensure_renderable=True).get("render_bounds")
			render_cache[model] = rb[0][2] if rb else None
		return render_cache[model]
	profile_cache: dict = {}
	def visual_gap(base, x, y, z):
		"""Sink needed so every side's drawn wall bottom meets the terrain (median per side)."""
		m = base["model_id"]
		if m not in profile_cache:
			try:
				profile_cache[m] = client.command("mesh_profile", m, 16)
			except Exception:
				profile_cache[m] = None
		prof = profile_cache[m]
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
				wx, wy = x + lx * c - ly * s, y + lx * s + ly * c
				g = ground(wx, wy, z + bz + 1.0)
				if g is None: continue
				for k in edge: sides[k].append(z + bz - g)
		if getattr(args, "edge_stat", "p25") == "max":
			meds = [max(v) for v in sides.values() if v]               # every edge on the ground
		else:
			meds = [sorted(v)[len(v) // 4] for v in sides.values() if v]   # 25th percentile: wall bottoms, not window gaps / corner footings
		return max(meds) if meds else None
	def footprint(it, grow=0.0):
		b = bounds(it["model_id"])
		if not b: return None
		(ax, ay, _), (bx, by, _) = b
		h = math.radians(it["rotation"][2]); c, s = math.cos(h), math.sin(h)
		x, y = it["position"][:2]
		return [(x + lx * c - ly * s, y + lx * s + ly * c)
		        for lx, ly in ((ax - grow, ay - grow), (bx + grow, ay - grow), (bx + grow, by + grow), (ax - grow, by + grow))]
	def quad_area(q):
		return abs(sum(q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1] for i in range(4))) / 2
	def inside(px, py, quad):
		sign = None
		for i in range(4):
			(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % 4]
			cross = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
			if abs(cross) < 1e-9: continue
			if sign is None: sign = cross > 0
			elif (cross > 0) != sign: return False
		return True
	# a stack = a base object plus everything resting on it: centre inside its
	# footprint and higher up (stacked containers, a hut or lamp on a post...)
	items.sort(key=lambda i: i["position"][2])
	rider_of: dict = {}
	stacks: dict = {}
	for it in items:
		if it["instance_id"] in rider_of: continue
		quad = footprint(it, 0.2)
		group = [it]
		if quad:
			bz = it["position"][2]
			for other in items:
				if other is it or other["instance_id"] in rider_of: continue
				oq = footprint(other)
				if oq and quad_area(oq) > quad_area(quad) * 1.5:
					continue                         # something bigger can't rest on this (a smoke emitter under a ruin...)
				if other["position"][2] > bz + 0.5 and inside(other["position"][0], other["position"][1], quad):
					rider_of[other["instance_id"]] = it["instance_id"]; group.append(other)
		stacks[it["instance_id"]] = group
	def ground(x, y, z):
		for _ in range(10):
			hit = client.raycast_segment([x, y, z], [x, y, z - 60.0]).get("hit")
			if not hit: return None
			if not hit.get("agent_scene") or hit.get("model_id") in floor_models: return hit["point"][2]
			z = hit["point"][2] - 0.02
		return None
	ops, report = [], []
	for stack in stacks.values():
		stack.sort(key=lambda i: i["position"][2])
		base = stack[0]
		x, y, z = base["position"]
		if box and not (box[0] <= x <= box[2] and box[1] <= y <= box[3]): continue
		if want_models and base["model_id"] not in want_models: continue
		if want_ids and base["instance_id"] not in want_ids: continue
		b = bounds(base["model_id"])
		if not b: continue
		(ax, ay, az), (bx, by, _) = b
		rb = render_bottom(base["model_id"])
		if rb is not None and rb > az:
			az = rb                                  # mesh starts above its collision box: settle the visible bottom
		ax += 0.3; ay += 0.3; bx -= 0.3; by -= 0.3
		h = math.radians(base["rotation"][2]); c, s = math.cos(h), math.sin(h)
		pts = [(x + lx * c - ly * s, y + lx * s + ly * c) for lx, ly in ((ax, ay), (bx, ay), (bx, by), (ax, by), (0, 0))]
		bottom = z + az
		gs = [g for g in (ground(px, py, bottom + 1.0) for px, py in pts) if g is not None]
		if len(gs) < 3:
			report.append({"instance_id": base["instance_id"], "skipped": "no ground under footprint"}); continue
		gap = bottom - min(gs)
		relief = max(gs) - min(gs)
		if getattr(args, "visual", False):
			vg = visual_gap(base, x, y, z)
			if vg is not None:
				gap = min(vg, args.max_sink)
		if gap <= args.min_gap: continue
		for it in stack:
			ix, iy, iz = it["position"]
			ops.append({"action": "transform", "instance_id": it["instance_id"], "x": ix, "y": iy,
			            "z": iz - gap - 0.03, "heading": it["rotation"][2], "snap": False})
		if gap > 0.5 or relief > 1.5:
			report.append({"instance_id": base["instance_id"], "name": base.get("name"), "lowered": round(gap, 2),
			               "relief": round(relief, 2), "position": [round(x, 1), round(y, 1)]})
	if ops and not args.dry_run:
		patch_in_chunks(service, ops)
	return {"ok": True, "transforms": len(ops), "dry_run": args.dry_run, "notes": report}


def add_engine_commands(sub: argparse._SubParsersAction) -> None:
	sub.add_parser("ping")
	sub.add_parser("capabilities")
	scene = sub.add_parser("scene")
	scene.add_argument("logical")
	scene.add_argument("physical", type=Path)
	session = sub.add_parser("session")
	session_sub = session.add_subparsers(dest="session_command", required=True)
	begin = session_sub.add_parser("begin")
	begin.add_argument("name", nargs="?")
	session_sub.add_parser("status")
	session_sub.add_parser("commit")
	session_sub.add_parser("rollback")
	place = sub.add_parser("place")
	place.add_argument("model")
	for name in ("x", "y", "z", "heading"): place.add_argument(name, type=float)
	place.add_argument("--no-snap", action="store_true")
	batch = sub.add_parser("batch")
	batch.add_argument("file", type=Path)
	transform = sub.add_parser("transform")
	transform.add_argument("instance_id", type=int)
	for name in ("x", "y", "z", "heading"): transform.add_argument(name, type=float)
	transform.add_argument("--no-snap", action="store_true")
	transform3d = sub.add_parser("transform3d")
	transform3d.add_argument("instance_id", type=int)
	for name in ("x", "y", "z", "pitch", "roll", "yaw"): transform3d.add_argument(name, type=float)
	analysis = sub.add_parser("analyze-placement")
	analysis.add_argument("instance_id", type=int); analysis.add_argument("--grid", type=int, default=3)
	fit = sub.add_parser("fit-terrain")
	fit.add_argument("instance_id", type=int); fit.add_argument("profile", choices=("building", "prop"))
	fit.add_argument("--radius", type=float, default=30.0); fit.add_argument("--step", type=float, default=3.0)
	fit.add_argument("--max-relief", type=float)
	delete = sub.add_parser("delete", help="Delete one or more instances (one patch)")
	delete.add_argument("instance_id", type=int, nargs="+")
	tbatch = sub.add_parser("transform-batch",
		help="Move many instances in one patch: TSV rows 'instance_id x y z heading [snap]'")
	tbatch.add_argument("file", type=Path)
	settle = sub.add_parser("settle",
		help="Dig placed objects into the terrain: lower each (and anything stacked on it) until its lowest "
		     "footprint point touches the ground; ground found by raycasts that skip placed objects")
	settle.add_argument("--bbox", help="x0,y0,x1,y1 world rectangle (default: whole layer)")
	settle.add_argument("--models", type=int, nargs="*", help="only these model ids")
	settle.add_argument("--ids", type=int, nargs="*", help="only these instance ids")
	settle.add_argument("--min-gap", type=float, default=0.05)
	settle.add_argument("--dry-run", action="store_true")
	settle.add_argument("--visual", action="store_true",
	                    help="ground the drawn walls of buildings (not vehicles / open structures): per side, compare the mesh bottom with the "
	                         "terrain (25th percentile per side) and sink by the worst side - for models built for sloped sites")
	settle.add_argument("--max-sink", type=float, default=3.0)
	settle.add_argument("--edge-stat", choices=("p25", "max"), default="p25",
	                    help="--visual: p25 = typical wall bottom per side (buildings); max = every edge must touch "
	                         "the ground (mounds, heaps, slabs)")
	settle.add_argument("--floor-models", type=int, nargs="*", default=[18981, 19543, 19544, 19545],
	                    help="placed models that count as ground (slabs, ground planes)")
	gscan = sub.add_parser("ground-scan",
		help="Map ground height + collision surface (DIRTTRACK, SAND, TARMAC...) over a rectangle")
	gscan.add_argument("--bbox", required=True, help="x0,y0,x1,y1 (use --bbox=... for negative values)")
	gscan.add_argument("--step", type=float, default=1.0)
	gscan.add_argument("--out", type=Path, required=True, help="scan JSON")
	gscan.add_argument("--png", type=Path, help="coloured surface map (north up)")
	gscan.add_argument("--layer", choices=("ground", "top"), default="ground")
	gscan.add_argument("--include-agent", action="store_true", help="also hit placed (layer) objects")
	gscan.add_argument("--game-dir", help="for surface names (default: ARIANE_GAME_DIR)")
	tmask = sub.add_parser("track-mask", help="Summarise / render the track cells of a ground scan")
	tmask.add_argument("scan", type=Path)
	tmask.add_argument("--surface", nargs="+", default=["DIRTTRACK"])
	tmask.add_argument("--png", type=Path)
	tmask.add_argument("--at", type=float, nargs=2, metavar=("X", "Y"), help="query distance + direction at a point")
	ortho = sub.add_parser("capture-ortho",
		help="Straight-down orthographic capture with a known metres-per-pixel scale (no fog, long draw distance)")
	ortho.add_argument("cx", type=float); ortho.add_argument("cy", type=float)
	ortho.add_argument("half_width", type=float, help="half of the captured width in metres")
	ortho.add_argument("--out", type=Path, required=True)
	ortho.add_argument("--cam-z", type=float, default=250.0)
	ortho.add_argument("--lod-mult", type=float, default=3.0)
	ortho.add_argument("--delay", type=int, default=30, help="frames to wait for streaming")
	prefab = sub.add_parser("prefab", help="Reusable multi-part structures (save from placed objects, place anywhere)")
	prefab_sub = prefab.add_subparsers(dest="prefab_command", required=True)
	prefab_sub.add_parser("list")
	psave = prefab_sub.add_parser("save"); psave.add_argument("name")
	psave.add_argument("--ids", type=int, nargs="+", required=True)
	psave.add_argument("--origin", type=float, nargs=3, metavar=("X", "Y", "HEADING"),
	                   help="prefab origin + facing (default: first instance)")
	psave.add_argument("--notes", default="")
	pplace = prefab_sub.add_parser("place"); pplace.add_argument("name", help="saved name or a .json path")
	for n in ("x", "y", "heading"): pplace.add_argument(n, type=float)
	pplace.add_argument("--no-snap", action="store_true")
	pplace.add_argument("--base-z", type=float, help="put ground parts at this height (e.g. on a slab) instead of snapping")
	rep = sub.add_parser("replace-building",
		help="Remove the world building at x,y (and its LOD) and place another model at the same transform")
	rep.add_argument("x", type=float); rep.add_argument("y", type=float); rep.add_argument("model", type=int)
	rep.add_argument("--old-model", type=int, help="only match this world model")
	rep.add_argument("--heading-offset", type=float, default=0.0)
	rep.add_argument("--dx", type=float, default=0.0, help="offset along the old building's local X")
	rep.add_argument("--dy", type=float, default=0.0)
	rep.add_argument("--dz", type=float, help="fixed height offset (default: snap to ground)")
	rep.add_argument("--keep-old", action="store_true", help="place the model without removing the building")
	rep.add_argument("--wire-poles", action="store_true",
	                 help="plant a pole at each wire end left hanging over the old footprint")
	wends = sub.add_parser("wire-ends", help="Wire ends hanging over a rectangle (x0,y0,x1,y1 at ground z)")
	wends.add_argument("--box", required=True, help="x0,y0,x1,y1"); wends.add_argument("--ground-z", type=float, required=True)
	fac = sub.add_parser("facade", help="Line one side of the world building at x,y with a model (scaffolding, rubble...)")
	fac.add_argument("x", type=float); fac.add_argument("y", type=float); fac.add_argument("model", type=int)
	fac.add_argument("--side", default="auto", help="auto (faces the road / open space), N/S/E/W or 0-3")
	fac.add_argument("--spacing", type=float); fac.add_argument("--offset", type=float, default=0.3)
	fac.add_argument("--levels", default="auto", help="'auto' = up to the building height, or a number")
	fac.add_argument("--level-height", type=float); fac.add_argument("--heading-offset", type=float, default=0.0)
	fac.add_argument("--margin", type=float, default=1.0); fac.add_argument("--max-levels", type=int, default=6)
	fac.add_argument("--scan", type=Path, help="ground scan: 'auto' side faces the nearest road")
	fac.add_argument("--building-model", type=int, help="only match this world model")
	fac.add_argument("--min-run", type=int, default=3, help="drop wall runs shorter than this many bays")
	fac.add_argument("--min-levels", type=int, default=2, help="skip walls lower than this many levels")
	lintp = sub.add_parser("lint", help="Check a site: wall gaps, overlaps, world clashes, objects on tracks / roads")
	lintp.add_argument("--site", type=Path, required=True, help="site JSON (perimeter, gates, scan...)")
	lintp.add_argument("--json", type=Path, help="write the issue list here")
	lintp.add_argument("--fix-ids", type=Path, help="write ids of interior objects that would resolve issues")
	lintp.add_argument("--protect", type=int, nargs="*", default=[])
	lintp.add_argument("--max", type=int, default=40, help="issues to print")
	ssync = sub.add_parser("samp-sync", help="SA-MP > Sync to server (writes scriptfiles/ariane_live.txt)")
	ssync.add_argument("--map", help="map name (also used for gamemodes/wasteland/maps/<name>.inc)")
	ssync.add_argument("--force", action="store_true", help="allow syncing an empty layer")
	suppress = sub.add_parser("suppress-world-models")
	suppress.add_argument("x", type=float); suppress.add_argument("y", type=float)
	suppress.add_argument("radius", type=float); suppress.add_argument("model_ids", type=int, nargs="+")
	sub.add_parser("clear")
	sub.add_parser("list")
	list_page = sub.add_parser("list-page")
	list_page.add_argument("--offset", type=int, default=0); list_page.add_argument("--limit", type=int, default=64)
	list_page.add_argument("--x", type=float); list_page.add_argument("--y", type=float)
	list_page.add_argument("--radius", type=float)
	sub.add_parser("bounds")
	sub.add_parser("validate")
	validate_zone = sub.add_parser("validate-zone")
	validate_zone.add_argument("x", type=float); validate_zone.add_argument("y", type=float)
	validate_zone.add_argument("radius", type=float); validate_zone.add_argument("--offset", type=int, default=0)
	validate_zone.add_argument("--limit", type=int, default=64)
	validate_semantics = sub.add_parser("validate-semantics")
	validate_semantics.add_argument("zones", type=Path,
	                                help="JSON array of explicit entrance/route/keep-clear zones")
	zone = sub.add_parser("inspect-zone")
	zone.add_argument("x", type=float); zone.add_argument("y", type=float)
	zone.add_argument("radius", type=float); zone.add_argument("--offset", type=int, default=0)
	zone.add_argument("--limit", type=int, default=64)
	detail = sub.add_parser("asset-detail")
	detail.add_argument("model")
	camera = sub.add_parser("camera")
	for name in ("px", "py", "pz", "tx", "ty", "tz", "fov"): camera.add_argument(name, type=float)
	sub.add_parser("camera-context")
	screen = sub.add_parser("screen-to-world")
	screen.add_argument("pixel_x", type=float); screen.add_argument("pixel_y", type=float)
	screen.add_argument("width", type=int); screen.add_argument("height", type=int)
	raycast = sub.add_parser("raycast-segment")
	for name in ("start_x", "start_y", "start_z", "target_x", "target_y", "target_z"):
		raycast.add_argument(name, type=float)
	raycast.add_argument("--target-tolerance", type=float, default=1.0)
	capture = sub.add_parser("capture")
	capture.add_argument("path", type=Path)
	capture.add_argument("--label", default="current")
	capture_pose = sub.add_parser("capture-pose")
	capture_pose.add_argument("path", type=Path)
	for name in ("px", "py", "pz", "tx", "ty", "tz"): capture_pose.add_argument(name, type=float)
	capture_pose.add_argument("--fov", type=float, default=58.0)
	capture_pose.add_argument("--label", default="custom")
	capture_pose.add_argument("--expected-camera-revision", type=int)
	views = sub.add_parser("capture-views")
	views.add_argument("directory", type=Path); views.add_argument("--fov", type=float, default=58.0)
	views.add_argument("--center", type=float, nargs=3, metavar=("X", "Y", "Z"))
	views.add_argument("--span", type=float)
	sub.add_parser("save")


def engine_command(client: ArianeClient, args: argparse.Namespace) -> dict:
	command = args.command
	if command in {"ping", "capabilities", "clear", "list", "validate", "save"}:
		return client.command(command)
	if command == "scene": return client.command("scene", args.logical, args.physical.resolve())
	if command == "session":
		fields = [args.name] if args.session_command == "begin" and args.name else []
		return client.command(f"session_{args.session_command}", *fields)
	if command == "place":
		return client.command("place", args.model, args.x, args.y, args.z, args.heading, int(not args.no_snap))
	if command == "batch":
		rows = [line for line in args.file.read_text(encoding="utf-8").splitlines()
		        if line.strip() and not line.lstrip().startswith("#")]
		return client.command("batch", *rows)
	if command == "transform":
		return client.command("transform", args.instance_id, args.x, args.y, args.z,
		                      args.heading, int(not args.no_snap))
	if command == "transform3d":
		return client.command("transform3d", args.instance_id, args.x, args.y, args.z,
		                      args.pitch, args.roll, args.yaw)
	if command == "analyze-placement":
		return client.command("analyze_placement", args.instance_id, args.grid)
	if command == "fit-terrain":
		fields = [args.instance_id, args.profile, args.radius, args.step]
		if args.max_relief is not None: fields.append(args.max_relief)
		return client.command("fit_terrain", *fields)
	if command == "delete": return client.command("delete", args.instance_id)
	if command == "suppress-world-models":
		return client.command("suppress_world_models", args.x, args.y, args.radius, *args.model_ids)
	if command == "bounds": return client.command("scene_bounds")
	if command == "list-page":
		fields = [args.offset, args.limit]
		if args.x is not None and args.y is not None and args.radius is not None:
			fields.extend([args.x, args.y, args.radius])
		return client.command("list_page", *fields)
	if command == "inspect-zone":
		return client.command("inspect_zone_page", args.x, args.y, args.radius, args.offset, args.limit)
	if command == "validate-zone":
		return client.command("validate_zone", args.x, args.y, args.radius, args.offset, args.limit)
	if command == "asset-detail": return client.command("asset_detail", args.model)
	if command == "camera":
		return client.command("camera", args.px, args.py, args.pz, args.tx, args.ty, args.tz, args.fov)
	if command == "camera-context": return client.command("camera_context")
	if command == "screen-to-world":
		return client.command("screen_to_world", args.pixel_x, args.pixel_y, args.width, args.height)
	if command == "raycast-segment":
		return {"ray": client.raycast_segment(
			[args.start_x, args.start_y, args.start_z],
			[args.target_x, args.target_y, args.target_z],
			target_tolerance=args.target_tolerance)}
	if command == "capture": return client.capture_current(args.path, label=args.label)
	if command == "capture-pose":
		return client.capture_at_pose(args.path, position=[args.px, args.py, args.pz],
		                              target=[args.tx, args.ty, args.tz], fov=args.fov,
		                              label=args.label,
		                              expected_camera_revision=args.expected_camera_revision)
	if command == "capture-views":
		return client.capture_views(args.directory, args.fov, center=args.center, span=args.span)
	raise ValueError(f"unsupported command: {command}")


def main(argv: list[str] | None = None) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--socket", type=Path, default=DEFAULT_ENGINE_SOCKET)
	parser.add_argument("--timeout", type=float, default=30.0)
	parser.add_argument("--db", type=Path, default=DEFAULT_DATABASE)
	parser.add_argument("--discovery-dir", type=Path, default=DEFAULT_SESSION_DIR)
	parser.add_argument("--state-dir", type=Path, default=DEFAULT_STATE_DIR)
	sub = parser.add_subparsers(dest="command", required=True)
	add_engine_commands(sub)
	call = sub.add_parser("call", help="Call a shared service method; JSON parameters inline or from a file")
	call.add_argument("method")
	inputs = call.add_mutually_exclusive_group()
	inputs.add_argument("--params", default="{}")
	inputs.add_argument("--file", type=Path)
	survey = sub.add_parser("survey", help="Observe the place under the camera")
	survey.add_argument("output_directory")
	survey.add_argument("--radius", type=float, default=30)
	survey.add_argument("--grid", type=int, default=7)
	survey.add_argument("--pixel-x", type=float); survey.add_argument("--pixel-y", type=float)
	environment = sub.add_parser("environment")
	environment.add_argument("--hour", type=int); environment.add_argument("--minute", type=int)
	environment.add_argument("--weather-a", type=int); environment.add_argument("--weather-b", type=int)
	environment.add_argument("--blend", type=float)
	assets = sub.add_parser("assets")
	asset_sub = assets.add_subparsers(dest="asset_command", required=True)
	index = asset_sub.add_parser("index")
	sources = index.add_mutually_exclusive_group()
	sources.add_argument("--gtastuff", type=Path, default=DEFAULT_GTASTUFF)
	sources.add_argument("--gta-dir", type=Path, help="build a basic catalogue from your GTA data/*.ide files")
	index.add_argument("--semantic-assets", type=Path,
	                   help="optional AI-enriched asset JSON; kept external when its license is unknown")
	search = asset_sub.add_parser("search")
	search.add_argument("query", nargs="?", default="")
	search.add_argument("--category"); search.add_argument("--max-width", type=float)
	search.add_argument("--max-depth", type=float); search.add_argument("--has-collision", action="store_true")
	search.add_argument("--include-lod", action="store_true"); search.add_argument("--limit", type=int, default=30)
	search.add_argument("--include-hazards", action="store_true", help="also return explosive props (hidden by default)")
	asides = asset_sub.add_parser("sides", help="Which sides of a model are actually drawn (open-backed models)")
	asides.add_argument("asset_id", type=int)
	smatch = asset_sub.add_parser("size-match", help="Models with a similar footprint (e.g. a ruin to replace a building)")
	smatch.add_argument("width", type=float); smatch.add_argument("depth", type=float)
	smatch.add_argument("--query", default=""); smatch.add_argument("--tolerance", type=float, default=0.25)
	smatch.add_argument("--limit", type=int, default=20)
	hazards = asset_sub.add_parser("mark-hazards", help="Tag explosive / breakable / dynamic assets from data/object.dat")
	hazards.add_argument("--game-dir", help="default: ARIANE_GAME_DIR")
	search.add_argument("--defined-only", action="store_true",
	                    help="only return models known to the running engine")
	inspect = asset_sub.add_parser("inspect"); inspect.add_argument("asset_id", type=int)
	inspect.add_argument("--ensure-renderable", action="store_true")
	preview = asset_sub.add_parser("preview"); preview.add_argument("asset_id", type=int)
	preview.add_argument("--output", type=Path, required=True); preview.add_argument("--size", type=int, default=512)
	similar = asset_sub.add_parser("similar"); similar.add_argument("asset_id", type=int)
	similar.add_argument("--limit", type=int, default=20)
	contact = asset_sub.add_parser("contact-sheet")
	contact.add_argument("asset_ids", type=int, nargs="+")
	contact.add_argument("--output", type=Path, required=True)
	contact.add_argument("--columns", type=int, default=5)
	contact.add_argument("--tile-size", type=int, default=220)
	contact.add_argument("--thumbnail-dir", type=Path)
	discovery = sub.add_parser("discovery")
	discovery_sub = discovery.add_subparsers(dest="discovery_command", required=True)
	discovery_start = discovery_sub.add_parser("start")
	discovery_start.add_argument("brief")
	discovery_start.add_argument("--role", action="append", default=[])
	discovery_start.add_argument("--style", action="append", default=[])
	discovery_start.add_argument("--avoid", action="append", default=[])
	discovery_start.add_argument("--context", type=int, nargs="*", default=[])
	discovery_start.add_argument("--pool-limit", type=int, default=2500)
	discovery_start.add_argument("--has-collision", action="store_true")
	discovery_start.add_argument("--max-width", type=float)
	discovery_start.add_argument("--max-depth", type=float)
	discovery_coverage = discovery_sub.add_parser("coverage")
	discovery_coverage.add_argument("session_id")
	discovery_families = discovery_sub.add_parser("families")
	discovery_families.add_argument("session_id")
	discovery_families.add_argument("--offset", type=int, default=0)
	discovery_families.add_argument("--limit", type=int, default=100)
	discovery_results = discovery_sub.add_parser("results")
	discovery_results.add_argument("session_id")
	discovery_results.add_argument("--offset", type=int, default=0)
	discovery_results.add_argument("--limit", type=int, default=100)
	discovery_open = discovery_sub.add_parser("open")
	discovery_open.add_argument("session_id"); discovery_open.add_argument("family")
	discovery_open.add_argument("--offset", type=int, default=0)
	discovery_open.add_argument("--limit", type=int, default=64)
	discovery_atlas = discovery_sub.add_parser("atlas")
	discovery_atlas.add_argument("session_id"); discovery_atlas.add_argument("--output", type=Path, required=True)
	discovery_atlas.add_argument("--family-limit", type=int, default=64)
	discovery_atlas.add_argument("--thumbnail-dir", type=Path)
	discovery_residuals = discovery_sub.add_parser("residuals")
	discovery_residuals.add_argument("session_id")
	discovery_residuals.add_argument("--limit", type=int, default=64)
	discovery_residuals.add_argument("--strategy", choices=("stratified", "unknown"), default="stratified")
	discovery_mark = discovery_sub.add_parser("mark")
	discovery_mark.add_argument("session_id")
	discovery_mark.add_argument("decision", choices=("shortlist", "rejected"))
	discovery_mark.add_argument("asset_ids", type=int, nargs="+")
	discovery_mark.add_argument("--reason", default="")
	checkpoint = sub.add_parser("checkpoint")
	checkpoint_sub = checkpoint.add_subparsers(dest="checkpoint_command", required=True)
	checkpoint_sub.add_parser("list")
	checkpoint_save = checkpoint_sub.add_parser("save"); checkpoint_save.add_argument("name")
	checkpoint_restore = checkpoint_sub.add_parser("restore"); checkpoint_restore.add_argument("name")
	patch = sub.add_parser("patch")
	patch.add_argument("file", type=Path, help="JSON list or {operations,patch_id,expected_revision}")
	resolve_patch = sub.add_parser("resolve-patch")
	resolve_patch.add_argument("file", type=Path,
	                           help="JSON list or {operations,expected_revision}; never mutates the scene")
	apply_plan = sub.add_parser("apply-plan")
	apply_plan.add_argument("plan_id"); apply_plan.add_argument("--patch-id")
	groups = sub.add_parser("groups")
	group_sub = groups.add_subparsers(dest="group_command", required=True)
	group_sub.add_parser("list")
	group_create = group_sub.add_parser("create"); group_create.add_argument("group")
	group_create.add_argument("members", nargs="+")
	group_inspect = group_sub.add_parser("inspect"); group_inspect.add_argument("group")
	group_inspect.add_argument("--offset", type=int, default=0); group_inspect.add_argument("--limit", type=int, default=50)
	group_transform = group_sub.add_parser("transform"); group_transform.add_argument("group")
	for name in ("dx", "dy", "dz", "dheading"): group_transform.add_argument(f"--{name}", type=float, default=0.0)
	group_transform.add_argument("--patch-id"); group_transform.add_argument("--expected-revision", type=int)
	group_clone = group_sub.add_parser("clone"); group_clone.add_argument("group")
	group_clone.add_argument("new_group"); group_clone.add_argument("key_prefix")
	for name in ("dx", "dy", "dz", "dheading"): group_clone.add_argument(f"--{name}", type=float, default=0.0)
	group_clone.add_argument("--patch-id")
	group_delete = group_sub.add_parser("delete"); group_delete.add_argument("group"); group_delete.add_argument("--patch-id")
	support = sub.add_parser("support")
	support_sub = support.add_subparsers(dest="support_command", required=True)
	support_set = support_sub.add_parser("set"); support_set.add_argument("child"); support_set.add_argument("parent")
	support_snap = support_sub.add_parser("snap"); support_snap.add_argument("child"); support_snap.add_argument("parent")
	support_snap.add_argument("--clearance", type=float, default=0.0)
	sub.add_parser("validate-composition")
	args = parser.parse_args(argv)
	try:
		if args.command in {"call", "survey", "environment"}:
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			if args.command == "call":
				params = json.loads(args.file.read_text() if args.file else args.params)
				if not isinstance(params, dict): raise ValueError("parameters must be a JSON object")
				payload = service.dispatch(args.method, params)
			elif args.command == "survey":
				payload = service.vibe.observe(args.output_directory, args.radius, args.grid, args.pixel_x, args.pixel_y)
			else:
				settings = {key: getattr(args, key) for key in ("hour", "minute", "weather_a", "weather_b", "blend")
				            if getattr(args, key) is not None}
				payload = service.environment(**settings)
		elif args.command == "assets":
			if args.asset_command == "index": payload = build_index(
				args.gtastuff, args.db, semantic_assets=args.semantic_assets, gta_dir=args.gta_dir)
			elif args.asset_command == "search":
				service = ArianeService(args.socket, args.db, args.timeout,
				                        args.discovery_dir, args.state_dir)
				if args.defined_only:
					payload = {"assets": service.search_assets(
						args.query, category=args.category, max_width=args.max_width,
						max_depth=args.max_depth, has_collision=True if args.has_collision else None,
						defined_only=True, limit=args.limit)}
				else:
					payload = {"assets": AssetIndex(args.db).search(
						args.query, category=args.category, max_width=args.max_width,
						max_depth=args.max_depth, has_collision=True if args.has_collision else None,
						exclude_lod=not args.include_lod, limit=args.limit, include_hazards=args.include_hazards)}
			elif args.asset_command == "sides":
				service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
				a = service.probe_asset(args.asset_id, ensure_renderable=True)
				areas = a.get("mesh_side_area") or {}
				opposite = {"+x": "-x", "-x": "+x", "+y": "-y", "-y": "+y", "up": "down", "down": "up"}
				open_sides = [k for k in ("+x", "-x", "+y", "-y")
				              if areas and areas[k] < 0.15 * max(areas[opposite[k]], 1.0)]
				payload = {"ok": True, "model": args.asset_id, "name": a.get("name"), "side_area_m2": areas,
				           "open_sides": open_sides,
				           "hint": "model-local: +y is the side facing the heading direction rotated by 90 (local +Y); "
				                   "cover open sides or turn them against a wall"}
			elif args.asset_command == "size-match":
				if __package__:
					from . import building_tools as bt
				else:
					import building_tools as bt
				payload = {"assets": bt.size_match(AssetIndex(args.db), args.width, args.depth, query=args.query,
				                                   tolerance=args.tolerance, limit=args.limit)}
			elif args.asset_command == "mark-hazards":
				import os
				game_dir = args.game_dir or os.environ.get("ARIANE_GAME_DIR")
				if not game_dir: raise ValueError("pass --game-dir or set ARIANE_GAME_DIR")
				payload = AssetIndex(args.db).mark_hazards(game_dir)
			elif args.asset_command == "inspect":
				if args.ensure_renderable:
					payload = {"asset": ArianeService(args.socket, args.db, args.timeout,
						args.discovery_dir, args.state_dir).inspect_asset(
						args.asset_id, ensure_renderable=True)}
				else: payload = {"asset": AssetIndex(args.db).inspect(args.asset_id)}
			elif args.asset_command == "preview":
				payload = ArianeService(args.socket, args.db, args.timeout,
					args.discovery_dir, args.state_dir).render_asset_views(
					args.asset_id, args.output, size=args.size)
			elif args.asset_command == "similar": payload = {"assets": AssetIndex(args.db).similar(args.asset_id, args.limit)}
			else: payload = AssetIndex(args.db).contact_sheet(
				args.asset_ids, args.output, columns=args.columns, tile_size=args.tile_size,
				thumbnail_dir=args.thumbnail_dir)
		elif args.command == "discovery":
			discovery_service = AssetDiscovery(AssetIndex(args.db), session_dir=args.discovery_dir)
			if args.discovery_command == "start":
				constraints = {key: value for key, value in {
					"has_collision": True if args.has_collision else None,
					"max_width": args.max_width, "max_depth": args.max_depth,
				}.items() if value is not None}
				brief = {"scene": args.brief, "roles": args.role, "styles": args.style,
				         "avoid": args.avoid} if args.role or args.style or args.avoid else args.brief
				payload = discovery_service.start(brief, context_asset_ids=args.context,
				                                  constraints=constraints, pool_limit=args.pool_limit)
			elif args.discovery_command == "coverage": payload = discovery_service.coverage(args.session_id)
			elif args.discovery_command == "families": payload = discovery_service.families(
				args.session_id, offset=args.offset, limit=args.limit)
			elif args.discovery_command == "results": payload = discovery_service.results(
				args.session_id, offset=args.offset, limit=args.limit)
			elif args.discovery_command == "open": payload = discovery_service.open_family(
				args.session_id, args.family, offset=args.offset, limit=args.limit)
			elif args.discovery_command == "atlas": payload = discovery_service.atlas(
				args.session_id, args.output, family_limit=args.family_limit,
				thumbnail_dir=args.thumbnail_dir)
			elif args.discovery_command == "residuals": payload = discovery_service.residuals(
				args.session_id, limit=args.limit, strategy=args.strategy)
			else: payload = discovery_service.mark(
				args.session_id, args.asset_ids, decision=args.decision, reason=args.reason)
		elif args.command in {"checkpoint", "validate-semantics", "patch", "resolve-patch", "apply-plan", "groups", "support",
		                    "validate-composition"}:
			checkpoint_service = ArianeService(args.socket, args.db, args.timeout,
			                                   args.discovery_dir, args.state_dir)
			if args.command == "validate-semantics":
				payload = checkpoint_service.validate_semantic_zones(
					json.loads(args.zones.read_text(encoding="utf-8")))
			elif args.command == "patch":
				document = json.loads(args.file.read_text(encoding="utf-8"))
				if isinstance(document, list): document = {"operations": document}
				payload = checkpoint_service.apply_scene_patch(document["operations"],
					patch_id=document.get("patch_id"), expected_revision=document.get("expected_revision"))
			elif args.command == "resolve-patch":
				document = json.loads(args.file.read_text(encoding="utf-8"))
				if isinstance(document, list): document = {"operations": document}
				payload = checkpoint_service.resolve_scene_patch(
					document["operations"], expected_revision=document.get("expected_revision"))
			elif args.command == "apply-plan":
				payload = checkpoint_service.apply_resolved_plan(args.plan_id, patch_id=args.patch_id)
			elif args.command == "groups":
				if args.group_command == "list": payload = checkpoint_service.list_groups()
				elif args.group_command == "create":
					members = [int(value) if value.isdigit() else value for value in args.members]
					payload = checkpoint_service.create_group(args.group, members)
				elif args.group_command == "inspect":
					payload = checkpoint_service.inspect_group(args.group, offset=args.offset, limit=args.limit)
				elif args.group_command == "transform":
					payload = checkpoint_service.transform_group(args.group, dx=args.dx, dy=args.dy,
						dz=args.dz, dheading=args.dheading, patch_id=args.patch_id,
						expected_revision=args.expected_revision)
				elif args.group_command == "clone":
					payload = checkpoint_service.clone_group(args.group, args.new_group, args.key_prefix,
						dx=args.dx, dy=args.dy, dz=args.dz, dheading=args.dheading,
						patch_id=args.patch_id)
				else: payload = checkpoint_service.delete_group(args.group, patch_id=args.patch_id)
			elif args.command == "support":
				child = int(args.child) if args.child.isdigit() else args.child
				parent = int(args.parent) if args.parent.isdigit() else args.parent
				if args.support_command == "set": payload = checkpoint_service.set_support(child, parent)
				else: payload = checkpoint_service.snap_to_support(child, parent, args.clearance)
			elif args.command == "validate-composition": payload = checkpoint_service.validate_composition()
			elif args.checkpoint_command == "list": payload = checkpoint_service.checkpoint_list()
			elif args.checkpoint_command == "save": payload = checkpoint_service.checkpoint_save(args.name)
			else: payload = checkpoint_service.checkpoint_restore(args.name)
		elif args.command == "wire-ends":
			if __package__:
				from . import building_tools as bt
			else:
				import building_tools as bt
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			x0, y0, x1, y1 = (float(v) for v in args.box.split(","))
			payload = {"ok": True, "ends": bt.wire_ends(service, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], args.ground_z)}
		elif args.command in {"replace-building", "facade"}:
			if __package__:
				from . import building_tools as bt, ground
			else:
				import building_tools as bt, ground
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			if args.command == "replace-building":
				payload = bt.replace_building(service, args.x, args.y, args.model, old_model=args.old_model,
				                              heading_offset=args.heading_offset, dx=args.dx, dy=args.dy, dz=args.dz,
				                              keep_old=args.keep_old, wire_poles=args.wire_poles)
			else:
				mask = None
				if args.scan:
					data = ground.load(args.scan)
					names = [n.upper() for n in data["surface_names"]]
					mask = ground.TrackMask(data, tuple(n for n in names if ground.surface_class(n) == "road"), min_cells=20)
				payload = bt.facade(service, args.x, args.y, args.model, side=args.side, spacing=args.spacing,
				                    offset=args.offset, levels=int(args.levels) if str(args.levels).isdigit() else "auto",
				                    level_height=args.level_height, heading_offset=args.heading_offset, margin=args.margin,
				                    max_levels=args.max_levels, road_mask=mask, building_model=args.building_model,
				                    min_run=args.min_run, min_levels=args.min_levels)
		elif args.command == "lint":
			if __package__:
				from . import lint as lint_mod
			else:
				import lint as lint_mod
			service = ArianeService(args.socket, args.db, max(args.timeout, 120.0), args.discovery_dir, args.state_dir)
			site = json.loads(args.site.read_text(encoding="utf-8"))
			result = lint_mod.lint(service, site, base_dir=args.site.parent)
			if args.fix_ids:
				kill, flagged = lint_mod.fix_ids(result, set(args.protect))
				args.fix_ids.write_text(" ".join(map(str, kill)), encoding="utf-8")
				result["fix"] = {"delete": len(kill), "file": str(args.fix_ids), "protected_but_flagged": flagged}
			result.pop("_internal", None)
			if args.json:
				args.json.write_text(json.dumps(result["issues"], indent=1), encoding="utf-8")
			payload = {k: v for k, v in result.items() if k != "issues"}
			payload["issues"] = result["issues"][:args.max]
			payload["issues_total"] = len(result["issues"])
		elif args.command == "prefab":
			if __package__:
				from . import prefabs
			else:
				import prefabs
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			if args.prefab_command == "list":
				payload = {"ok": True, "prefabs": prefabs.list_prefabs(service)}
			elif args.prefab_command == "save":
				payload = prefabs.save_prefab(service, args.name, args.ids, tuple(args.origin) if args.origin else None, args.notes)
			else:
				payload = prefabs.place_prefab(service, prefabs.load_prefab(service, args.name), args.x, args.y,
				                               args.heading, snap=not args.no_snap, base_z=args.base_z)
		elif args.command in {"ground-scan", "track-mask", "capture-ortho", "samp-sync"}:
			if __package__:
				from . import ground
			else:
				import ground
			if args.command == "track-mask":
				data = ground.load(args.scan)
				mask = ground.TrackMask(data, tuple(args.surface))
				payload = {"ok": True, **mask.stats()}
				if args.at:
					d = mask.direction(*args.at)
					payload["at"] = {"distance": round(d[0], 2), "direction": [round(d[1], 3), round(d[2], 3)],
					                 "nearest": [round(d[3], 2), round(d[4], 2)]} if d else None
				if args.png:
					view = dict(data)
					names = [n.upper() for n in data["surface_names"]]
					keep = {names.index(n.upper()) for n in args.surface if n.upper() in names}
					view["ground_surface"] = [(s if m else (names.index("SAND_MEDIUM") if "SAND_MEDIUM" in names else -1))
					                          for s, m in zip(data["ground_surface"], mask.mask)]
					ground.render_png(view, args.png)
					payload["png"] = str(args.png)
			else:
				client = ArianeClient(args.socket, max(args.timeout, 120.0))
				if args.command == "ground-scan":
					box = tuple(float(v) for v in args.bbox.split(","))
					data = ground.scan(client, box, args.step, include_agent=args.include_agent, game_dir=args.game_dir)
					ground.save(data, args.out)
					from collections import Counter
					classes = Counter(ground.class_at(data, i, args.layer) for i in range(data["nx"] * data["ny"]))
					names = data["surface_names"]
					surfaces = Counter(names[s] if 0 <= s < len(names) else "none" for s in data[f"{args.layer}_surface"])
					payload = {"ok": True, "out": str(args.out), "grid": [data["nx"], data["ny"]], "step": args.step,
					           "classes": dict(classes.most_common()), "surfaces": dict(surfaces.most_common(12))}
					if args.png:
						ground.render_png(data, args.png, args.layer)
						payload["png"] = str(args.png)
				elif args.command == "capture-ortho":
					payload = client.command("capture_ortho", args.out.resolve(), args.cx, args.cy, args.half_width,
					                         args.cam_z, args.lod_mult, args.delay)
				else:
					payload = client.command("samp_sync", args.map or "-", int(args.force))
		elif args.command in {"transform-batch", "settle"} or (args.command == "delete" and len(args.instance_id) > 1):
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			if args.command == "delete":
				payload = patch_in_chunks(service, [{"action": "delete", "instance_id": i} for i in args.instance_id])
			elif args.command == "transform-batch":
				ops = []
				for line in args.file.read_text(encoding="utf-8").splitlines():
					cols = line.split()
					if len(cols) < 5 or cols[0].startswith("#"): continue
					ops.append({"action": "transform", "instance_id": int(cols[0]), "x": float(cols[1]), "y": float(cols[2]),
					            "z": float(cols[3]), "heading": float(cols[4]),
					            "snap": len(cols) > 5 and cols[5] not in ("0", "false")})
				payload = patch_in_chunks(service, ops)
			else:
				payload = settle_objects(service, ArianeClient(args.socket, args.timeout), args)
		elif args.command in {"place", "transform", "transform3d", "delete", "clear", "suppress-world-models"}:
			service = ArianeService(args.socket, args.db, args.timeout, args.discovery_dir, args.state_dir)
			if args.command == "clear": payload = service.engine("clear")
			elif args.command == "suppress-world-models":
				payload = service.suppress_native_models(args.x, args.y, args.radius, args.model_ids)
			else:
				op = {"action": args.command}
				if args.command == "place": op.update(key=f"cli:{uuid.uuid4().hex}", model=args.model)
				else: op["instance_id"] = args.instance_id[0] if isinstance(args.instance_id, list) else args.instance_id
				if args.command != "delete":
					op.update(x=args.x, y=args.y, z=args.z, heading=args.yaw if args.command == "transform3d" else args.heading,
					          snap=not getattr(args, "no_snap", True))
				if args.command == "transform3d": op.update(pitch=args.pitch, roll=args.roll, snap=False)
				payload = service.apply_scene_patch([op])
				if payload.get("objects"): payload["instance_id"] = payload["objects"][0]["instance_id"]
		else:
			payload = engine_command(ArianeClient(args.socket, args.timeout), args)
		emit(payload)
		return 0
	except (ArianeError, FileNotFoundError, OSError, RuntimeError, ValueError) as error:
		if isinstance(error, ScenePatchError): emit(error.payload)
		elif isinstance(error, ArianeError) and error.response:
			emit({"ok": False, "error": str(error), **{
				key: value for key, value in error.response.items()
				if key in {"scene_revision", "camera_revision"}}})
		else: emit({"ok": False, "error": str(error)})
		return 1


if __name__ == "__main__":
	raise SystemExit(main())
