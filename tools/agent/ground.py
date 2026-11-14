"""Ground scans: what is on the ground where (collision surface materials).

``ground_scan`` in the engine casts vertical rays over a rectangle and returns,
per cell, the topmost hit (height, surface, model) and the lowest hit (the
ground under roofs and trees). This module tiles those requests, names the
surfaces (data/surfinfo.dat), renders a coloured map and turns a surface class
- typically ``DIRTTRACK`` - into a queryable mask: distance to the nearest
track, the track direction there, and whether a footprint sits on it.
"""

from __future__ import annotations

import json
import math
import os
from collections import deque
from pathlib import Path

DEFAULT_SURFACES = [  # GTA SA surfinfo.dat order (index = surface id), used when the file isn't found
	"DEFAULT", "TARMAC", "TARMAC_FUCKED", "TARMAC_REALLYFUCKED", "PAVEMENT", "PAVEMENT_FUCKED", "GRAVEL",
	"FUCKED_CONCRETE", "PAINTED_GROUND", "GRASS_SHORT_LUSH", "GRASS_MEDIUM_LUSH", "GRASS_LONG_LUSH",
	"GRASS_SHORT_DRY", "GRASS_MEDIUM_DRY", "GRASS_LONG_DRY", "GOLFGRASS_ROUGH", "GOLFGRASS_SMOOTH",
	"STEEP_SLIDYGRASS", "STEEP_CLIFF", "FLOWERBED", "MEADOW", "WASTEGROUND", "WOODLANDGROUND", "VEGETATION",
	"MUD_WET", "MUD_DRY", "DIRT", "DIRTTRACK", "SAND_DEEP", "SAND_MEDIUM", "SAND_COMPACT", "SAND_ARID",
	"SAND_MORE", "SAND_BEACH", "CONCRETE_BEACH", "ROCK_DRY", "ROCK_WET", "ROCK_CLIFF", "WATER_RIVERBED",
	"WATER_SHALLOW", "CORNFIELD", "HEDGE", "WOOD_CRATES", "WOOD_SOLID", "WOOD_THIN", "GLASS",
	"GLASS_WINDOWS_LARGE", "GLASS_WINDOWS_SMALL", "EMPTY1", "EMPTY2", "GARAGE_DOOR", "THICK_METAL_PLATE",
	"SCAFFOLD_POLE", "LAMP_POST", "METAL_GATE", "METAL_CHAIN_FENCE", "GIRDER", "FIRE_HYDRANT", "CONTAINER",
	"NEWS_VENDOR"]

CLASS_COLOURS = {
	"track": (196, 110, 40), "dirt": (170, 120, 70), "sand": (226, 200, 150), "road": (90, 90, 95),
	"pavement": (160, 160, 165), "grass": (110, 150, 70), "rock": (130, 115, 100), "water": (60, 110, 190),
	"structure": (40, 40, 45), "none": (255, 0, 255), "other": (200, 170, 200),
}


def surface_names(game_dir: str | None = None) -> list[str]:
	game_dir = game_dir or os.environ.get("ARIANE_GAME_DIR")
	if game_dir:
		for name in ("data/surfinfo.dat", "DATA/SURFINFO.DAT", "data/SURFINFO.DAT"):
			path = Path(game_dir) / name
			if path.exists():
				names = []
				for line in path.read_text(encoding="latin-1").splitlines():
					line = line.strip()
					if line and not line.startswith("#") and not line.startswith(";"):
						names.append(line.split()[0])
				if names:
					return names
	return list(DEFAULT_SURFACES)


