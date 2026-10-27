# Bloodborne for Apple Silicon — development branch

This is Ronny Stiftel's (`evill33t`) integration branch of the macOS Bloodborne port. **`dev`** combines the current upstream runtime with the fork's launcher, Metal renderer, achievements, online play and seamless co-op work. It replaces the former `codex/all-my-prs` branch while preserving the original PR histories.

The original x86-64 game and compatibility runtime run through Rosetta 2. A separate **native arm64 process renders directly through Metal**. You supply your own Bloodborne 1.09 files; game executables, DLC, textures, geometry, fonts and saves are not distributed here.

This is an experimental development build. Included features and draft integrations are described below; inclusion does not mean every feature has completed campaign, hardware or multiplayer compatibility testing. The isolated path-tracing prototype is excluded.

## Features

### Launcher, settings and controls

- Bundled fallback app icon from the Windows launcher, replaced at runtime by the selected game’s icon when available. [Branding sources and credits](assets/branding/README.md).
- Native SwiftUI launcher with **General, Advanced, Controls, Achievements and Online** pages; game-folder validation, save-folder selection and remembered settings.
- A persistent **Regular / Seamless Co-op** selector above the pages. New configurations start in Regular with online play off. Switching modes preserves each mode's saves, accounts and server settings; the choice also applies when the launcher is hidden.
- In-game settings with **⌘,** or **L3 + R3**, native **Mac Settings** and **Exit Game** entries, and a launcher **Stop** button with process cleanup and crash-report recovery.
- Keyboard, mouse and SDL3 gamepad support, including controller selection, touchpad gestures, keyboard text entry and mouse camera capture. Rebind keys, mouse buttons and wheel steps in **Controls**; clear individual actions, inspect shared bindings or restore defaults. Left Alt walks at half stick deflection. Mouse sensitivity, vertical multiplier, inversion and camera-key behavior are configurable.
- Mods and cheats manager with import, enable/disable, ordering, conflict reporting, backups and private launch views. Community patches include frame-rate, camera, intro and visual options. Original game files remain the source for generated views. See [Mods](docs/MODS.md).
- Local achievements browser, base-game platinum progression, DLC trophies, per-save-profile history, optional in-game banners and trophy artwork read from your own dump. Shared co-op unlocks use the same local history and achievement UI. See [Achievements](docs/ACHIEVEMENTS.md).
- Built-in benchmark workflow with isolated benchmark settings/saves, when the required local benchmark data is available; dated logs, crash reports, About information and optional update checks with a test-build preference. Checkout builds are updated with Git; upstream release downloads do not contain every `dev` integration.

### Graphics and performance

| Feature | Behavior |
| --- | --- |
| Direct Metal | Native arm64 GPU process, Metal command encoding, shader translation to MSL and Metal resource management. Vulkan-named compatibility files/types remain in the source; the running renderer uses neither a Vulkan loader nor MoltenVK. |
| Upscaling and anti-aliasing | **MetalFX Temporal** is the recommended default. MetalFX Spatial, AMD FSR 3.1, TAA and Off are also available, with presets, sharpening and motion/reactivity controls. **Spatial: lighter on the GPU, shimmers more in motion.** |
| Resolution and aspect ratio | 720p, 1080p, 1440p and 4K output choices, native 16:10/ultrawide modes, separate scene/UI sizing, plus an optional live-resolution path. Some changes require restarting the game. |
| Frame rates | 30/40/60/90/120 FPS cap choices, fixed or variable timing, optional frame pacing and a guarded experimental adaptive timing adapter. Seamless co-op uses the reference client's fixed 60 FPS timing. See [Adaptive FPS](docs/ADAPTIVE_FPS.md). |
| Texture and memory handling | Lossless texture compression, bounded GPU arenas, guest-memory mappings, resource retirement and synchronization fixes, including a separate host reservation path for macOS 27. Performance and memory needs depend on the area and settings. |
| Shader and draw work | Pipeline/shader caches, background compilation with Wait/Skip/Balanced policies, resource-binding caching and fullscreen load avoidance. Skip policies can temporarily omit draws while compilation finishes. |
| Visual controls | Original post-effect switches, model LOD, FPS/debug views, Metal HUD and optional additional screen-space occlusion and diffuse bounce lighting with Off/Low/High modes. Screen-space lighting cannot include hidden geometry or offscreen light sources. See [Lighting](docs/SCREEN_SPACE_LIGHTING.md). |

