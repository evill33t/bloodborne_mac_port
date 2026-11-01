# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixed, executable-checked runtime requirements of enabled imported maps."""
import hashlib
import json
import struct
from pathlib import Path

SCHEMA = 'bbmap-runtime-1'
MAIN_768 = 'bloodborne-1.09-connected-map-main-768'
ELF_SHA256 = 'cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86'
MAIN_OFFSET = 0x4736dc0


def requirements(path):
    path = Path(path)
    if path.is_symlink() or path.stat().st_size > 4096:
        raise ValueError(f'{path}: unsupported imported-map runtime metadata')
    value = json.loads(path.read_text())
    if (not isinstance(value, dict) or set(value) != {'schema', 'requirements'} or
            value['schema'] != SCHEMA or not isinstance(value['requirements'], list) or
            any(p != MAIN_768 for p in value['requirements'])):
        raise ValueError(f'{path}: unsupported imported-map runtime requirement')
    return set(value['requirements'])


def merge(paths):
    profiles = set()
    for path in paths:
        if Path(path).is_file():
            profiles.update(requirements(path))
    return {'schema': SCHEMA, 'requirements': sorted(profiles)}


def heap_writes(path, elf):
    if path is None or not requirements(path):
        return []
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
            return [(MAIN_OFFSET, struct.pack('<Q', 768 * 1024 * 1024))]
    raise ValueError('Imported map MAIN heap table is outside the executable file segments')