def surface_class(name: str) -> str:
	n = name.upper()
	if n.startswith("P_"):                       # procedural variants (P_GRASSLIGHTER, P_DIRTROCKY, ...)
		n = n[2:]
		if n.startswith("BUSHY") or n.startswith("GRASS") or "LEAF" in n or "UNDERWOOD" in n or "FOREST" in n:
			return "grass"
		if n.startswith("DIRT") or n.startswith("MUD") or n.startswith("GRAVEL"): return "dirt"
		if n.startswith("CONCRETE") or n.startswith("PAVE"): return "pavement"
		if n.startswith("ROCK") or n.startswith("CLIFF"): return "rock"
	if n == "DIRTTRACK": return "track"
	if n in ("DIRT", "MUD_DRY", "MUD_WET", "WASTEGROUND", "GRAVEL"): return "dirt"
	if n.startswith("SAND"): return "sand"
	if n.startswith("TARMAC") or n in ("DEFAULT",): return "road"
	if n.startswith("PAVEMENT") or n in ("PAINTED_GROUND", "FUCKED_CONCRETE", "CONCRETE_BEACH"): return "pavement"
	if "GRASS" in n or n in ("MEADOW", "FLOWERBED", "WOODLANDGROUND", "VEGETATION", "HEDGE", "CORNFIELD"): return "grass"
	if n.startswith("ROCK") or n == "STEEP_CLIFF": return "rock"
	if n.startswith("WATER"): return "water"
	return "structure"