Experimental integrations are opt-in:

- **MetalFX 2× frame generation:** interpolates presentation frames on supported hardware with macOS 26 or newer. It needs temporal inputs and a compatible display/cap cadence; external Adaptive-Sync requires fullscreen. It does not increase the game's simulation or input rate. HDR suspends generation while retaining the saved preference. See [Frame generation](docs/DESIGN_FRAME_GENERATION.md).
- **Scene-based HDR/EDR:** native scene-radiance processing, one-pass UI composition, display-headroom detection, paper-white and peak controls. Admission is currently limited to the supported native 1080p Native AA path; unsupported resolution, wide-output or shader combinations fall back to SDR. Display calibration and broader gameplay coverage remain work in progress. See [HDR](docs/HDR.md).
- **Native velocity experiment:** an optional game-motion producer alongside the existing synthetic motion path. It remains off by default; an improvement over the default has not been established by matched gameplay captures. See [Native velocity](docs/native_velocity_experiment.md).

### Online play and seamless co-op

**Regular** retains normal gameplay and saves. Its **Online** page also exposes the separate The Hunter's Dream and shadNet pools, with account configuration, connection checks, optional UPnP and credential storage. Their communities and servers have their own compatibility requirements. HTTPS certificate verification is retained for the existing HTTPS backend.

**Seamless Co-op** defaults to **`bloodborne.phobos.services`** and uses the client adapted from Kaniel's [bloodborne-server](https://github.com/lKaniel/bloodborne-server). It includes:

- Automatic server-scoped account registration, encrypted authenticated UDP transport, pinned identity for the default host, reconnect handling and isolated per-server save profiles seeded from a copy of the chosen regular saves.
- Persistent teams and worlds; hunter/name/code invitations, online-hunter and world browsers, ready checks, world selection, session restoration, shared world/event/boss progress, enemy ownership and HP synchronization, and independent travel.
- Downed/revive/wipe behavior, spectating, death modes and rot, optional enemy scaling/harder enemies, chalice sharing, bloodstain retention, shared trophies and cutscene skip voting.
- Windows-reference-style co-op presentation, with the requested Windows launcher hero background on team setup and “Entering the hunt”: hunter cards, live portraits, readiness/leader marks, party HP/vial/bullet displays, compass, pings, invitations, loading screens and game-styled dialogs. Game art and fonts are read from the player's own files, with fallbacks.
- Trading carried goods, equipment and Blood Echoes, with offers and acceptance; duel requests and countdowns; host invasion opt-in, invasion search, cancellation and return home.
- Native red invader appearance and hostile PvP behavior through the guarded compatibility fallback. Victory payouts include Insight, level-based Echoes and a Blood Dreg for an eligible Corruption oath winner. Other inspected covenant PvP lots add no item; this is documented server policy rather than a claim of a complete retail matchmaking/economy recreation.
- Persistent reward mailboxes and duplicate protection. On the supported Mac save path, an invasion payout is acknowledged only after the character save and receipt commit together; a crash replays an uncommitted payout or acknowledges the matching receipt. Move the **complete** per-server save directory, including hidden receipts, when backing it up.
- Localized co-op menus, keyboard/controller guides, configurable compass placement, bloodstain/ping visibility and an optional free-camera hotkey.

