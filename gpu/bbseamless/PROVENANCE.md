# Reference client sources

Updated to [lKaniel/bloodborne-server v1.3.1 at 5d2305e](https://github.com/lKaniel/bloodborne-server/tree/5d2305ee611ef1e03be39ff925f0da75c9a70393)
from the original 8d5e54f import. Local Mac adapters and durable invasion rewards are retained.
The v1.3.1 friends/open-team work also credits **Amanises**; lamp reset credits **Provi**, as
retained in the original sources. See [the integration record](../../docs/SEAMLESS_V1_3_1.md).
Original co-op client author: **Kaniel / lKaniel**
([GitHub](https://github.com/lKaniel)). The Mac overlay/Metal foundation and original
`tests/test_mtl_overlay.mm` harness are by **Mark Hall**, as credited in their retained
SPDX copyright headers in this repository
([Metal overlay source](https://github.com/evill33t/bloodborne_mac_port/blob/4451b9f851667e48aaa56e0e1597aa7f55642bb9/gpu/shim/bbport_overlay_metal.mm),
[test harness](https://github.com/evill33t/bloodborne_mac_port/blob/4451b9f851667e48aaa56e0e1597aa7f55642bb9/tests/test_mtl_overlay.mm)). New adaptation commits cite these sources and
authors explicitly in their commit messages, without claiming original authors co-authored
the Mac adaptations.
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
The reference's co-op artwork, status icons, editable PNG/SVG sources and packers are retained
under `../art/`, `../../scripts/coop_art.py` and `../../scripts/party_icons_atlas.py`.
`../shim/bbport_coop_art.*`, `bbport_party_icons.*`, `bbport_party_layout.h` and the matching
art decoder tests retain their original source hashes. Kaniel's packer describes the custom
co-op artwork as drawn for the reference with Nano Banana; it is separate from game-owned
art. The Mac `bbport_coop_style.*` and presentation changes adapt the reference's
[`MenuArt`, `CoopArt`, `Team`, `Party`, and `Compass` UI](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/gpu/shim/bbport_overlay.cpp).
Game menu textures, gauge strips and the menu font are read from the player's own files at
runtime and are never included in these artwork files or release packages.
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

The parity follow-up also retains Kaniel's `bbport_menu_texts.inc` and
`bbport_key_guide.h`. `bbport_coop_russian.inc` extracts the Russian `Ui::T` text from the
pinned `bbport_overlay.cpp`; `bbport_map_atlas.h` retains its `Map::WorldOf` and
`Map::AtlasOffset` placements. `bbport_language.h` adapts the language lookup from
`bbport_settings.cpp`, including game-language IDs, with macOS font fallbacks. Original and
adapted hashes are recorded in `source_files.json`. `bbport_coop_texts.inc` supplements the
reference table with labels specific to the Mac interface; it is a local adaptation.

The controller map navigation, crosshair, lamp snapping, place labels, compass layouts,
ping arrows, prerendered views and world browser adapt Kaniel's `Map`, `Compass`, `Pings`,
`Team` and `Ui::Keys` namespaces. Native Metal resource retention and the presentation fixture
continue Mark Hall's original overlay/test architecture. The launcher preparation helper
adapts Kaniel's visible Windows Map card using the same read-only map parsers and generated
cache as the game. The retained collision reader credits **MikeTat1988 / Hunter's Atlas**
([original project](https://github.com/MikeTat1988/HuntersAtlas)); no game-owned art, fonts,
geometry or textures are added to the distribution.

The invasion follow-up extends Kaniel / lKaniel's `bloodborne_invasion`, hostile-body and
`TradeDesk` design, with original implementation contributions by **Vitalii Shevchuk** and
the **shadNet / shadPS4 projects**. The separate server extension is committed as
`4201408d919f9787af43a075068947a44e95a231` and
`fceb97debb950b94df220c9963ccced4ad31fa2a` on top of the pinned server source; both commits
carry original-author and original-source trailers. Its shared protocol/trade adaptations
are included here with updated adapted hashes; the server itself remains in its own checkout.

Native visual research also credits **vladtepes22**,
[`NightwatchCoop/party_glow_rules.h` at db7c9fb2](https://github.com/vladtepes22/NightwatchCoop/blob/db7c9fb2f07a007a9bb53d3056a425c887c9d4b3/engine/gpu/shim/party/party_glow_rules.h),
for the effect/stateInfo/VFX relationship. The original
[`Paramdex Bloodborne definitions`](https://github.com/soulsmods/Paramdex/tree/master/BB/Defs)
and [effect names](https://github.com/soulsmods/Paramdex/blob/292ac5c10ace71116466cf8cd99c7dc034ef7602/BB/Names/SpEffectParam.txt)
identify 9026 as invasion guest play (names integrated by **Natalie Weizenbaum**, with
Smithbox/Paramdex contributors). These sources guide read-only inspection of the player's
1.09 parameters; no Nightwatch implementation or game parameter data is copied into the port.
The existing Mark Hall native-runtime/test foundations remain credited.

The local `invasion_visual.h` adapter admits only a blank runtime effect row or its own
previous result, derives the appearance from the player's own loaded parameter data and
retains the existing full-HP fallback policy. It distributes no game parameter bytes.

The covenant/atomic follow-up uses the original FromSoftware 1.09 PvP item lots 5500/5510,
empty covenant lots 5600–5810, oath effects and goods 704, inspected from the player's own
assets using the [Paramdex field definitions](https://github.com/soulsmods/Paramdex/tree/292ac5c10ace71116466cf8cd99c7dc034ef7602/BB/Defs)
and Soulstruct's event readers (Grimrukh and contributors). The optional wire fields and
reward validation extend Kaniel's protocol; separate server commits `ebbef913` and `9e60cec7`
retain the original source/author trailers.

The native save sites were independently checked in the owned executable, guided by
vladtepes22's [SaveRequest research](https://github.com/vladtepes22/NightwatchCoop/blob/db7c9fb2f07a007a9bb53d3056a425c887c9d4b3/engine/gpu/shim/party/game_state.cpp).
The new Mac directory/receipt transaction continues Mark Hall's runtime/save foundation
and Kaniel's [temporary-close save work](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/src/runtime_file.c).
No Nightwatch implementation or native save/parameter data is included in the distribution.
