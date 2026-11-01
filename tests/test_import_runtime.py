# SPDX-License-Identifier: GPL-2.0-or-later
"""Launch-scoped heap requirements; no game execution or original asset writes.

The installed-executable check is optional. Set BB_IMPORT_RUNTIME_ELF to the
verified decoded 1.09 ELF, or keep it in an ancestor checkout's out/mac-settings-*.
"""
from paths import ROOT
import hashlib
import io
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
REQUIRED_1024 = {'schema': 'bbmap-runtime-1',
                 'requirements': ['bloodborne-1.09-connected-map-main-1024']}
BOTH_REQUIRED = {'schema': 'bbmap-runtime-1',
                 'requirements': sorted(REQUIRED['requirements'] + REQUIRED_1024['requirements'])}
OLD_HEAP = 524 * 1024 * 1024
NEW_HEAP = 768 * 1024 * 1024
NEW_HEAP_1024 = 1024 * 1024 * 1024
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
        self.assertEqual(runtime.MAIN_1024, REQUIRED_1024['requirements'][0])
        self.assertEqual(runtime.MAIN_OFFSET, HEAP_ADDRESS)
        self.assertEqual(runtime.requirements(self.metadata), {runtime.MAIN_768})
        self.assertEqual(runtime.merge([self.metadata, self.metadata,
                                        self.root / 'not-present.json']), REQUIRED)
        self.metadata.write_text(json.dumps(EMPTY))
        self.assertEqual(runtime.requirements(self.metadata), set())
        self.assertEqual(runtime.merge([self.metadata]), EMPTY)

    def test_fixed_1024_profile_and_mixed_layers_choose_one_maximum_write(self):
        large = self.root / 'large.json'
        large.write_text(json.dumps(REQUIRED_1024))
        self.assertEqual(runtime.requirements(large), {runtime.MAIN_1024})
        self.assertEqual(runtime.merge([large, large]), REQUIRED_1024)
        self.assertEqual(runtime.merge([large, self.metadata, large]), BOTH_REQUIRED)
        blob = synthetic_elf()
        with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(blob).hexdigest()):
            self.assertEqual(runtime.heap_writes(large, blob),
                             [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP_1024))])
            for profiles in ([runtime.MAIN_768, runtime.MAIN_1024],
                             [runtime.MAIN_1024, runtime.MAIN_768, runtime.MAIN_1024]):
                large.write_text(json.dumps({**EMPTY, 'requirements': profiles}))
                self.assertEqual(runtime.heap_writes(large, blob),
                                 [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP_1024))])

    def test_unknown_profiles_and_unbounded_metadata_reject(self):
        invalid = [None, [], {}, {'schema': EMPTY['schema'], 'requirements': ''},
                   {'schema': 'bbmap-runtime-2', 'requirements': []},
                   {**REQUIRED, 'address': HEAP_ADDRESS},
                   {**REQUIRED, 'requirements': [runtime.MAIN_768, 'custom']},
                   {**REQUIRED, 'requirements': [False]},
                   {**REQUIRED, 'requirements': ['MAIN-1024']},
                   {**REQUIRED, 'requirements': ['bloodborne-1.09-connected-map-main-1536']},
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
        for value in (REQUIRED, REQUIRED_1024, BOTH_REQUIRED):
            self.metadata.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError, 'verified Bloodborne 1.09'):
                runtime.heap_writes(self.metadata, blob)
        self.assertEqual(blob, before)

    def test_virtual_mapping_qword_and_file_backing_guards(self):
        good = synthetic_elf()
        with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(good).hexdigest()):
            self.assertEqual(runtime.heap_writes(self.metadata, good),
                             [(HEAP_ADDRESS, struct.pack('<Q', NEW_HEAP))])
        for requirement in (REQUIRED, REQUIRED_1024, BOTH_REQUIRED):
            self.metadata.write_text(json.dumps(requirement))
            for blob, reason in [(synthetic_elf(value=OLD_HEAP - 1), 'layout differs'),
                                 (synthetic_elf(value=NEW_HEAP), 'layout differs'),
                                 (synthetic_elf(value=NEW_HEAP_1024), 'layout differs'),
                                 (synthetic_elf(address=HEAP_ADDRESS + 8), 'outside'),
                                 (synthetic_elf(size=7), 'outside'),
                                 (synthetic_elf(kind=2), 'outside')]:
                with self.subTest(requirement=requirement, reason=reason, sha=hashlib.sha256(blob).hexdigest()):
                    with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(blob).hexdigest()):
                        with self.assertRaisesRegex(ValueError, reason):
                            runtime.heap_writes(self.metadata, blob)

    def test_heap_write_participates_in_existing_conflict_audit(self):
        for capacity in (NEW_HEAP, NEW_HEAP_1024):
            writes = [(HEAP_ADDRESS, struct.pack('<Q', capacity))]
            for first in (True, False):
                with self.subTest(capacity=capacity, import_first=first):
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


class DirectMemoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.metadata = self.root / 'runtime.json'

    def test_1024_and_mixed_profiles_request_a_bounded_minimum(self):
        for profile in (REQUIRED_1024, BOTH_REQUIRED):
            self.metadata.write_text(json.dumps(profile))
            for current in (None, '', 5056, '5056', 6000, '6143'):
                with self.subTest(profile=profile, current=current):
                    self.assertEqual(runtime.direct_memory_mib(self.metadata, current), 6144)
            for current in (6144, '6144', 8192, '9152', 16384):
                with self.subTest(profile=profile, current=current):
                    self.assertIsNone(runtime.direct_memory_mib(self.metadata, current))

    def test_absent_empty_and_768_profiles_preserve_unset_and_existing_settings(self):
        for current in (None, '', 5056, '6144', 9152, '16384'):
            self.assertIsNone(runtime.direct_memory_mib(None, current))
            for profile in (EMPTY, REQUIRED):
                self.metadata.write_text(json.dumps(profile))
                self.assertIsNone(runtime.direct_memory_mib(self.metadata, current))

    def test_current_direct_memory_uses_the_existing_runtime_bounds(self):
        self.metadata.write_text(json.dumps(REQUIRED_1024))
        for current in ('invalid', '-1', '5055', '16385', '6144.0', 6144.0,
                        '0x1800', ' 6144', '+6144', '\u0666\u0661\u0664\u0664', True, [], {}):
            with self.subTest(current=current):
                with self.assertRaisesRegex(ValueError, '5056 to 16384'):
                    runtime.direct_memory_mib(self.metadata, current)

    def command(self, current=None):
        args = [sys.executable, str(ROOT / 'scripts/import_runtime.py'),
                '--direct-memory', str(self.metadata)]
        if current is not None:
            args += ['--current-dmem', current]
        return subprocess.run(args, capture_output=True, text=True, timeout=15)

    def test_cli_prints_only_an_increase_and_preserves_larger_resolution_budgets(self):
        for profile in (EMPTY, REQUIRED, REQUIRED_1024, BOTH_REQUIRED):
            self.metadata.write_text(json.dumps(profile))
            for current in (None, '', '5056', '6144', '9152'):
                with self.subTest(profile=profile, current=current):
                    result = self.command(current)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = '6144\n' if profile in (REQUIRED_1024, BOTH_REQUIRED) and current in (None, '', '5056') else ''
                    self.assertEqual(result.stdout, expected)
                    self.assertEqual(result.stderr, '')

    def test_cli_rejects_invalid_metadata_and_current_value_without_output(self):
        for metadata in ('{bad json', json.dumps({**EMPTY, 'requirements': ['unknown-profile']})):
            self.metadata.write_text(metadata)
            result = self.command('9152')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, '')
        self.metadata.write_text(json.dumps(REQUIRED_1024))
        result = self.command('16385')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('5056 to 16384', result.stderr)


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

    def test_1024_metadata_merges_with_768_only_from_enabled_mods(self):
        self.add_mod('Small')
        large = self.add_mod('Large')
        (large / 'runtime-requirements.json').write_text(json.dumps(REQUIRED_1024))
        self.assertEqual(self.launch(), (BOTH_REQUIRED, b'imported'))
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'disabled': ['Small']}))
        self.assertEqual(self.launch(config=config), (REQUIRED_1024, b'imported'))
        config.write_text(json.dumps({'disabled': ['Large']}))
        self.assertEqual(self.launch(config=config), (REQUIRED, b'imported'))
        self.assertEqual(self.launch(enabled='0'), (EMPTY, b'original'))
        self.assertEqual(self.launch(online='1'), (EMPTY, b'original'))


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

    def test_1024_and_mixed_profiles_emit_one_qword_with_dynamic_label(self):
        good = synthetic_elf()
        (self.root / 'eboot.elf').write_bytes(good)
        for requirements in (REQUIRED, REQUIRED_1024, BOTH_REQUIRED):
            capacity = NEW_HEAP if requirements == REQUIRED else NEW_HEAP_1024
            self.metadata.write_text(json.dumps(requirements))
            printed = io.StringIO()
            with self.subTest(requirements=requirements):
                with mock.patch.object(runtime, 'ELF_SHA256', hashlib.sha256(good).hexdigest()), mock.patch('sys.stdout', printed):
                    blob = self.compile(self.metadata, extra='')
                self.assertEqual(patch_records(blob), [(HEAP_ADDRESS, struct.pack('<Q', capacity))])
                self.assertIn(f'Imported map MAIN heap: {capacity // (1024 * 1024)} MiB', printed.getvalue())
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
            for requirements in (REQUIRED, REQUIRED_1024, BOTH_REQUIRED):
                metadata.write_text(json.dumps(requirements))
                capacity = NEW_HEAP if requirements == REQUIRED else NEW_HEAP_1024
                self.assertEqual(runtime.heap_writes(metadata, blob),
                                 [(HEAP_ADDRESS, struct.pack('<Q', capacity))])
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)


if __name__ == '__main__':
    unittest.main()
