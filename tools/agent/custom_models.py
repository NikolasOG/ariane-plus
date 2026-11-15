"""SA-MP custom models (open.mp artwork): AddSimpleModel in <server>/models/artconfig.txt.

    AddSimpleModel(virtualworld, baseid, newid, "file.dff", "file.txd");

open.mp sends the files to players; ``newid`` is negative (-1000 .. -30000) and
``baseid`` is the stock model whose collision the client uses. Ariane reads the
same artconfig.txt and registers every entry as a runtime object definition
at an internal id (30000+). Place objects with the internal ``model_id``;
scene items carry ``samp_model_id`` and sync/export write the SA-MP id.

Typical flow::

    add_simple_model(service, server_dir, "my_wall.dff", "my_wall.txd", base_id=19353)
    -> {"samp_id": -1000, "model_id": 30000, ...}
    place model 30000 like any other object
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path

LINE = re.compile(r"AddSimpleModel(?:Timed)?\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"")


def list_custom_models(service) -> dict:
	return service.engine("custom_models")


def reload_custom_models(service, models_dir: str | None = None) -> dict:
	"""Re-read artconfig.txt (default <server>/models) and register new entries."""
	return service.engine("reload_custom_models", [models_dir or "-"])


def resolve(service, samp_id: int) -> int | None:
	"""SA-MP custom id (-2000) -> internal model id to place, or None."""
	for m in list_custom_models(service).get("models", []):
		if m["samp_id"] == samp_id and m.get("ok"):
			return m["model_id"]
	return None


def read_artconfig(models_dir: str | Path) -> list[dict]:
	path = Path(models_dir) / "artconfig.txt"
	if not path.exists():
		return []
	out = []
	for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
		if line.strip().startswith("//"):
			continue
		m = LINE.search(line)
		if m:
			out.append({"world": int(m[1]), "base_id": int(m[2]), "samp_id": int(m[3]), "dff": m[4], "txd": m[5]})
	return out


def next_free_id(models_dir: str | Path, start: int = -1000) -> int:
	used = {e["samp_id"] for e in read_artconfig(models_dir)}
	i = start
	while i in used:
		i -= 1
	if i < -30000:
		raise ValueError("no free custom model ids (-1000 .. -30000)")
	return i


def add_simple_model(service, server_dir: str | Path, dff: str | Path, txd: str | Path, *, base_id: int,
                     samp_id: int | None = None, world: int = -1, reload: bool = True) -> dict:
	"""Copy a dff/txd pair into <server>/models, append an AddSimpleModel line
	and (re)load it in Ariane. Returns the registry entry (samp_id, model_id).
	base_id gives the collision and object flags in game - pick a stock model
	with a similar shape/size (asset search / size-match help)."""
	models = Path(server_dir) / "models"
	models.mkdir(parents=True, exist_ok=True)
	dff, txd = Path(dff), Path(txd)
	for src in (dff, txd):
		if not src.exists():
			raise FileNotFoundError(src)
		target = models / src.name
		if target.resolve() != src.resolve():
			shutil.copyfile(src, target)
	if samp_id is None:
		samp_id = next_free_id(models)
	if not -30000 <= samp_id <= -1000:
		raise ValueError("custom model ids are -1000 .. -30000")
	if any(e["samp_id"] == samp_id for e in read_artconfig(models)):
		raise ValueError(f"id {samp_id} is already in artconfig.txt")
	art = models / "artconfig.txt"
	text = art.read_text(encoding="utf-8") if art.exists() else ""
	if text and not text.endswith("\n"):
		text += "\n"
	text += f'AddSimpleModel({world}, {base_id}, {samp_id}, "{dff.name}", "{txd.name}");\n'
	art.write_text(text, encoding="utf-8")
	result = {"ok": True, "samp_id": samp_id, "artconfig": str(art)}
	if reload:
		listing = reload_custom_models(service, str(models))
		entry = next((m for m in listing.get("models", []) if m["samp_id"] == samp_id), None)
		result["entry"] = entry
		result["model_id"] = entry.get("model_id") if entry else None
		if entry and not entry.get("ok"):
			result["ok"] = False
			result["error"] = entry.get("error")
	return result
