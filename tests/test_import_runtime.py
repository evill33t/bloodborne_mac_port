# SPDX-License-Identifier: GPL-2.0-or-later
"""Launch-scoped heap requirements; no game execution or original asset writes.

The installed-executable check is optional. Set BB_IMPORT_RUNTIME_ELF to the
verified decoded 1.09 ELF, or keep it in an ancestor checkout's out/mac-settings-*.
"""
from paths import ROOT
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import import_runtime as runtime
import patches


EMPTY = {'schema': 'bbmap-runtime-1', 'requirements': []}
REQUIRED = {'schema': 'bbmap-runtime-1',
            'requirements': ['bloodborne-1.09-connected-map-main-768']}
OLD_HEAP = 524 * 1024 * 1024
NEW_HEAP = 768 * 1024 * 1024
HEAP_ADDRESS = 0x4736dc0


def synthetic_elf(value=OLD_HEAP, address=HEAP_ADDRESS, size=8, kind=1):
    # Deliberately different file/virtual offsets catch treating the image
    # address as an ELF file offset. Synthetic profiles are allowed only by
    # test-local hash substitution, never through the public runtime profile.
    blob = bytearray(0x108)
    blob[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', blob, 0x20, 0x40)
    struct.pack_into('<HH', blob, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', blob, 0x40,
                     kind, 6, 0x100, address, 0, size, 0x6000000, 0x1000)
    struct.pack_into('<Q', blob, 0x100, value)
    return bytes(blob)


def patch_records(blob):
    magic, base, count = struct.unpack_from('<8sQQ', blob)
    if magic != b'BBPATCH2' or base != 0x400000:
        raise AssertionError('unexpected patch serialization')
    result, cursor = [], 24
    for _ in range(count):
        offset, size = struct.unpack_from('<QQ', blob, cursor)
        cursor += 16
        result.append((offset, blob[cursor:cursor + size]))
        cursor += size
    if cursor != len(blob):
        raise AssertionError('unexpected patch trailer')
    return result


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.metadata = self.root / 'runtime.json'
        self.metadata.write_text(json.dumps(REQUIRED))

    def test_public_profile_is_fixed_and_merge_is_deduplicated(self):
        self.assertEqual(runtime.SCHEMA, EMPTY['schema'])
        self.assertEqual(runtime.MAIN_768, REQUIRED['requirements'][0])
        self.assertEqual(runtime.MAIN_OFFSET, HEAP_ADDRESS)
        self.assertEqual(runtime.requirements(self.metadata), {runtime.MAIN_768})
        self.assertEqual(runtime.merge([self.metadata, self.metadata,
                                        self.root / 'not-present.json']), REQUIRED)
        self.metadata.write_text(json.dumps(EMPTY))
        self.assertEqual(runtime.requirements(self.metadata), set())
        self.assertEqual(runtime.merge([self.metadata]), EMPTY)

    def test_unknown_profiles_and_unbounded_metadata_reject(self):
        invalid = [None, [], {}, {'schema': EMPTY['schema'], 'requirements': ''},
                   {'schema': 'bbmap-runtime-2', 'requirements': []},
                   {**REQUIRED, 'address': HEAP_ADDRESS},
                   {**REQUIRED, 'requirements': [runtime.MAIN_768, 'custom']},
                   {**REQUIRED, 'requirements': [False]},
                   {**REQUIRED, 'requirements': ['MAIN-1024']},
                   {**REQUIRED, 'requirements': [{}]}]
        for value in invalid:
            with self.subTest(value=value):
                self.metadata.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    runtime.requirements(self.metadata)
        self.metadata.write_text(json.dumps(REQUIRED) + ' ' * 4096)
        with self.assertRaisesRegex(ValueError, 'metadata'):
            runtime.requirements(self.metadata)
        self.metadata.write_text(json.dumps(REQUIRED))
        alias = self.root / 'alias.json'
        alias.symlink_to(self.metadata)
        with self.assertRaisesRegex(ValueError, 'metadata'):
            runtime.requirements(alias)

    def test_no_requirement_never_parses_or_hashes_an_executable(self):
        self.assertEqual(runtime.heap_writes(None, b'not an ELF'), [])
        self.metadata.write_text(json.dumps(EMPTY))
        self.assertEqual(runtime.heap_writes(self.metadata, b'not an ELF'), [])

    def test_hash_guard_rejects_other_executables_before_any_write(self):
        blob = synthetic_elf()
        before = bytes(blob)
        with self.assertRaisesRegex(ValueError, 'verified Bloodborne 1.09'):
            runtime.heap_writes(self.metadata, blob)
        self.assertEqual(blob, before)

    def test_virtual_mapping_qword_and_file_backing_guards(self):
        good = synthetic_elf()
        with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(good).hexdigest()):
            self.assertEqual(runtime.heap_writes(self.metadata, good),
                             [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP))])
        for blob, reason in [(synthetic_elf(value=OLD_HEAP - 1), 'layout differs'),
                             (synthetic_elf(address=HEAP_ADDRESS + 8), 'outside'),
                             (synthetic_elf(size=7), 'outside'),
                             (synthetic_elf(kind=2), 'outside')]:
            with self.subTest(reason=reason, sha=hashlib.sha256(blob).hexdigest()):
                with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(blob).hexdigest()):
                    with self.assertRaisesRegex(ValueError, reason):
                        runtime.heap_writes(self.metadata, blob)

    def test_heap_write_participates_in_existing_conflict_audit(self):
        writes = [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP))]
        for first in (True, False):
            with self.subTest(import_first=first):
                audit = patches.PatchAudit()
                conflict = [(HEAP_ADDRESS + 4, b'\x01')]
                if first:
                    audit.add('Imported map MAIN heap', writes)
                    with self.assertRaisesRegex(ValueError, 'conflicts with Imported map MAIN heap'):
                        audit.add('Other heap patch', conflict)
                else:
                    audit.add('Other heap patch', conflict)
                    with self.assertRaisesRegex(ValueError, 'conflicts with Other heap patch'):
                        audit.add('Imported map MAIN heap', writes)
        audit = patches.PatchAudit()
        audit.add('Same heap', writes)
        self.assertEqual(audit.add('Imported map MAIN heap', writes), writes)


class ModRuntimeCliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = self.root / 'Game'
        (self.game / 'dvdroot_ps4/chr').mkdir(parents=True)
        (self.game / 'dvdroot_ps4/chr/a.dcx').write_bytes(b'original')
        self.mods = self.root / 'mods'
        self.runtime_out = self.root / 'launch/runtime.json'

    def add_mod(self, name='Lordran', layout='dvdroot_ps4/chr', metadata='outer'):
        folder = self.mods / name
        asset = folder / layout / 'a.dcx'
        asset.parent.mkdir(parents=True, exist_ok=True)
        asset.write_bytes(b'imported')
        if metadata:
            base = folder if metadata == 'outer' else asset.parent.parent.parent
            (base / 'runtime-requirements.json').write_text(json.dumps(REQUIRED))
        return folder

    def launch(self, mods=None, enabled='1', online='0', config=None, check=True):
        command = [sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                   '--out', str(self.root / 'overlay'), '--mods-dir', str(mods or self.mods),
                   '--runtime-out', str(self.runtime_out), '--enabled', enabled]
        if config:
            command += ['--config', str(config)]
        result = subprocess.run(command, capture_output=True, text=True, check=check,
                                env={**os.environ, 'BB_ONLINE': online}, timeout=15)
        if check:
            view = Path(result.stdout.strip())
            return json.loads(self.runtime_out.read_text()), (view / 'dvdroot_ps4/chr/a.dcx').read_bytes()
        return result

    def test_named_direct_legacy_and_wrapped_requirements_follow_mounted_assets(self):
        scenarios = [('named', 'dvdroot_ps4/chr', 'outer'),
                     ('direct', 'dvdroot_ps4/chr', 'outer'),
                     ('wrapped-outer', 'app0/dvdroot_ps4/chr', 'outer'),
                     ('wrapped-inner', 'app0/dvdroot_ps4/chr', 'inner'),
                     ('archive-outer', 'Archive v1.2/dvdroot_ps4/chr', 'outer'),
                     ('archive-inner', 'Archive v1.2/dvdroot_ps4/chr', 'inner'),
                     ('bare', 'chr', 'outer')]
        for name, layout, metadata in scenarios:
            with self.subTest(layout=name):
                # Separate roots avoid named-discovery mixing layout cases.
                self.mods = self.root / name
                folder = self.add_mod(layout=layout, metadata=metadata)
                value, assets = self.launch(mods=folder if name == 'direct' else self.mods)
                self.assertEqual(value, REQUIRED)
                self.assertEqual(assets, b'imported')
                for option in ({'enabled': '0'}, {'online': '1'}):
                    self.assertEqual(self.launch(mods=folder if name == 'direct' else self.mods,
                                                 **option), (EMPTY, b'original'))
        self.mods = self.root / 'empty-mods'
        legacy = Path(str(self.game) + '-mods')
        (legacy / 'dvdroot_ps4/chr').mkdir(parents=True)
        (legacy / 'dvdroot_ps4/chr/a.dcx').write_bytes(b'legacy')
        (legacy / 'runtime-requirements.json').write_text(json.dumps(REQUIRED))
        self.assertEqual(self.launch(), (REQUIRED, b'legacy'))
        self.assertEqual(self.launch(enabled='0'), (EMPTY, b'original'))
        self.assertEqual(self.launch(online='1'), (EMPTY, b'original'))
        self.assertEqual((self.game / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'original')

    def test_disabled_online_and_no_mods_overwrite_stale_requirements_with_empty(self):
        folder = self.add_mod()
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'disabled': ['Lordran']}))
        for options in ({'enabled': '0'}, {'online': '1'}, {'config': config},
                        {'mods': self.root / 'missing-mods'}):
            with self.subTest(options=options):
                self.runtime_out.parent.mkdir(parents=True, exist_ok=True)
                self.runtime_out.write_text(json.dumps(REQUIRED))
                self.assertEqual(self.launch(**options), (EMPTY, b'original'))
                self.assertFalse(self.runtime_out.with_suffix('.tmp').exists())
        # A malformed disabled import must not be parsed or request any patch.
        (folder / 'runtime-requirements.json').write_text('{bad metadata')
        self.assertEqual(self.launch(enabled='0'), (EMPTY, b'original'))
        self.assertEqual(self.launch(online='1'), (EMPTY, b'original'))
        self.assertEqual(self.launch(config=config), (EMPTY, b'original'))

    def test_multiple_layers_deduplicate_and_unknown_requirement_fails_closed(self):
        a = self.add_mod('A')
        self.add_mod('B')
        self.assertEqual(self.launch(), (REQUIRED, b'imported'))
        (a / 'runtime-requirements.json').write_text(json.dumps({
            **REQUIRED, 'requirements': ['unapproved-heap']}))
        self.runtime_out.write_text(json.dumps(EMPTY))
        result = self.launch(check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsupported imported-map runtime requirement', result.stderr)
        self.assertEqual(json.loads(self.runtime_out.read_text()), EMPTY)


class PatchRuntimeCliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.settings = self.root / 'bbport.ini'
        self.settings.write_text('')
        self.xml = self.root / 'patch.xml'
        self.xml.write_text('<Patch><Metadata Name="Fixture" AppVer="01.09">'
                            '<PatchList><Line Type="bytes" Address="0x400040" Value="abcd"/>'
                            '</PatchList></Metadata></Patch>')
        (self.root / 'eboot.elf').write_bytes(synthetic_elf(address=0))
        self.metadata = self.root / 'runtime.json'

    def compile(self, runtime_path=None, extra='Fixture'):
        args = ['patches.py', '--out', str(self.root), '--settings', str(self.settings),
                '--xml', str(self.xml), '--fps', '30', '--extra', extra,
                '--game-dir', str(self.root / 'game')]
        if runtime_path is not None:
            args += ['--import-runtime', str(runtime_path)]
        with mock.patch.object(sys, 'argv', args), mock.patch.dict(os.environ, {
                'BB_INTEL_TONEMAP_FIX': '0', 'BB_ONLINE': '0'}, clear=False):
            patches.main()
        return (self.root / 'patches.bin').read_bytes()

    def test_no_argument_and_empty_import_keep_patch_compiler_byte_identical(self):
        before = (self.root / 'eboot.elf').read_bytes()
        baseline = self.compile()
        expected = struct.pack('<8sQQQQ', b'BBPATCH2', 0x400000, 1, 0x40, 2) + b'\xab\xcd'
        self.assertEqual(baseline, expected)
        self.metadata.write_text(json.dumps(EMPTY))
        self.assertEqual(self.compile(self.metadata), baseline)
        self.assertEqual((self.root / 'eboot.elf').read_bytes(), before)
        self.assertEqual(patch_records(baseline), [(0x40, b'\xab\xcd')])

    def test_unknown_requirement_rejects_without_replacing_existing_patch_output(self):
        old = self.compile()
        self.metadata.write_text(json.dumps({**REQUIRED, 'requirements': ['arbitrary-write']}))
        with self.assertRaisesRegex(ValueError, 'unsupported imported-map runtime requirement'):
            self.compile(self.metadata)
        self.assertEqual((self.root / 'patches.bin').read_bytes(), old)

    def test_public_patch_entrypoint_uses_hash_and_audit_guards(self):
        self.metadata.write_text(json.dumps(REQUIRED))
        with self.assertRaisesRegex(ValueError, 'verified Bloodborne 1.09'):
            self.compile(self.metadata)
        good = synthetic_elf()
        (self.root / 'eboot.elf').write_bytes(good)
        with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(good).hexdigest()):
            blob = self.compile(self.metadata, extra='')
            self.assertEqual(patch_records(blob), [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP))])
            self.xml.write_text('<Patch><Metadata Name="Other heap" AppVer="01.09">'
                                '<PatchList><Line Type="bytes64" Address="0x4b36dc0" Value="0x20c00000"/>'
                                '</PatchList></Metadata></Patch>')
            with self.assertRaisesRegex(ValueError, 'Imported map MAIN heap conflicts with Other heap'):
                self.compile(self.metadata, extra='Other heap')
            self.assertEqual((self.root / 'patches.bin').read_bytes(), blob)
        self.assertEqual((self.root / 'eboot.elf').read_bytes(), good)


def installed_elf():
    if os.environ.get('BB_IMPORT_RUNTIME_ELF'):
        return Path(os.environ['BB_IMPORT_RUNTIME_ELF'])
    for base in (ROOT, *ROOT.parents):
        candidates = sorted((base / 'out').glob('mac-settings-*/eboot.elf'))
        if candidates:
            return candidates[-1]
    return None


class InstalledExecutableTests(unittest.TestCase):
    def test_actual_109_elf_has_exact_main_heap_contract(self):
        path = installed_elf()
        if path is None:
            self.skipTest('optional decoded ELF absent; use BB_IMPORT_RUNTIME_ELF')
        blob = path.read_bytes()
        before = hashlib.sha256(blob).hexdigest()
        self.assertEqual(before, runtime.ELF_SHA256)
        with tempfile.TemporaryDirectory() as directory:
            metadata = Path(directory) / 'runtime.json'
            metadata.write_text(json.dumps(REQUIRED))
            self.assertEqual(runtime.heap_writes(metadata, blob),
                             [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP))])
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)


if __name__ == '__main__':
    unittest.main()
