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

`source_files.json` records original paths and SHA-256 hashes of the imported files at this
checkpoint. New local adapters are `CMakeLists.txt`, `client.h`, `mac_client.cpp` and
`bbseamless.exports`. They are unfinished and not wired into the normal build; see
`docs/SEAMLESS_COOP_CLIENT.md` before using or resuming this prototype.

Shared `vendor/coop/monocypher` retains its own notices. Zydis and Xbyak were used from temporary
external checkouts during the initial compile attempt and are not vendored or installed by
this checkpoint. Dependency/bootstrap integration remains to be implemented. No game code,
game assets, saves, backend executable or server configuration is included.
