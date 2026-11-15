"""Mapping styles: user-curated object sets ("wasteland", "farm", ...) that
agents prefer when they are asked to map in that style.

One JSON file per style lives in Ariane's data directory, next to
``samp_settings.ini`` (``<ariane.exe dir>/ariane/styles/<slug>.json``). The
editor's Mapping Styles window (Window menu, Ctrl+Shift+M) and these functions
read and write the same files, so a model added in the Object Browser is
visible to the agent tools straight away and the other way round.

File format (``format: ariane-style-1``)::

	{
	  "format": "ariane-style-1",
	  "name": "wasteland",
	  "description": "what the style looks like / when to use it",
	  "models": [
	    {"id": 12957, "name": "sw_pickupwreck01", "role": "vehicle", "weight": 1.0, "notes": "rusty pickup"}
	  ],
	  "textures": ["rusty_metal", "corrugated_metal"],   # materials.TEXTURE_PRESETS names
	  "avoid": [{"id": 1225, "name": "barrel4", "reason": "explosive"}],
	  "notes": "free text: rules of the style"
	}

``role`` is free text; the editor offers ROLES. ``weight`` is the relative
pick frequency (1 = normal). Styles found in ``tools/agent/styles/`` are
copied into the data directory once (deleting one there keeps it deleted).

Where the data directory is: ``ARIANE_STYLES_DIR``, else
``ARIANE_DATA_DIR/styles``, else the source checkout's Release build
(``bin/win-amd64-d3d9/Release/ariane/styles``).
"""

from __future__ import annotations

import json
import os
import random
import re
import tempfile
from pathlib import Path
from typing import Any, Iterable

FORMAT = "ariane-style-1"
ROLES = ("wall", "shelter", "ruin", "prop", "debris", "vehicle", "vegetation", "ground", "light", "sign", "other")
BUNDLED_DIR = Path(__file__).resolve().parent / "styles"
_REPO_ROOT = Path(__file__).resolve().parents[2]
_seeded_dirs: set[Path] = set()


class StyleNotFound(KeyError):
	def __str__(self):
		return str(self.args[0]) if self.args else "style not found"


# ------------------------------------------------------------------ files
def styles_dir() -> Path:
	"""The shared styles folder (created, and seeded with the bundled styles once)."""
	if os.environ.get("ARIANE_STYLES_DIR"):
		d = Path(os.environ["ARIANE_STYLES_DIR"])
	elif os.environ.get("ARIANE_DATA_DIR"):
		d = Path(os.environ["ARIANE_DATA_DIR"]) / "styles"
	else:
		d = _REPO_ROOT / "bin" / "win-amd64-d3d9" / "Release" / "ariane" / "styles"
		if not d.parent.is_dir():	# another platform's build
			for cand in sorted(_REPO_ROOT.glob("bin/*/Release/ariane")):
				d = cand / "styles"
				break
	d.mkdir(parents=True, exist_ok=True)
	if d not in _seeded_dirs:
		_seeded_dirs.add(d)
		_seed(d)
	return d


def _seed(d: Path) -> None:
	marker = d / ".seeded"
	try:
		done = set(json.loads(marker.read_text(encoding="utf-8")))
	except (OSError, ValueError):
		done = set()
	changed = False
	for src in sorted(BUNDLED_DIR.glob("*.json")):
		if src.name in done:
			continue
		dst = d / src.name
		if not dst.exists():
			dst.write_bytes(src.read_bytes())
		done.add(src.name)
		changed = True
	if changed:
		marker.write_text(json.dumps(sorted(done)), encoding="utf-8")


def slug(name: str) -> str:
	"""File name for a style name (same rule as the editor): lowercase a-z 0-9 - _, others -> _."""
	s = re.sub(r"[^a-z0-9_-]+", "_", (name or "").strip().lower()).strip("_")
	return s[:48] or "style"


def _path(name: str) -> Path:
	return styles_dir() / f"{slug(name)}.json"


