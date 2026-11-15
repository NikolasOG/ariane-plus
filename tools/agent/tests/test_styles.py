import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

AGENT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AGENT_DIR))
import styles


class StylesTests(unittest.TestCase):
	def setUp(self):
		self.tmp = tempfile.TemporaryDirectory()
		self.old = os.environ.get("ARIANE_STYLES_DIR")
		os.environ["ARIANE_STYLES_DIR"] = self.tmp.name

	def tearDown(self):
		if self.old is None: os.environ.pop("ARIANE_STYLES_DIR", None)
		else: os.environ["ARIANE_STYLES_DIR"] = self.old
		self.tmp.cleanup()

	def test_bundled_wasteland_is_seeded_once(self):
		names = [s["name"] for s in styles.list_styles()]
		self.assertIn("wasteland", names)
		styles.delete_style("wasteland")
		styles._seeded_dirs.clear()
		self.assertNotIn("wasteland", [s["name"] for s in styles.list_styles()])

	def test_wasteland_excludes_explosives(self):
		s = styles.get_style("wasteland")
		ids = {m["id"] for m in s["models"]}
		self.assertTrue({1225, 1370}.isdisjoint(ids))
		self.assertEqual({a["id"] for a in s["avoid"]}, {1225, 1370})

	def test_slug_matches_editor_rule(self):
		self.assertEqual(styles.slug("  Old Farm!! v2 "), "old_farm_v2")
		self.assertEqual(styles.slug("***"), "style")

	def test_add_remove_pick_and_rename(self):
		styles.add_models("Test Set", [10, 11], role="wall", description="d")
		styles.add_models("Test Set", [{"id": 12, "role": "shelter", "weight": 2}])
		styles.update_style("Test Set", add_avoid=[13])
		r = styles.add_models("Test Set", [13])
		self.assertEqual(r["rejected"][0]["id"], 13)
		self.assertEqual(styles.pick("Test Set", "wall", 2, seed=1)["ids"].__len__(), 2)
		self.assertEqual(set(styles.pick("Test Set", "wall", 2, seed=1)["ids"]), {10, 11})
		self.assertEqual(styles.pick("test set", "shelter", 3)["ids"], [12, 12, 12])
		styles.remove_models("Test Set", [10])
		renamed = styles.rename_style("Test Set", "Other")
		self.assertEqual(renamed["file"], "other.json")
		self.assertFalse((Path(self.tmp.name) / "test_set.json").exists())
		data = json.loads((Path(self.tmp.name) / "other.json").read_text(encoding="utf-8"))
		self.assertEqual([m["id"] for m in data["models"]], [11, 12])

	def test_apply_to_models_modes(self):
		self.assertEqual(styles.apply_to_models(None, None, "wasteland", "shelter")[0], [3414, 3418])
		models, weights = styles.apply_to_models([3414, 999, 1225], None, "wasteland", mode="prefer")
		self.assertEqual(models, [3414, 999])
		self.assertGreater(weights[0], weights[1])
		self.assertEqual(styles.apply_to_models([3414, 999], None, "wasteland", mode="limit")[0], [3414])

	def test_filter_assets_limit_adds_missing_style_models(self):
		found = [{"id": 999, "name": "other"}, {"id": 12957, "name": "sw_pickupwreck01"}]
		out = styles.filter_assets(found, "wasteland", "vehicle", "limit", query="wreck",
		                           inspect=lambda i: {"id": i, "name": str(i)})
		self.assertEqual({a["id"] for a in out}, {12957, 3593, 3594})
		self.assertTrue(all(a["style_role"] == "vehicle" for a in out))


if __name__ == "__main__":
	unittest.main()