def scan(client, bbox: tuple[float, float, float, float], step: float = 1.0, *, include_agent: bool = False,
         z_top: float = 400.0, tile_cells: int = 60000, game_dir: str | None = None) -> dict:
	"""Run ground_scan over bbox in tiles and merge into one grid (row-major, south row first)."""
	x0, y0, x1, y1 = bbox
	nx = int(math.floor((x1 - x0) / step)) + 1
	ny = int(math.floor((y1 - y0) / step)) + 1
	keys = ("top_z", "top_surface", "top_model", "ground_z", "ground_surface")
	grid = {k: [None] * (nx * ny) for k in keys}
	rows_per_tile = max(1, tile_cells // nx)
	row = 0
	while row < ny:
		rows = min(rows_per_tile, ny - row)
		ty0 = y0 + row * step
		ty1 = y0 + (row + rows - 1) * step
		reply = client.command("ground_scan", x0, ty0, x0 + (nx - 1) * step + 1e-3, ty1 + 1e-3, step, z_top,
		                       int(include_agent))
		tnx, tny = reply["nx"], reply["ny"]
		for r in range(min(tny, rows)):
			for c in range(min(tnx, nx)):
				for k in keys:
					grid[k][(row + r) * nx + c] = reply[k][r * tnx + c]
		row += rows
	names = surface_names(game_dir)
	return {"origin": [x0, y0], "step": step, "nx": nx, "ny": ny, "surface_names": names, **grid}


def save(data: dict, path: str | Path) -> None:
	Path(path).write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")


def load(path: str | Path) -> dict:
	return json.loads(Path(path).read_text(encoding="utf-8"))


def class_at(data: dict, i: int, layer: str = "ground") -> str:
	s = data[f"{layer}_surface"][i]
	if s is None or s < 0:
		return "none"
	names = data["surface_names"]
	return surface_class(names[s]) if s < len(names) else "other"


def render_png(data: dict, path: str | Path, layer: str = "ground", scale: int = 4,
               mask: "TrackMask | None" = None) -> None:
	from PIL import Image, ImageDraw
	nx, ny = data["nx"], data["ny"]
	img = Image.new("RGB", (nx * scale, ny * scale))
	draw = ImageDraw.Draw(img)
	for r in range(ny):
		for c in range(nx):
			i = r * nx + c
			colour = CLASS_COLOURS.get(class_at(data, i, layer), CLASS_COLOURS["other"])
			z = data[f"{layer}_z"][i]
			if z is not None:  # light hill shading from the height difference to the west neighbour
				zw = data[f"{layer}_z"][i - 1] if c > 0 and data[f"{layer}_z"][i - 1] is not None else z
				shade = max(-30, min(30, (z - zw) * 25 / max(data["step"], 0.25)))
				colour = tuple(max(0, min(255, int(v + shade))) for v in colour)
			py = (ny - 1 - r) * scale  # north up
			draw.rectangle([c * scale, py, c * scale + scale - 1, py + scale - 1], fill=colour)
	img.save(path)


class TrackMask:
	"""Cells of a scan whose ground surface is in ``surfaces`` (default DIRTTRACK)."""

	def __init__(self, data: dict, surfaces: tuple[str, ...] = ("DIRTTRACK",), layer: str = "ground",
	             min_cells: int = 12):
		self.data = data
		self.ox, self.oy = data["origin"]
		self.step = data["step"]
		self.nx, self.ny = data["nx"], data["ny"]
		names = [n.upper() for n in data["surface_names"]]
		wanted = {names.index(s.upper()) for s in surfaces if s.upper() in names}
		raw = [s in wanted for s in data[f"{layer}_surface"]]
		self.mask = self._drop_specks(raw, min_cells)
		self.dist = self._distance_field()

	def _drop_specks(self, raw: list[bool], min_cells: int) -> list[bool]:
		seen = [False] * len(raw)
		out = [False] * len(raw)
		for start, on in enumerate(raw):
			if not on or seen[start]:
				continue
			comp, queue = [], deque([start])
			seen[start] = True
			while queue:
				i = queue.popleft(); comp.append(i)
				r, c = divmod(i, self.nx)
				for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
					rr, cc = r + dr, c + dc
					if 0 <= rr < self.ny and 0 <= cc < self.nx:
						j = rr * self.nx + cc
						if raw[j] and not seen[j]:
							seen[j] = True; queue.append(j)
			if len(comp) >= min_cells:
				for i in comp: out[i] = True
		return out

	def _distance_field(self) -> list[float]:
		"""Chamfer (1, sqrt2) distance in metres from every cell to the nearest mask cell."""
		INF = 1e9
		d = [0.0 if m else INF for m in self.mask]
		nx, ny, s = self.nx, self.ny, self.step
		diag = s * math.sqrt(2)
		for r in range(ny):
			for c in range(nx):
				i = r * nx + c
				if c > 0: d[i] = min(d[i], d[i - 1] + s)
				if r > 0:
					d[i] = min(d[i], d[i - nx] + s)
					if c > 0: d[i] = min(d[i], d[i - nx - 1] + diag)
					if c < nx - 1: d[i] = min(d[i], d[i - nx + 1] + diag)
		for r in range(ny - 1, -1, -1):
			for c in range(nx - 1, -1, -1):
				i = r * nx + c
				if c < nx - 1: d[i] = min(d[i], d[i + 1] + s)
				if r < ny - 1:
					d[i] = min(d[i], d[i + nx] + s)
					if c < nx - 1: d[i] = min(d[i], d[i + nx + 1] + diag)
					if c > 0: d[i] = min(d[i], d[i + nx - 1] + diag)
		return d

	def _cell(self, x: float, y: float) -> int | None:
		c = int(round((x - self.ox) / self.step)); r = int(round((y - self.oy) / self.step))
		if 0 <= c < self.nx and 0 <= r < self.ny:
			return r * self.nx + c
		return None

	def on_track(self, x: float, y: float) -> bool:
		i = self._cell(x, y)
		return i is not None and self.mask[i]

	def distance(self, x: float, y: float) -> float:
		i = self._cell(x, y)
		return 1e9 if i is None else self.dist[i]

	def direction(self, x: float, y: float, radius: float = 6.0):
		"""(distance, ux, uy, nearest_x, nearest_y): unit direction of the track
		near (x, y), from the principal axis of track cells around the nearest one."""
		i = self._cell(x, y)
		if i is None:
			return None
		# walk down the distance field to the nearest track cell
		r, c = divmod(i, self.nx)
		for _ in range(4000):
			if self.mask[r * self.nx + c]: break
			best = (self.dist[r * self.nx + c], r, c)
			for dr in (-1, 0, 1):
				for dc in (-1, 0, 1):
					rr, cc = r + dr, c + dc
					if 0 <= rr < self.ny and 0 <= cc < self.nx and self.dist[rr * self.nx + cc] < best[0]:
						best = (self.dist[rr * self.nx + cc], rr, cc)
			if (best[1], best[2]) == (r, c): break
			r, c = best[1], best[2]
		tx, ty = self.ox + c * self.step, self.oy + r * self.step
		k = int(radius / self.step)
		pts = [(cc, rr) for rr in range(max(0, r - k), min(self.ny, r + k + 1))
		       for cc in range(max(0, c - k), min(self.nx, c + k + 1)) if self.mask[rr * self.nx + cc]]
		if len(pts) < 3:
			return (self.distance(x, y), 1.0, 0.0, tx, ty)
		mx = sum(p[0] for p in pts) / len(pts); my = sum(p[1] for p in pts) / len(pts)
		sxx = sum((p[0] - mx) ** 2 for p in pts); syy = sum((p[1] - my) ** 2 for p in pts)
		sxy = sum((p[0] - mx) * (p[1] - my) for p in pts)
		ang = 0.5 * math.atan2(2 * sxy, sxx - syy)
		return (self.distance(x, y), math.cos(ang), math.sin(ang), tx, ty)

	def footprint_hits(self, quad: list[tuple[float, float]], margin: float = 0.0) -> int:
		"""Number of track cells inside a convex footprint (grown by margin)."""
		xs = [p[0] for p in quad]; ys = [p[1] for p in quad]
		hits = 0
		c0 = max(0, int((min(xs) - margin - self.ox) / self.step)); c1 = min(self.nx - 1, int((max(xs) + margin - self.ox) / self.step) + 1)
		r0 = max(0, int((min(ys) - margin - self.oy) / self.step)); r1 = min(self.ny - 1, int((max(ys) + margin - self.oy) / self.step) + 1)
		for r in range(r0, r1 + 1):
			for c in range(c0, c1 + 1):
				if not self.mask[r * self.nx + c]:
					continue
				x, y = self.ox + c * self.step, self.oy + r * self.step
				if _point_in_convex(x, y, quad, margin):
					hits += 1
		return hits

	def stats(self) -> dict:
		cells = sum(self.mask)
		return {"track_cells": cells, "track_area_m2": round(cells * self.step * self.step, 1),
		        "grid": [self.nx, self.ny], "step": self.step}


def _point_in_convex(px: float, py: float, quad, margin: float = 0.0) -> bool:
	n = len(quad)
	cx = sum(p[0] for p in quad) / n; cy = sum(p[1] for p in quad) / n
	for i in range(n):
		(x1, y1), (x2, y2) = quad[i], quad[(i + 1) % n]
		ex, ey = x2 - x1, y2 - y1
		L = math.hypot(ex, ey) or 1.0
		nx_, ny_ = ey / L, -ex / L
		if (cx - x1) * nx_ + (cy - y1) * ny_ > 0:
			nx_, ny_ = -nx_, -ny_                 # make the normal point outward
		if (px - x1) * nx_ + (py - y1) * ny_ > margin:
			return False
	return True


class StructureMask(TrackMask):
	"""Cells where something solid stands well above the ground (buildings,
	walls, big props): top hit more than ``min_height`` above the lowest hit.
	Scans skip agent objects by default, so this is the world's buildings only."""

	def __init__(self, data: dict, min_height: float = 2.5, min_cells: int = 4):
		self.data = data
		self.ox, self.oy = data["origin"]
		self.step = data["step"]
		self.nx, self.ny = data["nx"], data["ny"]
		raw = []
		for top, low in zip(data["top_z"], data["ground_z"]):
			raw.append(top is not None and low is not None and top - low > min_height)
		self.mask = self._drop_specks(raw, min_cells)
		self.dist = self._distance_field()