def _normalise(data: dict, fallback_name: str) -> dict:
	models = []
	seen = set()
	for m in data.get("models") or []:
		if isinstance(m, int): m = {"id": m}
		try: mid = int(m["id"])
		except (KeyError, TypeError, ValueError): continue
		if mid in seen: continue
		seen.add(mid)
		models.append({"id": mid, "name": str(m.get("name") or ""), "role": str(m.get("role") or ""),
		               "weight": float(m.get("weight", 1.0) if m.get("weight") is not None else 1.0),
		               "notes": str(m.get("notes") or "")})
	avoid = []
	for a in data.get("avoid") or []:
		if isinstance(a, int): a = {"id": a}
		try: avoid.append({"id": int(a["id"]), "name": str(a.get("name") or ""), "reason": str(a.get("reason") or "")})
		except (KeyError, TypeError, ValueError): continue
	return {"format": FORMAT, "name": str(data.get("name") or fallback_name),
	        "description": str(data.get("description") or ""), "models": models,
	        "textures": [str(t) for t in data.get("textures") or []], "avoid": avoid,
	        "notes": str(data.get("notes") or "")}


def _write(style: dict) -> Path:
	path = _path(style["name"])
	text = json.dumps(_normalise(style, style["name"]), indent=1, ensure_ascii=False) + "\n"
	fd, tmp = tempfile.mkstemp(prefix=".tmp-", suffix=".json", dir=path.parent)
	with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
		f.write(text)
	os.replace(tmp, path)	# atomic: the editor never reads a half-written file
	return path


# ------------------------------------------------------------------ read
def list_styles() -> list[dict]:
	"""Every style: name, description, model count and models per role."""
	out = []
	for p in sorted(styles_dir().glob("*.json")):
		if p.name.startswith("."): continue
		try: s = _normalise(json.loads(p.read_text(encoding="utf-8")), p.stem)
		except (OSError, ValueError): continue
		roles: dict[str, int] = {}
		for m in s["models"]: roles[m["role"] or "-"] = roles.get(m["role"] or "-", 0) + 1
		out.append({"name": s["name"], "file": p.name, "description": s["description"],
		            "models": len(s["models"]), "roles": roles, "textures": len(s["textures"])})
	return out


def get_style(name: str) -> dict:
	"""The full style (raises KeyError if there is none)."""
	p = _path(name)
	if not p.exists():
		for cand in styles_dir().glob("*.json"):	# match the display name too
			try: data = json.loads(cand.read_text(encoding="utf-8"))
			except (OSError, ValueError): continue
			if str(data.get("name", "")).lower() == name.lower():
				return {**_normalise(data, cand.stem), "file": cand.name}
		raise StyleNotFound(f"no style named {name!r} (have: {', '.join(s['name'] for s in list_styles()) or 'none'})")
	return {**_normalise(json.loads(p.read_text(encoding="utf-8")), p.stem), "file": p.name}


def show_style(name: str) -> dict:
	"""get_style plus models grouped by role and the texture presets resolved (for agents)."""
	s = get_style(name)
	by_role: dict[str, list[dict]] = {}
	for m in s["models"]:
		by_role.setdefault(m["role"] or "unassigned", []).append(m)
	try:
		from . import materials
	except ImportError:
		import materials
	textures = {t: materials.TEXTURE_PRESETS.get(t, {"missing": True}) for t in s["textures"]}
	return {**s, "by_role": by_role, "texture_presets": textures,
	        "hint": "choose models from by_role (weight = how often), retexture with the listed presets, "
	                "never use the avoid list"}


# ------------------------------------------------------------------ write
def create_style(name: str, description: str = "", notes: str = "") -> dict:
	if _path(name).exists(): raise ValueError(f"style {name!r} already exists")
	style = {"name": name.strip(), "description": description, "notes": notes, "models": [], "textures": [], "avoid": []}
	_write(style)
	return get_style(name)


