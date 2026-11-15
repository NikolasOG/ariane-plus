"""Extract files from a GTA SA IMG (v2, "VER2") archive.

    python img_extract.py <gta3.img> <out_dir> name.dff name.txd ...
    python img_extract.py <gta3.img> --list [substring]

Handy for making a custom model from a stock one (copy its dff/txd, edit,
then list it in <server>/models/artconfig.txt) or for test fixtures.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def entries(img: str | Path) -> dict[str, tuple[int, int]]:
	with open(img, "rb") as f:
		head = f.read(8)
		if head[:4] != b"VER2":
			raise ValueError("not a VER2 (GTA SA) img archive")
		count = struct.unpack("<I", head[4:8])[0]
		out = {}
		for _ in range(count):
			offset, streaming, archive, name = struct.unpack("<IHH24s", f.read(32))
			size = (archive or streaming) * 2048
			out[name.split(b"\0", 1)[0].decode("latin-1").lower()] = (offset * 2048, size)
		return out


def extract(img: str | Path, names: list[str], out_dir: str | Path) -> list[str]:
	table = entries(img)
	out_dir = Path(out_dir)
	out_dir.mkdir(parents=True, exist_ok=True)
	written = []
	with open(img, "rb") as f:
		for name in names:
			key = name.lower()
			if key not in table:
				raise KeyError(f"{name} not in {img}")
			offset, size = table[key]
			f.seek(offset)
			(out_dir / name).write_bytes(f.read(size))
			written.append(str(out_dir / name))
	return written


if __name__ == "__main__":
	if len(sys.argv) >= 3 and sys.argv[2] == "--list":
		sub = sys.argv[3].lower() if len(sys.argv) > 3 else ""
		for n in sorted(entries(sys.argv[1])):
			if sub in n:
				print(n)
	elif len(sys.argv) >= 4:
		for path in extract(sys.argv[1], sys.argv[3:], sys.argv[2]):
			print(path)
	else:
		print(__doc__)
