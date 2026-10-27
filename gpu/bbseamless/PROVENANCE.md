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
`bbport_save_editor.h` and `bbport_save_editor_items.cpp`; read-only text/texture parsing is
used for the UI. Kernel error/time headers retain their emulator source paths in the manifest.

The terrain readers/rasterizer in `../shim/bbport_map_art.*`, `bbport_map_collision.cpp` and
`../../tests/test_map_art.cpp` retain the pinned reference's source hashes. Collision reading
also retains its Hunter's Atlas GPL notice. The native GPU target compiles the existing
`text/bbport_game_art.cpp` through a shim without loading the network module. The worker/cache,
map camera/fog and Metal resource ownership are local adapters. The live Metal shader in
`../shim/bbport_overlay_metal.mm` ports the reference's
`bbport/gpu/shim/shaders/map_3d.vert` and `map_3d.frag`; it uses no Vulkan renderer or loader.

Local adapters include `CMakeLists.txt`, `client.h`, `mac_client.cpp`, `mac_epoll.cpp`,
`mac_memory.h`, `native_ui.cpp`, `ui.h`, `touch.h`, `include/common/float_chars.h` and the export
list. Adaptations supply kqueue readiness, Mach memory/protection queries, libc++ compatibility
on macOS 15, pthread attachment and the port's runtime callbacks. The native Metal overlay,
versioned cross-process snapshots/actions and Metal portrait readback live in `gpu/shim/` and
`gpu/shadps4/video_core/texture_cache/`. No Vulkan renderer from the reference project is imported.

The normal bootstrap pins Zydis 4.1.1 at
`a2278f1d254e492f6a6b39f6cb5d1f5d515659dc` (including Zycore), and Xbyak at
`cd6bad3cad2e07a3e277c23d2b1f065320024717`. Their licenses, Monocypher's notices,
this provenance file and the source manifest are included in release packaging.
Shared `vendor/coop/monocypher` retains its own notices. No game executable, game assets,
saves, backend executable or server configuration is included.

See [client setup and validation](../../docs/SEAMLESS_COOP_CLIENT.md).

The Mac client enables the reference's existing `invasion_fallback_switch`: the pinned
server's placeholder kind/sign joined as a friendly cooperator in the 2026-10-27 game test.
The fallback sets the invader's local character type and keeps its remote body on the hostile
team. The live retest verified native PvP damage and return-home/world restoration; it does
not establish the still-unknown retail invasion kind/sign or mixed-client compatibility.