def update_style(name: str, *, description: str | None = None, notes: str | None = None,
                 textures: list[str] | None = None, add_textures: Iterable[str] = (),
                 avoid: list | None = None, add_avoid: Iterable = ()) -> dict:
	s = get_style(name)
	if description is not None: s["description"] = description
	if notes is not None: s["notes"] = notes
	if textures is not None: s["textures"] = list(textures)
	for t in add_textures:
		if t not in s["textures"]: s["textures"].append(t)
	if avoid is not None: s["avoid"] = list(avoid)
	have = {a["id"] for a in s["avoid"]}
	for a in add_avoid:
		a = {"id": a} if isinstance(a, int) else a
		if int(a["id"]) not in have: s["avoid"].append(a); have.add(int(a["id"]))
	_write(s)
	return get_style(s["name"])


def delete_style(name: str) -> dict:
	s = get_style(name)
	(styles_dir() / s["file"]).unlink()
	return {"ok": True, "deleted": s["name"]}


def rename_style(name: str, new_name: str) -> dict:
	s = get_style(name)
	if slug(new_name) != slug(s["name"]) and _path(new_name).exists():
		raise ValueError(f"style {new_name!r} already exists")
	old = styles_dir() / s["file"]
	s["name"] = new_name.strip()
	_write(s)
	if old != _path(new_name): old.unlink(missing_ok=True)
	return get_style(new_name)


def add_models(name: str, models: Iterable, *, role: str | None = None, weight: float | None = None,
               notes: str | None = None, create: bool = True, description: str = "", service=None,
               allow_hazards: bool = False) -> dict:
	"""Add (or update) models. ``models`` are ids or dicts {id, role, weight, notes}; role / weight / notes
	apply to plain ids and fill gaps in dicts. With ``service`` each id is checked with probe_asset (must be
	defined) and explosive props from the asset index are refused unless allow_hazards."""
	try: s = get_style(name)
	except StyleNotFound:
		if not create: raise
		s = create_style(name, description)
	index = {m["id"]: m for m in s["models"]}
	avoid = {a["id"] for a in s["avoid"]}
	added, updated, rejected = [], [], []
	for m in models:
		m = {"id": int(m)} if not isinstance(m, dict) else dict(m)
		mid = int(m["id"])
		if mid in avoid:
			rejected.append({"id": mid, "reason": "on the style's avoid list"}); continue
		if service is not None:
			try: probe = service.probe_asset(mid)
			except Exception as e:	# engine down etc.: keep going unverified
				probe = {"defined": True, "unverified": str(e)}
			if not probe.get("defined"):
				rejected.append({"id": mid, "reason": "model is not defined in this install"}); continue
			if not m.get("name") and probe.get("name"): m["name"] = probe["name"]
			hazard = _hazard(service, mid)
			if "explosive" in hazard and not allow_hazards:
				rejected.append({"id": mid, "reason": f"hazard: {hazard}"}); continue
		entry = index.get(mid)
		if entry is None:
			entry = {"id": mid, "name": "", "role": "", "weight": 1.0, "notes": ""}
			s["models"].append(entry); index[mid] = entry; added.append(mid)
		else:
			updated.append(mid)
		for key, default in (("role", role), ("weight", weight), ("notes", notes)):
			value = m.get(key, default)
			if value is not None: entry[key] = value
		if m.get("name"): entry["name"] = m["name"]
	_write(s)
	return {"ok": True, "style": s["name"], "added": added, "updated": updated, "rejected": rejected,
	        "models": len(s["models"])}


def _hazard(service, mid: int) -> str:
	try:
		asset = service.assets.inspect(mid) or {}
		return str(asset.get("hazard") or "")
	except Exception:
		return ""


def remove_models(name: str, ids: Iterable[int]) -> dict:
	s = get_style(name)
	ids = {int(i) for i in ids}
	before = len(s["models"])
	s["models"] = [m for m in s["models"] if m["id"] not in ids]
	_write(s)
	return {"ok": True, "style": s["name"], "removed": before - len(s["models"]), "models": len(s["models"])}


# ------------------------------------------------------------------ selection
def _roles(role) -> set[str] | None:
	if role is None or role == "" or role == "any": return None
	if isinstance(role, str): role = role.split(",")
	return {r.strip().lower() for r in role if r.strip()}


