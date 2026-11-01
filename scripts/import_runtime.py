# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixed, executable-checked runtime requirements of enabled imported maps."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

SCHEMA = 'bbmap-runtime-1'
MAIN_768 = 'bloodborne-1.09-connected-map-main-768'
MAIN_1024 = 'bloodborne-1.09-connected-map-main-1024'
_MAIN_MIB = {MAIN_768: 768, MAIN_1024: 1024}
ELF_SHA256 = 'cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86'
MAIN_OFFSET = 0x4736dc0
_DIRECT_MEMORY_MIN = 5056
_DIRECT_MEMORY_MAX = 16384
_MAIN_1024_DIRECT_MEMORY = 6144


def requirements(path):
    path = Path(path)
    if path.is_symlink() or path.stat().st_size > 4096:
        raise ValueError(f'{path}: unsupported imported-map runtime metadata')
    value = json.loads(path.read_text())
    if (not isinstance(value, dict) or set(value) != {'schema', 'requirements'} or
            value['schema'] != SCHEMA or not isinstance(value['requirements'], list) or
            any(p not in (MAIN_768, MAIN_1024) for p in value['requirements'])):
        raise ValueError(f'{path}: unsupported imported-map runtime requirement')
    return set(value['requirements'])


def merge(paths):
    profiles = set()
    for path in paths:
        if Path(path).is_file():
            profiles.update(requirements(path))
    return {'schema': SCHEMA, 'requirements': sorted(profiles)}


def direct_memory_mib(path, current_dmem=None):
    """Recommend a launch-only increase, or None to preserve the environment.

    A missing/empty setting uses the retail 5056 MiB default. MAIN_1024 needs
    space for both its larger pool and the independently allocated HAVOK heap.
    Larger resolution/user budgets remain intact; other imports request none.
    """
    profiles = requirements(path) if path is not None else set()
    current = _DIRECT_MEMORY_MIN
    if current_dmem is not None and current_dmem != '':
        text = str(current_dmem)
        if (isinstance(current_dmem, bool) or not text.isascii() or
                not text.isdecimal() or
                not _DIRECT_MEMORY_MIN <= int(text) <= _DIRECT_MEMORY_MAX):
            raise ValueError('BB_DMEM_MB must be an integer from 5056 to 16384 MiB')
        current = int(text)
    if MAIN_1024 in profiles and current < _MAIN_1024_DIRECT_MEMORY:
        return _MAIN_1024_DIRECT_MEMORY
    return None


def heap_writes(path, elf):
    if path is None:
        return []
    profiles = requirements(path)
    if not profiles:
        return []
    # Multiple enabled imports share one MAIN heap. Select the largest fixed
    # requirement once; never stack conflicting 768/1024 writes at this slot.
    capacity = max(_MAIN_MIB[profile] for profile in profiles)
    if hashlib.sha256(elf).hexdigest() != ELF_SHA256:
        raise ValueError('Imported map MAIN heap requires the verified Bloodborne 1.09 executable profile')
    # The patch uses image virtual offsets; ELF file offsets differ here.
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phsize, count = struct.unpack_from('<HH', elf, 0x36)
    for index in range(count):
        kind, _, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize)
        if kind == 1 and address <= MAIN_OFFSET and MAIN_OFFSET + 8 <= address + size:
            old, = struct.unpack_from('<Q', elf, offset + MAIN_OFFSET - address)
            if old != 524 * 1024 * 1024:
                raise ValueError('Imported map MAIN heap layout differs from the verified executable')
            return [(MAIN_OFFSET, struct.pack('<Q', capacity * 1024 * 1024))]
    raise ValueError('Imported map MAIN heap table is outside the executable file segments')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--direct-memory', required=True, type=Path,
                        metavar='RUNTIME_JSON', help='enabled imported-map requirements')
    parser.add_argument('--current-dmem', help='current BB_DMEM_MB; empty means unset')
    args = parser.parse_args(argv)
    try:
        recommendation = direct_memory_mib(args.direct_memory, args.current_dmem)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    if recommendation is not None:
        print(recommendation)


if __name__ == '__main__':
    main()