The client has real Mac-to-Mac gameplay and invasion evidence in the [dated validation record](docs/SEAMLESS_COOP_VALIDATION.md). Mixed Windows/Mac invasions, the unknown retail invasion kind/sign and exhaustive campaign/DLC/chalice coverage remain unverified. See [Client setup](docs/SEAMLESS_COOP_CLIENT.md) and [Invasion rewards](docs/SEAMLESS_INVASION_REWARDS.md) for protocol, reward and recovery details.

### Multiplayer maps

Maps are generated locally from **your own level geometry, collision and textures**. Available views are **Rendered terrain, Prerendered 3D, Live 3D and Coordinates**. The launcher offers **Prepare maps…**, progress and cancellation; generated maps are cached for later sessions.

The multiplayer map includes teammates, portraits, lamps, place labels, cleared areas, bloodstains, fog of war and saved exploration. It supports controller navigation, zoom/pan, a crosshair, lamp snapping, location pings and travel to eligible lit lamps. The live 3D view uses the native Metal renderer. No pre-extracted game geometry, textures or map assets are supplied.

### Server source and hosting

The separately hosted Linux server is included under [`server/`](server/LOCAL_FORK.md), with the imported source manifest, retained upstream patches, invasion/covenant reward extensions, durable payouts and **ARM64/amd64 Docker support**. The default Docker path uses Qt from conda-forge and builds QtHttpServer. An optional aqt path is also retained.

The Mac launcher connects as a client; it does not start this server. See the [standalone Compose example](server/hosting/docker-compose.phobos.yml) and [server import notes](server/LOCAL_FORK.md) before hosting or migrating an existing installation.

| Public service | Protocol and destination port |
| --- | --- |
| Co-op link | UDP **31314** |
| Web API | TCP **31315** |
| Discovery, messages, ghosts and bloodstains | TCP **20443** |

TCP **31313** remains local to the server in hosted mode. Stats/admin services stay private. Normal clients use outbound connections and do not need those ports forwarded at home. The two HTTP services use the upstream HTTP protocol; the authenticated co-op UDP link is encrypted.

## Requirements

- Apple Silicon Mac, **macOS 15 or newer**, and at least **16 GB unified memory**. Higher resolutions and experimental effects need more resources.
- **Rosetta 2** for the x86-64 game process. The GPU host and launcher are native arm64.
- Your own unpacked/decrypted Bloodborne **1.09** files, including the original, unmodified `eboot.bin`, `sce_sys`, `sce_module` and `dvdroot_ps4`. The verified European GOTY layout is `CUSA03173`; other layouts must pass the loader and feature-specific admission checks. DLC needs your own matching DLC content.
- To build: Xcode Command Line Tools, arm64 Homebrew, Python 3 and the pinned dependencies below. Intel Macs, Windows and Linux are not targets of this Mac client.

A prepatched executable is rejected because this runtime applies checked patches itself. Point the launcher at an unpacked game folder, not a PKG. The optional [`pkg_extract`](tools/pkg_extract/) tool handles supported decrypted packages; it requires Crypto++ when built separately.

## Build and launch

```sh
git clone --recursive --branch dev https://github.com/evill33t/bloodborne_mac_port.git
cd bloodborne_mac_port

# Native build tools; Rosetta is needed for the game-side build/run.
brew install cmake ninja nasm glslang pkgconf boost
softwareupdate --install-rosetta

# Pinned dependencies for the two process architectures.
bash scripts/macos/bootstrap_deps.sh
BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh

# Runtime, online modules and native GPU host.
bash build_macos.sh

# A local Finder app that runs this checkout.
bash scripts/macos/make_app.sh
```

Open **Bloodborne.app**, choose the game folder under **Locations**, choose **Regular** or **Seamless Co-op**, then press **Play**. **Online** contains the co-op server and host rules. The selected mode is remembered. Hold **⌥** while opening the app to show a hidden launcher.

For Terminal launch, use `Bloodborne.command`, or:

```sh
BB_GAME_DIR=/absolute/path/to/CUSA03173 bash run.sh
```

For a self-contained app, use `bash scripts/macos/package_app.sh`; its bundled Python, libraries and notices are assembled separately from the local checkout app. No game data is packaged. Upstream's [release downloads](https://github.com/Wooloomooloo2/bloodborne_mac_port/releases) are upstream builds; use this branch's source build for the combined features listed here.

Update an existing checkout with `git pull --ff-only` and `git submodule update --init --recursive`, then rebuild. Build prerequisites and submodule revisions are recorded in [`bootstrap_deps.sh`](scripts/macos/bootstrap_deps.sh), [`.gitmodules`](.gitmodules) and the corresponding source manifests.

## Controls at a glance

| Control | Action |
| --- | --- |
| ⌘, / L3 + R3 | In-game settings |
| ⌘ while mouse camera is captured | Release the cursor |
| F6 | Co-op team/menu |
| F8 / hold touchpad | Multiplayer map |
| G / double-tap touchpad | Team ping; keyboard key is configurable |
| Single touchpad tap | Original gesture menu after the double-tap interval |
| Right-click on the map | Location ping |
| F7 / L3 + Square | Free camera, when explicitly enabled |
| Attack a downed teammate | Revive |
| Hold Circle while downed | Give up |
| Options during a cutscene | Vote to skip |

Regular game actions and mouse bindings are listed and editable in the launcher's **Controls** page. Co-op-specific shortcuts are described in the [client guide](docs/SEAMLESS_COOP_CLIENT.md).

## Saves, configuration and diagnostics

Default data lives in `~/Library/Application Support/Bloodborne`:

- `bbport.ini`: renderer, controls and online settings. Packaged apps also keep `launch.conf` here; a checkout keeps it beside `Bloodborne.command`.
- `user/savedata`: regular saves, unless another folder is selected.
- `user-seamless/<server-hash>/`: co-op saves and payout receipts for each server.
- `seamless/identities/`: private server-scoped identities. Preserve these with your backups to keep the same account.
- Generated launch views, prepared images and map/shader caches are separate from the source game folder. `BB_DATA_DIR` and the documented save overrides support isolated setups.

Use **Show Log** or **Crash Report…** after a failure. App session logs live under `~/Library/Logs/Bloodborne`; launch/configuration behavior is described in [the client guide](docs/SEAMLESS_COOP_CLIENT.md). Avoid publishing identity files, passwords or tokens with reports.

Some features require their documented supported executable/parameter layout and will reject conflicting hook bytes or keep a fallback. The gameplay tests recorded in this repository are bounded checks; full progression, every mod combination, long sessions and all hardware remain ongoing work. See [Known issues](docs/MACOS_BACKLOG.md), [Findings](docs/FINDINGS.md) and [Roadmap](docs/ROADMAP.md).

## Development and validation

```sh
bash build_macos.sh --test
bash scripts/macos/build_launcher.sh --test
```

The suites cover runtime/input/save behavior, launcher models, achievement delivery, online codecs and module admission, map parsing/rendering, native menus, shared-memory bridges and focused Metal rendering fixtures. GPU checks need a Metal-capable Mac and may open a test window. Live gameplay, network/server probes and fixture results are tracked separately in the relevant validation documents.

Design references: [native GPU process](docs/DESIGN_NATIVE_GPU_PROCESS.md), [Metal backend](docs/DESIGN_METAL_BACKEND.md), [native Mac menus](docs/NATIVE_MAC_SETTINGS.md), [patch safety](docs/PATCH_SAFETY.md), [input](docs/DESIGN_INPUT.md) and [regular online](docs/ONLINE.md).

## Credits

Original authors retain their authorship. This branch combines and adapts their work; repository links below identify code origins, dependencies and separately identified research. Exact pins, original paths, hashes and adaptation notes remain in file headers, Git history and the linked provenance records.