def style_models(name: str, role=None) -> list[dict]:
	"""The style's usable models (avoid list removed), optionally only these roles (str, 'a,b' or list)."""
	s = get_style(name)
	roles = _roles(role)
	avoid = {a["id"] for a in s["avoid"]}
	return [m for m in s["models"] if m["id"] not in avoid and m["weight"] > 0
	        and (roles is None or (m["role"] or "").lower() in roles)]


def models_and_weights(name: str, role=None) -> tuple[list[int], list[float]]:
	ms = style_models(name, role)
	if not ms:
		raise ValueError(f"style {name!r} has no models" + (f" with role {role!r}" if role else ""))
	return [m["id"] for m in ms], [m["weight"] for m in ms]


def pick(name: str, role=None, count: int = 1, seed: int | None = None) -> dict:
	"""Weighted pick of `count` models: distinct while the pool lasts, then repeats."""
	pool = style_models(name, role)
	if not pool:
		raise ValueError(f"style {name!r} has no models" + (f" with role {role!r}" if role else ""))
	rng = random.Random(seed)
	left = list(pool)
	picked = []
	for _ in range(max(0, int(count))):
		src = left or pool
		m = rng.choices(src, [x["weight"] for x in src])[0]
		if left: left.remove(m)
		picked.append(m)
	return {"ok": True, "style": get_style(name)["name"], "role": role, "picked": picked,
	        "ids": [m["id"] for m in picked]}


def apply_to_models(models: list[int] | None, weights: list[float] | None, style: str | None,
                    role=None, mode: str = "prefer") -> tuple[list[int], list[float]]:
	"""Model list for a placement tool. No models -> the style's (by role). mode 'limit' keeps only style
	models; 'prefer' multiplies style models' weights by 3 x their style weight. Avoid-list models are
	always dropped."""
	if not style:
		if not models: raise ValueError("pass models or a style")
		return list(models), list(weights or [1.0] * len(models))
	s = get_style(style)
	avoid = {a["id"] for a in s["avoid"]}
	if not models:
		return models_and_weights(style, role)
	weights = list(weights or [1.0] * len(models))
	in_style = {m["id"]: m["weight"] for m in style_models(style, role)}
	out_m, out_w = [], []
	for m, w in zip(models, weights):
		if m in avoid: continue
		if m in in_style: out_m.append(m); out_w.append(w * 3.0 * in_style[m])
		elif mode != "limit": out_m.append(m); out_w.append(w)
	if not out_m:
		raise ValueError(f"none of the models are in style {style!r}" + (f" (role {role!r})" if role else ""))
	return out_m, out_w


def filter_assets(assets: list[dict], style: str, role=None, mode: str = "prefer", query: str = "",
                  inspect=None, limit: int | None = None) -> list[dict]:
	"""Re-rank asset search results by a style. 'prefer': style models first (tagged style_role/style_weight).
	'limit': only style models. Style models the text search missed are added when their name / notes / role match
	the query (needs `inspect(id)` -> asset dict)."""
	s = get_style(style)
	avoid = {a["id"] for a in s["avoid"]}
	members = {m["id"]: m for m in style_models(style, role)}
	tagged, rest = [], []
	for a in assets:
		aid = int(a.get("id", -1))
		if aid in avoid: continue
		if aid in members:
			m = members[aid]
			tagged.append({**a, "style": s["name"], "style_role": m["role"], "style_weight": m["weight"],
			               "style_notes": m["notes"]})
		elif mode != "limit":
			rest.append(a)
	if inspect is not None:
		have = {int(a["id"]) for a in tagged}
		words = [w for w in (query or "").lower().split() if w]
		for m in members.values():
			if m["id"] in have: continue
			text = f"{m['name']} {m['notes']} {m['role']}".lower()
			if words and not any(w in text for w in words): continue
			a = inspect(m["id"]) or {"id": m["id"], "name": m["name"]}
			tagged.append({**a, "style": s["name"], "style_role": m["role"], "style_weight": m["weight"],
			               "style_notes": m["notes"]})
	tagged.sort(key=lambda a: -a["style_weight"])
	out = tagged + rest
	return out[:limit] if limit else out
