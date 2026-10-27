#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
# SPDX-License-Identifier: GPL-2.0-or-later
# Original source: Kaniel/lKaniel, Vitalii Shevchuk and shadNet contributors;
# https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server
"""Verify the imported deployed server snapshot and pinned dependency."""
import hashlib
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
manifest = json.loads((root / "server/SOURCE_MANIFEST.json").read_text())
for entry in manifest["files"] + manifest["local_history"]:
    path = root / entry.get("path", entry.get("patch"))
    expected = entry.get("adapted_sha256", entry["sha256"])
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Source hash mismatch: {path.relative_to(root)}")
for entry in manifest["submodules"]:
    path = root / entry["path"]
    if not (path / ".git").exists():
        raise SystemExit(f"Initialize dependency: git submodule update --init {entry['path']}")
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], universal_newlines=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain"], universal_newlines=True).strip()
    if actual != entry["commit"] or dirty:
        raise SystemExit(f"Dependency pin/cleanliness mismatch: {entry['path']}")
print(f"Verified {len(manifest['files'])} source files, four original patches and pinned protobuf.")