### Port, runtime, renderer and online sources

| Original authors / project | Source and contribution |
| --- | --- |
| **Mark Hall / Wooloomooloo2** and Mac-port contributors | [`Wooloomooloo2/bloodborne_mac_port`](https://github.com/Wooloomooloo2/bloodborne_mac_port): macOS port, native Metal renderer/GPU process, launcher, settings, runtime and test foundations. |
| **deadinside28** | [`deadinside28/bloodborne_pc`](https://github.com/deadinside28/bloodborne_pc): original Linux bbport, flat-image execution, HLE runtime, two-stage graphics pipeline, synthetic motion and upscaler foundations. |
| **Mrsuss60** and Windows-port contributors | [`Mrsuss60/bloodborne_pc_windows_port`](https://github.com/Mrsuss60/bloodborne_pc_windows_port): this repository's Windows lineage, including runtime, audio, stability, memory and pacing work. |
| **shadPS4 Emulator Project** contributors | [`shadps4-emu/shadPS4`](https://github.com/shadps4-emu/shadPS4): video core, shader recompiler and PS4 library/network implementations. Original notices remain under `gpu/shadps4`, `gpu/bbnet` and imported co-op sources. |
| **Kaniel / lKaniel, Vitalii Shevchuk**, shadNet/shadPS4 and bloodborne-server contributors | [`lKaniel/bloodborne-server`](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1): seamless client/protocol, hooks, worlds, PvP, trades, maps, menus, custom co-op art, hosted backend and Docker foundation. See [client provenance](gpu/bbseamless/PROVENANCE.md), [artwork notices](gpu/art/README.md) and [server manifest/patches](server/LOCAL_FORK.md). |
| **shadNet contributors; Wozzardman** and P2P-fork contributors | [`shadps4-emu/shadNet`](https://github.com/shadps4-emu/shadNet), [`Wozzardman/shadnet-p2p`](https://github.com/Wozzardman/shadnet-p2p) and [`Wozzardman/shadp2p`](https://github.com/Wozzardman/shadp2p): retained server/network lineage and Bloodborne matching/broker foundations. |
| **droogie** and bbhost contributors; **Supermedo** and Windows-fork contributors | [`droogie/bbhost`](https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771) through [`Supermedo/bloodborne_pc`](https://github.com/Supermedo/bloodborne_pc): The Hunter's Dream backend; bbhost also supplies selected menu/timing foundations. See [online provenance](docs/ONLINE_PROVENANCE.md) and [native-menu provenance](docs/NATIVE_MAC_SETTINGS.md). |
| **nick24051998** and Supermedo Windows-port contributors | [`Supermedo/bloodborne_pc` launcher assets](https://github.com/Supermedo/bloodborne_pc/tree/5adea35dbd38851339b63773c1245c02a314f094/launcher): fallback app icon and hero image for co-op team/loading screens. [Exact source commits, hashes and usage](assets/branding/README.md). |
| **pyL1nx** and achievement-patch contributors | [`pyL1nx/bloodborne_pc`](https://github.com/pyL1nx/bloodborne_pc/tree/64919f7da13a24bac53578acec5f74f959fe5c4e): original achievement integration, catalog, history format and platinum rule. See [achievement provenance](docs/ACHIEVEMENTS.md#original-sources-and-adaptation). |
| **Ronny Stiftel / evill33t** and credited source contributors | [`evill33t/bloodborne_mac`](https://github.com/evill33t/bloodborne_mac): selected Mac renderer/timing/menu work at [`bbacb95c`](https://github.com/evill33t/bloodborne_mac/tree/bbacb95c3338efcd627c0edaec05ff7534e555da), and achievement implementation/design at [`5469befb`](https://github.com/evill33t/bloodborne_mac/tree/5469befbb0c39759ffbf91d20a2e1a514e9b7581). Local adaptation and integration live in [`evill33t/bloodborne_mac_port`](https://github.com/evill33t/bloodborne_mac_port). |
| **MikeTat1988** and Hunter's Atlas contributors | [`MikeTat1988/HuntersAtlas`](https://github.com/MikeTat1988/HuntersAtlas): collision-reader code retained through the map import, with its original GPL notice. |
| **Kyo, Lance McDonald, illusion, emoose, auser1337, yogesh1239** and community patch authors | Original authors recorded in [`patches/Bloodborne.xml`](patches/Bloodborne.xml); timing adaptations also cite the [exact source revisions](src/fps/PROVENANCE.md). |

### Libraries, retained source and build dependencies

| Authors / project | Repository or source; use |
| --- | --- |
| **AMD / GPUOpen** contributors; **FireBurn** | [`GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [`FireBurn/FSR-Vulkan`](https://github.com/FireBurn/FSR-Vulkan): retained FidelityFX/FSR sources, adapted to the Metal backend. AMD's GCN PM4/register headers retain their own notices under `gpu/third_party/gcn`. |
| **Omar Cornut** and Dear ImGui contributors | [`ocornut/imgui`](https://github.com/ocornut/imgui): in-game settings and co-op interface. |
| **Sam Lantinga** and SDL contributors | [`libsdl-org/SDL`](https://github.com/libsdl-org/SDL): window/input/controller/audio support. |
| **Alex Barney / Thealexbarney** | [`Thealexbarney/LibAtrac9`](https://github.com/Thealexbarney/LibAtrac9): AT9 decoding. |
| **FFmpeg contributors** | [`FFmpeg/FFmpeg`](https://github.com/FFmpeg/FFmpeg): audio/media decoding. |
| **sirit contributors** | [`shadps4-emu/sirit`](https://github.com/shadps4-emu/sirit): runtime SPIR-V assembler used by the shader recompiler. |
| **Khronos Group** and contributors | [`SPIRV-Headers`](https://github.com/KhronosGroup/SPIRV-Headers), [`SPIRV-Cross`](https://github.com/KhronosGroup/SPIRV-Cross), [`Vulkan-Headers`](https://github.com/KhronosGroup/Vulkan-Headers), [`Vulkan-ValidationLayers`](https://github.com/KhronosGroup/Vulkan-ValidationLayers) and [`glslang`](https://github.com/KhronosGroup/glslang): shader/interface definitions, SPIR-V-to-MSL, retained format-compatibility tables and shader build tools. |
| **Mesa / RADV contributors** | [`chaotic-cx/mesa-mirror`](https://github.com/chaotic-cx/mesa-mirror): cited sampler-mapping reference in the retained video core. |
| **red-prig** and fpPS4 contributors | [`red-prig/fpPS4`](https://github.com/red-prig/fpPS4): register-default tables retained through shadPS4. |
| **OpenOrbis contributors** | [`OpenOrbis/OpenOrbis-PS4-Toolchain`](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain): PS4 service definitions referenced in retained headers. |
| **kkAyataka** | [`kkAyataka/plusaes`](https://github.com/kkAyataka/plusaes): AES implementation retained in the shared common sources. |
| **Seladb** and PcapPlusPlus contributors | [`seladb/PcapPlusPlus`](https://github.com/seladb/PcapPlusPlus): macOS route-query adaptation in the network utility code. |
| **Victor Zverovich** and fmt contributors | [`fmtlib/fmt`](https://github.com/fmtlib/fmt): formatting. |
| **Yann Collet** and xxHash contributors | [`Cyan4973/xxHash`](https://github.com/Cyan4973/xxHash): hashing. |
| **Rich Geldreich** and miniz contributors | [`richgel999/miniz`](https://github.com/richgel999/miniz): compression/archive readers. |
| **Neargye** and contributors | [`Neargye/magic_enum`](https://github.com/Neargye/magic_enum): enum reflection. |
| **Tessil** and robin-map contributors | [`Tessil/robin-map`](https://github.com/Tessil/robin-map): hash containers. |
| **Boost authors** | [`boostorg/boost`](https://github.com/boostorg/boost): header libraries. |
| **Christian Rau** | [`half`](https://half.sourceforge.net/): half-precision arithmetic. |
| **Yuji Hirose / yhirose** and cpp-httplib contributors | [`yhirose/cpp-httplib`](https://github.com/yhirose/cpp-httplib): HTTP client. |
| **Google / Protocol Buffers contributors**; shadexternals maintainers | [`protocolbuffers/protobuf`](https://github.com/protocolbuffers/protobuf) and [`shadexternals/protobuf`](https://github.com/shadexternals/protobuf): client/server wire codecs and the pinned server submodule. |
| **Google / Abseil contributors** | [`abseil/abseil-cpp`](https://github.com/abseil/abseil-cpp): fetched by the server's newer protobuf build. |
| **OpenSSL Project** contributors | [`openssl/openssl`](https://github.com/openssl/openssl): cryptography and TLS. |
| **Thomas Bernard** and MiniUPnP contributors | [`miniupnp/miniupnp`](https://github.com/miniupnp/miniupnp): optional router mapping. |
| **Niels Lohmann** and JSON contributors | [`nlohmann/json`](https://github.com/nlohmann/json): JSON. |
| **Zyantific / Zydis and Zycore contributors** | [`zyantific/zydis`](https://github.com/zyantific/zydis), [`zyantific/zycore-c`](https://github.com/zyantific/zycore-c): x86 instruction decoding for admitted game hooks. |
| **Mitsunari Shige / herumi** and Xbyak contributors | [`herumi/xbyak`](https://github.com/herumi/xbyak): x86 hook generation. |
| **Loup Vaillant** and Monocypher contributors | [`LoupVaillant/Monocypher`](https://github.com/LoupVaillant/Monocypher): retained co-op cryptography. |
| **Jean-loup Gailly, Mark Adler** and zlib contributors | [`madler/zlib`](https://github.com/madler/zlib): compression used by the network dependencies. |
| **Baptiste Lepilleur / JsonCpp contributors**; **Upcaste contributors** | [`open-source-parsers/jsoncpp`](https://github.com/open-source-parsers/jsoncpp), including its cited [`upcaste/upcaste`](https://github.com/upcaste/upcaste) string helper: source retained inside SPIRV-Headers' header-generation tools. |
| **DejaVu and Bitstream Vera authors** | [`dejavu-fonts/dejavu-fonts`](https://github.com/dejavu-fonts/dejavu-fonts): bundled fallback font; [retained font license](gpu/third_party/fonts/LICENSE-DejaVu.txt). |
| **Python Software Foundation / CPython contributors; Gregory Szorc, Astral and python-build-standalone contributors** | [`python/cpython`](https://github.com/python/cpython), [`astral-sh/python-build-standalone`](https://github.com/astral-sh/python-build-standalone): scripts and packaged standalone Python. |
| **Qt Company** and Qt contributors | [`qt/qtbase`](https://github.com/qt/qtbase), [`qt/qthttpserver`](https://github.com/qt/qthttpserver): server framework and HTTP services. |
| **Mamba and conda-forge contributors** | [`mamba-org/mamba`](https://github.com/mamba-org/mamba), [`micromamba-releases`](https://github.com/mamba-org/micromamba-releases), [`conda-forge/qt-main-feedstock`](https://github.com/conda-forge/qt-main-feedstock): Docker Qt build dependency setup. |
| **aqtinstall contributors** | [`miurahr/aqtinstall`](https://github.com/miurahr/aqtinstall): optional Docker Qt installer path. |
| **Thomas Orozco / krallin** and Tini contributors | [`krallin/tini`](https://github.com/krallin/tini): server container init. |
| **Wei Dai** and Crypto++ contributors | [`weidai11/cryptopp`](https://github.com/weidai11/cryptopp): optional separate PKG extraction tool. |

Dependency pins and package notices are maintained in the bootstrap/package scripts and each component's directory. System and container dependencies also retain their own upstream notices.

### Research references and historical foundations

- **Yharnam-Hunters / Paleblood contributors**: [`Yharnam-Hunters/Paleblood`](https://github.com/Yharnam-Hunters/Paleblood), with cited analysis/test-reference work in [`evill33t/Paleblood`](https://github.com/evill33t/Paleblood). The [system-module](docs/SYSMODULE_WIRE.md), [patch-safety](docs/PATCH_SAFETY.md) and [timing](src/fps/PROVENANCE.md) notes identify exact revisions and distinguish evidence from ported implementation.
- **vladtepes22**: [`NightwatchCoop`](https://github.com/vladtepes22/NightwatchCoop/tree/db7c9fb2f07a007a9bb53d3056a425c887c9d4b3) effect/state/VFX and SaveRequest research. No Nightwatch implementation was copied into the local invasion adapters.
- **Soulsmods / Paramdex contributors, Natalie Weizenbaum and Smithbox contributors**: [`soulsmods/Paramdex`](https://github.com/soulsmods/Paramdex/tree/292ac5c10ace71116466cf8cd99c7dc034ef7602/BB) field definitions and effect names used to inspect the player's own covenant/reward parameters. **Grimrukh and Soulstruct contributors**: [`Grimrukh/soulstruct`](https://github.com/Grimrukh/soulstruct) event readers used for that research. No game parameter or event assets are distributed.
- **Michal Drobot**: [`ShaderFastLibs`](https://github.com/michaldrobot/ShaderFastLibs); **Academy / ACES contributors**: [`ampas/aces-dev`](https://github.com/ampas/aces-dev). The retained AMD shader header cites these mathematical/color references.
- **Khronos / MoltenVK contributors**: [`KhronosGroup/MoltenVK`](https://github.com/KhronosGroup/MoltenVK), used by earlier renderer versions. **Huax-1839**: [`bloodborne-macos`](https://github.com/Huax-1839/bloodborne-macos), credited for the earlier argument-buffer advice. The current direct Metal renderer does not load MoltenVK.
- **FromSoftware and Sony Interactive Entertainment**: original Bloodborne game and content. This is an independent community project.

### Mac-port contributors and testers

In addition to Mark Hall and Ronny Stiftel, retained Mac contributions include **[Noel Miller](https://github.com/noelmiller)** (native GPU host, macOS 27, input, memory and online foundations), **[Mojahed Abushawish](https://github.com/MojahedAbushawish)** (texture compression, mouse camera, memory and renderer fixes), **[DiAvisoo](https://github.com/DiAvisoo)** (ultrawide, launch checks, audio diagnostics and Stop), **[satperm](https://github.com/satperm)** (About, updates, logs and crash reports), and **[soulripper13](https://github.com/soulripper13)** (Dock icon). **zackcage6** contributed Windows testing and bug reports. Thanks to the other upstream contributors and everyone who supplied reports, logs and gameplay measurements; their individual authorship remains in Git history and source notices.

## License

The existing port, shadPS4 and imported server sources retain **GPL-2.0-or-later** notices. The native-menu/adaptive-timing code and The Hunter's Dream backend include **GPL-3.0-or-later** sources; the combined runtime is distributed under GPL-3.0-or-later. See [`LICENSE`](LICENSE), [`LICENSES/`](LICENSES/), [online provenance](docs/ONLINE_PROVENANCE.md), [timing provenance](src/fps/PROVENANCE.md) and the [server's retained licenses](server/LICENSES/).

Third-party libraries retain their individual licenses and copyright notices. Packaging copies the corresponding notices; this README supplements those records rather than replacing them. No game files or server/player secrets are included.
