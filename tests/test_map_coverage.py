#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from map_coverage import audit_map


class CoverageTests(unittest.TestCase):
    def test_placed_npc_is_not_a_complete_quest(self):
        def actor(name, kind="Enemy", model="c0000", talk=1):
            return {"Name": name, "kind": kind, "ModelName": model, "values": {"TalkID": talk, "EntityID": 6001}}
        plan = {"map": "m10_01_00_00", "actors": [actor("solaire"), actor("havel", talk=0),
                actor("unused", "DummyEnemy"), actor("lamp-controller", model="c1000")],
                "objects": [], "treasures": [{"Name": "key", "values": {"ItemLots": [10]}}],
                "eventPrograms": [{"ID": 100, "SHA256": "source"}]}
        converted = {"characters": [{"SourceName": "solaire", "TargetModel": "c8905"}],
                     "merchants": [{"SourceName": "merchant", "TargetModel": "c8904"}],
                     "items": {"Pickups": [{"SourceName": "key"}], "Skipped": []}}
        result = audit_map(plan, converted)
        self.assertEqual(result["missingRuntimeActors"], 1)
        self.assertEqual(result["actors"][0]["appearance"], "converted")
        self.assertEqual(result["actors"][0]["quest"], "unverified")
        self.assertEqual(result["treasures"][0]["appearance"], "converted")
        self.assertFalse(result["complete"])

    def test_old_inventory_does_not_look_complete(self):
        self.assertFalse(audit_map({"actors": []}, {})["complete"])

    def test_cave_slice_does_not_claim_the_abyss_or_new_londo(self):
        plan = {"map": "m16_00_00_00", "actors": [{"Name": "four-kings", "ModelName": "c5390", "kind": "Enemy", "values": {}}],
                "treasures": [], "eventPrograms": [], "objects": [
                    {"Name": "cave", "ModelName": "o0200", "kind": "Object"},
                    {"Name": "abyss", "ModelName": "o0200", "kind": "Object"}]}
        result = audit_map(plan, {"sourceSlice": ["cave"]}, [{"SourceMap": plan["map"], "SourceBonfirePart": "cave"}])
        self.assertEqual(result["actors"][0]["appearance"], "outside-import-slice")
        self.assertTrue(result["bonfires"][0]["included"])
        self.assertFalse(result["bonfires"][1]["included"])
        self.assertFalse(result["complete"])


if __name__ == "__main__":
    unittest.main()
