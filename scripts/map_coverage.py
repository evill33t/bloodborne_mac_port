#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare authored populations with converted outputs; appearance is not quest coverage."""
import argparse
from collections import Counter
import json
from pathlib import Path


def audit_map(plan, converted, travel=()):
    if "actors" not in plan or "treasures" not in plan or "eventPrograms" not in plan:
        return {"map": plan.get("map"), "status": "inventory-incomplete", "complete": False}
    spawned = {p["SourceName"]: p for p in (converted.get("enemySpawns") or {}).get("Spawns", [])}
    characters = {p["SourceName"]: p for p in converted.get("characters") or [] if p.get("TargetModel")}
    merchant_reports = converted.get("merchants") or []
    if isinstance(merchant_reports, dict):
        merchant_reports = merchant_reports.get("Reports", [])
    merchants = {p["SourceName"]: p for p in merchant_reports}
    omissions = {p["SourceName"]: p["Reason"] for p in (converted.get("enemySpawns") or {}).get("Skipped", [])}
    actors = []
    scope = converted.get("sourceSlice")
    for actor in plan["actors"]:
        name = actor["Name"]
        fields = actor["values"]
        dummy = actor["kind"] == "DummyEnemy"
        npc = fields.get("CharaInitID", -1) >= 0 or fields.get("TalkID", -1) > 0
        service = actor["ModelName"] == "c1000"
        visual = characters.get(name) or merchants.get(name) or spawned.get(name)
        excluded = scope is not None and name not in scope
        actors.append({"name": name, "model": actor["ModelName"], "entity": fields.get("EntityID"),
                       "kind": "outside-import-slice" if excluded else "source-dummy" if dummy else "service" if service else "npc" if npc else "enemy",
                       "appearance": "outside-import-slice" if excluded else "converted" if visual else "missing",
                       "quest": "unverified" if npc and not dummy and not excluded else "not-applicable",
                       "target": visual.get("TargetModel") if visual else None,
                       "omission": omissions.get(name)})
    pickups = {p["SourceName"] for p in (converted.get("items") or {}).get("Pickups", [])}
    skipped = {p["SourceName"]: p["Reason"] for p in (converted.get("items") or {}).get("Skipped", [])}
    treasures = [{"name": p["Name"], "appearance": "outside-import-slice" if scope is not None and p["values"].get("TreasurePartName") not in scope else "converted" if p["Name"] in pickups else "missing",
                  "source": p["values"], "omission": skipped.get(p["Name"])} for p in plan["treasures"]]
    lamp_parts = {p["SourceBonfirePart"] for p in travel if p["SourceMap"] == plan["map"]}
    fires = [{"name": p["Name"], "kind": p["kind"], "included": scope is None or p["Name"] in scope, "lamp": p["Name"] in lamp_parts}
             for p in plan["objects"] if p["ModelName"] == "o0200"]
    missing = [p for p in actors if p["kind"] not in ("source-dummy", "service", "outside-import-slice") and p["appearance"] == "missing"]
    return {"map": plan["map"], "target": converted.get("target"), "complete": False,
            "status": "partial", "sourceSlice": scope, "authoredCounts": dict(Counter(p["kind"] for p in actors)),
            "missingRuntimeActors": len(missing), "missingNpcModels": sorted({p["model"] for p in missing if p["kind"] == "npc"}),
            "actors": actors, "treasures": treasures, "bonfires": fires,
            "sourceEventPrograms": plan["eventPrograms"],
            "questState": "Per-actor quest transition and cross-map persistence verification remains required; placed actors and source event inventories do not establish this."}


def audit_folder(root):
    root = Path(root)
    reports = [(root / "source-plan.json", root / "conversion.json")]
    conversion = json.loads((root / "conversion.json").read_text())
    for attached in conversion.get("connectedMaps", []):
        folder = root / "maps" / attached["source_map"]
        reports.append((folder / "source-plan.json", folder / "conversion.json"))
    travel_path = root / "destination-travel.json"
    travel = json.loads(travel_path.read_text()).get("Destinations", []) if travel_path.exists() else []
    maps = [audit_map(json.loads(plan.read_text()), json.loads(report.read_text()), travel) for plan, report in reports]
    return {"schema": "bbmap-coverage-1", "complete": all(m["complete"] for m in maps), "maps": maps}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mod", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    result = audit_folder(args.mod)
    content = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(content)
    else:
        print(content, end="")
    raise SystemExit(1 if args.require_complete and not result["complete"] else 0)
