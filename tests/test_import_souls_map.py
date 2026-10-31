#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise import-job publication and failure boundaries without game assets."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/import_souls_map.py"


class ImportJobs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source, self.native = self.base / "source", self.base / "native"
        for directory in (self.source, self.native):
            directory.mkdir()
            (directory / "original").write_bytes(b"original asset")
        self.helper = self.base / "helper.py"
        self.helper.write_text('''import json,sys
from pathlib import Path
command=sys.argv[1];args=dict(zip(sys.argv[2::2],sys.argv[3::2]))
if command=='plan-map': print(json.dumps({'sourceMapSha256':'fixture'}))
elif command=='finalize-destination':
    out=Path(args['--output'])
    if (Path(args['--root'])/'fail-travel').exists():sys.exit(11)
    (out/'destination-travel.json').write_text(json.dumps({'composed':(out/'dvdroot_ps4/shared').read_text()}))
else:
    out=Path(args['--output']);out.mkdir();(out/'conversion.json').write_text(json.dumps({'spawnOffset':args.get('--spawn-offset'),'itemPolicy':args.get('--item-policy'),'destination':args.get('--destination'),'target':args['--target-map']}))
    (out/'artifact').write_text('complete')
    data=out/'dvdroot_ps4';data.mkdir()
    previous=Path(args['--native-overlay'])/'dvdroot_ps4/shared' if '--native-overlay' in args else None
    before=previous.read_text() if previous else ''
    (data/'shared').write_text(before+args['--map']+';')
    if args['--map']=='m10_01_00_00' and (Path(args['--root'])/'fail-burg').exists():sys.exit(10)
    if (Path(args['--root'])/'fail').exists():sys.exit(9)
''')
        self.output = self.base / "Mods" / "TestMap"

    def run_job(self, *extra):
        result = subprocess.run([sys.executable, str(SCRIPT), "--profile", "DS1R",
            "--root", str(self.source), "--native-root", str(self.native), "--map", "m10_02_00_00",
            "--output", str(self.output), "--helper", str(self.helper), "--dotnet", sys.executable,
            *map(str, extra)], text=True, capture_output=True)
        for directory in (self.source, self.native):
            self.assertEqual((directory / "original").read_bytes(), b"original asset")
        return result

    def test_publish_complete_job_then_refuse_overwrite(self):
        first = self.run_job()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual((self.output / "artifact").read_text(), "complete")
        self.assertIsNone(json.loads((self.output / "conversion.json").read_text())["itemPolicy"])
        job = json.loads((self.output / "import-job.json").read_text())
        self.assertEqual(job["source_anchor"], "-51.8,-60.6,55.7")
        self.assertEqual(json.loads((self.output / "import-manifest.json").read_text())["source_map_sha256"], "fixture")
        before = {str(p.relative_to(self.output)): p.read_bytes() for p in self.output.rglob("*") if p.is_file()}
        second = self.run_job()
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(before, {str(p.relative_to(self.output)): p.read_bytes() for p in self.output.rglob("*") if p.is_file()})

    def test_backend_failure_does_not_publish_partial_output(self):
        (self.source / "fail").touch()
        result = self.run_job()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_item_conversion_is_opt_in_and_saved_for_reruns(self):
        first = self.run_job("--import-items", "--attach-undead-burg")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(json.loads((self.output / "import-job.json").read_text())["import_items"])
        for file in (self.output / "conversion.json", self.output / "maps/m10_01_00_00/conversion.json"):
            self.assertEqual(json.loads(file.read_text())["itemPolicy"], "ds1-bloodborne")

    def test_item_conversion_rejects_unsupported_profile(self):
        result = self.run_job("--profile", "DS2S", "--import-items")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_game_folder_is_never_an_output(self):
        result = self.run_job("--output", self.source / "Mods")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.source / "Mods").exists())

    def test_case_alias_cannot_write_plan_metadata_into_source(self):
        alias = self.source.with_name(self.source.name.upper())
        if not alias.exists() or not alias.samefile(self.source):
            self.skipTest("Requires a case-insensitive filesystem")
        result = self.run_job("--plan-only", "--output", alias / "review-metadata")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.source / "review-metadata").exists())

    def test_dangling_output_symlink_is_refused(self):
        self.output.parent.mkdir()
        destination = self.base / "unexpected-output"
        self.output.symlink_to(destination, target_is_directory=True)
        result = self.run_job("--plan-only")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.output.is_symlink())
        self.assertFalse(destination.exists())

    def test_relative_smithbox_is_saved_as_absolute(self):
        result = self.run_job("--plan-only", "--smithbox", ".")
        self.assertEqual(result.returncode, 0, result.stderr)
        job = json.loads((self.output / "import-job.json").read_text())
        self.assertEqual(job["smithbox"], str(Path.cwd().resolve()))

    def test_connected_maps_compose_shared_outputs(self):
        result = self.run_job("--attach-undead-burg", "--spawn-offset=1,2,3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "dvdroot_ps4/shared").read_text(), "m10_02_00_00;m10_01_00_00;")
        conversion = json.loads((self.output / "conversion.json").read_text())
        self.assertEqual(conversion["connectedMaps"][0]["target_map"], "m24_00_00_00")
        self.assertEqual(conversion["spawnOffset"], "1,2,3")
        secondary = json.loads((self.output / "maps/m10_01_00_00/conversion.json").read_text())
        self.assertIsNone(secondary["spawnOffset"])
        self.assertTrue((self.output / "maps/m10_01_00_00/source-plan.json").is_file())

    def test_secondary_failure_does_not_publish_primary(self):
        (self.source / "fail-burg").touch()
        result = self.run_job("--attach-undead-burg")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_additional_destination_finalizes_both_composed_maps(self):
        result = self.run_job("--destination", "additional", "--attach-undead-burg", "--import-items")
        self.assertEqual(result.returncode, 0, result.stderr)
        job = json.loads((self.output / "import-job.json").read_text())
        self.assertEqual(job["destination"], "additional")
        self.assertEqual(job["target_map"], "m40_02_00_00")
        self.assertEqual(job["connected_maps"], [{"source_map": "m10_01_00_00", "target_map": "m40_03_00_00"}])
        for path, target in (("conversion.json", "m40_02_00_00"),
                             ("maps/m10_01_00_00/conversion.json", "m40_03_00_00")):
            report = json.loads((self.output / path).read_text())
            self.assertEqual(report["destination"], "additional")
            self.assertEqual(report["target"], target)
        self.assertEqual(json.loads((self.output / "destination-travel.json").read_text())["composed"],
                         "m10_02_00_00;m10_01_00_00;")
        self.assertEqual(json.loads((self.output / "import-manifest.json").read_text())["destination"], "additional")

    def test_switching_saved_replacement_job_migrates_fixed_allocation(self):
        self.assertEqual(self.run_job("--attach-undead-burg").returncode, 0)
        config = self.output / "import-job.json"
        migrated = self.base / "Mods" / "Additional"
        result = self.run_job("--config", config, "--destination", "additional", "--output", migrated)
        self.assertEqual(result.returncode, 0, result.stderr)
        job = json.loads((migrated / "import-job.json").read_text())
        self.assertEqual(job["target_map"], "m40_02_00_00")
        self.assertEqual(job["connected_maps"][0]["target_map"], "m40_03_00_00")
        rerun = self.base / "Mods" / "Rerun"
        result = self.run_job("--config", migrated / "import-job.json", "--output", rerun)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((rerun / "destination-travel.json").is_file())

    def test_travel_failure_never_publishes_a_partial_destination(self):
        (self.source / "fail-travel").touch()
        result = self.run_job("--destination", "additional", "--attach-undead-burg")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_additional_destination_rejects_arbitrary_map_and_profile(self):
        for extra in (("--target-map", "m21_00_00_00"), ("--profile", "DS2S"),
                      ("--map", "m10_01_00_00")):
            with self.subTest(extra=extra):
                result = self.run_job("--destination", "additional", *extra)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_additional_plan_avoids_assets_and_travel_finalizer(self):
        (self.source / "fail-travel").touch()
        result = self.run_job("--destination", "additional", "--attach-undead-burg", "--plan-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.output / "destination-travel.json").exists())
        self.assertFalse((self.output / "dvdroot_ps4").exists())

    def test_plan_only_avoids_conversion(self):
        (self.source / "fail").touch()
        result = self.run_job("--plan-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "source-plan.json").is_file())
        self.assertFalse((self.output / "conversion.json").exists())

    def test_connected_plan_inventories_both_maps_without_conversion(self):
        (self.source / "fail").touch()
        result = self.run_job("--plan-only", "--attach-undead-burg")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "maps/m10_01_00_00/source-plan.json").is_file())
        self.assertFalse((self.output / "dvdroot_ps4").exists())


if __name__ == "__main__":
    unittest.main()
