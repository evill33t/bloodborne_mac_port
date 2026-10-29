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
else:
    out=Path(args['--output']);out.mkdir();(out/'conversion.json').write_text(json.dumps({'spawnOffset':args.get('--spawn-offset')}))
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
