# Reference client sources

Imported from [lKaniel/bloodborne-server at 8d5e54f](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1).
The project and retained source headers grant GPL-2.0-or-later; retain individual copyright
and third-party notices. The repository's `LICENSE` applies. The existing THD online foundation
has separate GPL-3.0-or-later provenance in `docs/ONLINE_PROVENANCE.md`.

| Local path | Original path |
| --- | --- |
| `vendor/core/bloodborne*`, `vendor/core/guest_hook*`, `vendor/core/cpu_patches.h` | `emulator/src/core/` |
| `vendor/core/libraries/network`, `vendor/core/libraries/np` | Corresponding `emulator/src/core/libraries/` directories |
| `vendor/core/file_format/psf.*` | `emulator/src/core/file_format/psf.*` |
| `vendor/common` | `emulator/src/common` (headers and selected implementation files) |
| `vendor/shadnet` | `emulator/src/shadnet` |
| `vendor/shadnet/shadnet.proto` | `server/src/shadnet.proto` |
| `vendor/coop` | Selected shared wire, crypto and value code from `server/coop`; no server loop or hosting service |
| `include/core`, `include/common`, `include/imgui` | Corresponding `bbport/gpu/shim/` directories |
| `bbcoop`, `bbcoop.h` | `bbport/gpu/bbcoop`, `bbport/gpu/bbcoop.h` |
| `network_glue.cpp` | `bbport/gpu/bbnet/bbnet_glue.cpp` |

`source_files.json` records the pinned original paths and SHA-256 hashes. Locally adapted
copies also carry `adapted_sha256`; paths are relative to this directory. `touch.h` extracts
the reference runtime's MapHold gesture state, with the complete original file hash retained.
The event helper under `scripts/seamless/` comes from `tools/re/party_boss_events.py`.
The `text/` helpers come from `bbport/gpu/shim/bbport_game_art.*`,
`bbport_save_editor.h` and `bbport_save_editor_items.cpp`; only read-only game text parsing is
used for the UI. Kernel error/time headers retain their emulator source paths in the manifest.

Local adapters include `CMakeLists.txt`, `client.h`, `mac_client.cpp`, `mac_epoll.cpp`,
`mac_memory.h`, `native_ui.cpp`, `ui.h`, `touch.h`, `include/common/float_chars.h` and the export
list. Adaptations supply kqueue readiness, Mach memory/protection queries, libc++ compatibility
on macOS 15, pthread attachment and the port's runtime callbacks. The native Metal overlay,
versioned cross-process snapshots/actions and Metal portrait readback live in `gpu/shim/` and
`gpu/shadps4/video_core/texture_cache/`. No renderer from the reference project is imported.

The normal bootstrap pins Zydis 4.1.1 at
`a2278f1d254e492f6a6b39f6cb5d1f5d515659dc` (including Zycore), and Xbyak at
`cd6bad3cad2e07a3e277c23d2b1f065320024717`. Their licenses, Monocypher's notices,
this provenance file and the source manifest are included in release packaging.
Shared `vendor/coop/monocypher` retains its own notices. No game executable, game assets,
saves, backend executable or server configuration is included.

See [client setup and validation](../../docs/SEAMLESS_COOP_CLIENT.md).
