# Mac seamless client validation - 2026-10-26

Branch: `codex/seamless-coop-client`. Reference/server commit:
`8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`.
Server: `bloodborne.phobos.services`, public-key pin recorded in the client setup guide.

## Real gameplay

Two independent x86-64 game processes and two native arm64 Metal GPU hosts ran concurrently
on one Apple Silicon Mac, using macOS 27 and Rosetta 2. Each client had a separate generated
server account and copied base-game save. Extended runs enabled `MTL_DEBUG_LAYER=1`.
Fixed 60 FPS describes simulation timing; two simultaneous validation clients rendered around
17-45 FPS in the captured scenes. This was not a performance benchmark.

| Check | Observed result |
| --- | --- |
| Authentication and hooks | Both accounts linked to the real public backend; 62/62 feature groups enabled |
| Team and world | Invite/accept/start; two session members and two map members; remote player visible |
| Shared world/enemies | Server world/event updates applied; one owner and one follower synchronized enemy records |
| Trade | Sender 300 -> 290 echoes; recipient 300 -> 310, checked against live inventories |
| Visible trade UI | Open inventory, select echoes, confirm offer; recipient decline returned reserved echoes |
| Portraits | Real 560x768 Metal readback, game-process handoff, server transmission and peer portrait receipt; HUD displayed the portrait |
| Map and ping | Named area/lamps, both player positions, teammate compass, shared aim ping; saved exploration snapshot |
| Down/revive | Guest's 1-HP lethal fall entered Down; host's attacks completed revive; guest recovered at 297/594 HP |
| Duel | Proposal and acceptance; 3/2/1 countdown; fighting; cancellation restored HP, inventory and team state |
| Free camera | Enabled and disabled in the running game, asserted by the game-side camera state |
| Link interruption | Dropped host's link for four seconds; session and both map members recovered |
| Independent travel | Host travelled to Dream warp 2102962 and back to Central Yharnam 2412950; guest stayed connected |
| Original data | Seed save hashes unchanged after every run; 54 generated-event input checks and original eboot hash matched |
| Metal validation | No Metal validation assertion or GPU failure in the successful extended runs |

Evidence is local and excluded from Git:
`out/ingame-seamless-2026-10-26/validation.json`, per-client `game.log`,
`user/log/seamless.log`, `result.json`, `autoplay.txt`, `final-coop.png` and `shots/`.
`pair-1/` through `pair-4/` preserve earlier attempts. The successful trade/revive/duel
assertions are in `pair-3/`; final reconnect/travel/camera assertions are in the current logs.
Both final profiles report `seed_unchanged: true`. Test processes were stopped after capture.

The test scripts used the reference autoplay driver to control the real game, including
fixture-only inventory stock, positioning, godmode for non-combat travel checks and a lethal
fall. These controls are not exposed in the shipped co-op menus. The visible trade interaction
was also exercised through the actual native UI. Travel assertions used the game-side travel
command; the map correctly disabled unlit destinations, but a full lit-lamp controller trip
was not separately recorded.

Earlier fixture errors were retained, not counted as successful checks: `mark` expects a
character target (use `log` for labels), `kill self` forces full death (use the upstream lethal
fall for revive), and `cut-link` takes seconds. Gameplay found and fixed Mac transport guards,
HUD visibility observation, host-only rule publication and free-camera initialization.

## Build and focused checks

The canonical `build_macos.sh` built the x86-64 game/runtime, legacy online module, seamless
module, arm64 GPU library and both native host variants. The normal dependency bootstrap
installs pinned Zydis, its required Zycore package and Xbyak headers. No temporary source
checkout is required for a normal build.

Focused checks cover persistent per-server identities, private credentials and save seeding,
HTTP routes/key pin/overrides, the real dylib's ABI and NID imports, rejected hook images
remaining unchanged, kqueue readiness/EOF/failed modifications, Mach permissions, touchpad
hold/double-tap behavior, shared trophy persistence/deduplication, runtime pad handling,
legacy online codec/session behavior, launcher settings and shared process rings.
The canonical `build_macos.sh --test` completed with exit status 0. The final Python launch
suite has five passing cases, including custom save-folder isolation. The shell settings suite
also verifies legacy-credential isolation when Seamless is explicitly selected. Results are
retained in `out/mac-tests-final.log`, `out/seamless-launch-final.log` and
`out/seamless-package.log`.

The signed self-contained app (code revision `89293cb`, 155 MB; ZIP 55 MB) passed dependency
checks, a real dylib ABI/image-guard probe and deep strict signature verification. A further
in-game run used its bundled Python, scripts, runtime and Metal host: automatic authentication,
62/62 hooks, world entry, map and Team menu all worked. The launcher's custom save selection
was copied to the per-server namespace and its original files remained unchanged. The app's
signature still verified after gameplay. Evidence: `out/ingame-seamless-2026-10-26/package/`.
All test processes were stopped; no app was installed over the user's build.

## Coverage limits

This is Mac/Mac evidence from one machine and a real remote backend. Mixed Windows/Mac play,
a full campaign, boss completion, DLC, chalice progression, spectating/wipes/rot, group-travel
voting, item/equipment trades beyond echoes, cutscene voting and shared trophy delivery were
not all individually exercised in-game. Their reference implementations and client controls
are integrated; trophy persistence also has a focused native test. Legacy retail messages,
ghosts and bloodstains use the configured backend endpoints, but each was not separately
placed/replayed in this run. That initial map was a coordinate/exploration view; terrain maps
were added in the follow-up below. Invasions were enabled and tested in the follow-up below; their
retail red tint/native kind/sign, rewards/covenants and mixed Windows/Mac behavior remain
unverified.

The parity follow-up below adds explicit equipment/gem trading, spectator recovery and
group-travel refusal coverage; those were limits of the initial run above.


## Invasion follow-up - 2026-10-27

Enabled `Invasions=true` persistently on the existing `bloodborne-server` Compose service.
Only that container was restarted, with no players connected. It is healthy, retains the
same link identity, and all 49 unrelated containers kept their IDs and start times. The
original configuration is backed up under
`/docker/backups/bloodborne-invasions-20261027T111142Z/` on `opc`. Teams still default to
refusing invaders. No admission/build/HTTP rules, cooldowns or firewall ports were loosened.

The initial real-game test matched, showed the ten-second notice, joined a two-member game
session and returned home, but the upstream placeholder native kind/sign left the invader
friendly. Enabled the reference's existing hostile fallback for the Mac client, then repeated
in a fresh isolated team world with the same two test accounts:

- The invader had local character type 2 and team 21; its body on the host was also team 21.
- A real R1 attack, without writing the target's HP, reduced the host from 594 to 579 HP.
  Both the attacker's peer HP and the host's own HP confirmed the same change. Godmode was off
  on the host for this check; the nearby enemy was disabled to isolate PvP damage.
- The host turned **Allow invasions** off. Both games received `INVADERS_OFF` and the invader
  returned to its own lamp 2412959. Its saved world flags were restored (161 changed back),
  and its temporary invasion-home marker was removed.
- The invader stayed out of the team/party HUD. No failed test steps or Metal validation
  assertions occurred in the successful fallback run. Original seed save hashes were unchanged.

Evidence: `out/ingame-invasions-2026-10-27/validation.json`, per-role `hostile-fallback/`
logs/scripts/results, `shots/hostile-arrived.png`, `shots/pvp-damage.png` and `shots/home.png`.
The earlier friendly native-kind attempt is retained in `native-kind/`. Both clients were
stopped after the checks. The HUD now describes searching, arrival, active invasion and return
in ordinary text, without internal player IDs or numeric protocol phases.

## Rendered terrain and live Metal map - 2026-10-27

Reused the pinned reference's MSB/FLVER/TPF/BTL/collision readers and terrain rasterizer,
with a native utility worker/cache and Metal 3D pass. All terrain comes from the player's
own files. The coordinate fallback works while terrain is being generated or unavailable.
The optional 3D view renders a simplified mesh with vertex colors and detailed terrain
textures on top surfaces. Its walls are approximations, not the retail renderer's materials.

- Central Yharnam generated a textured chart in 14.6 seconds and its live mesh in 15.5 seconds
  on this Mac. The generated cache files were 8.5 MiB and 16 MiB. Both clients subsequently
  loaded the chart in 0.1–0.3 seconds and the host loaded its mesh in 0.2 seconds.
- Two isolated real Mac clients joined the same hunt on `bloodborne.phobos.services`. Both
  asserted a two-member game session and party. The flat and 3D views showed the teammate
  marker; the host's native right-click in its rotated 3D map produced a ping on the guest's
  flat terrain map. The scripted map-ping path also passed its native ping assertion.
- Checked the visible fog toggle, fit-area control, 46-degree live rotation, and
  closing/reopening the 3D view. Controls stayed visible after the layout fix.
  Mouse picking uses the camera's floor plane, not a ray through the roof/wall mesh.
- Metal validation was enabled. The successful two-client run had no failed autoplay steps,
  shader errors, GPU faults or Metal validation assertions. Original seed save hashes and
  executable hash remained unchanged. Both clients were stopped after the checks.
- Map parser/raster/cache and floor-replacement fixtures, projection/ping round trips, soft fog, mesh persistence,
  worker cache eviction and missing-area fallback pass on arm64 and x86-64. Real asset checks
  verified Yharnam's three lamps and Cathedral Ward's 92 collision meshes / 466,650 floor
  triangles, including a floor under the Grand Cathedral lamp. Settings round-trip and all
  five launch/save-isolation tests passed.
- `BB_LTO=OFF BB_NET=ON bash build_macos.sh --test` passed, including process-split, launcher
  and native Metal checks. The rebuilt 155 MiB app / 55 MiB ZIP passed deep strict signature
  verification. Its bundled Python/scripts, x86 runtime and arm64 Metal host authenticated,
  entered the world and displayed the live terrain map (`PACKAGED_TERRAIN_PASS`); both cache
  loads took 0.1 seconds. The app signature still verified after that game session.

Evidence: `out/ingame-terrain-2026-10-27/validation.json`, per-role scripts/logs/results,
`host/terrain-3d-rotated.png`, `host/reopened-3d.png`, `guest/terrain-full.png` and
`guest/received-map-ping.png`. This is bounded Central Yharnam gameplay and Cathedral Ward
asset parsing evidence; full DLC/chalice terrain coverage and mixed Windows/Mac play were
not exercised. No server deployment or firewall change was required.

## Windows-style co-op presentation - 2026-10-27

Adapted Kaniel / lKaniel's reference artwork and the `MenuArt`, `CoopArt`, `Team`, `Party`
and `Compass` presentation to the existing native Metal overlay. Retained original atlas
sources, hashes and GPL notices, and credited Mark Hall's Mac overlay and test harness.
Commit messages include original-source URLs, pinned revisions and author credits.

The UI now has four framed hunter cards with ready/leader marks and empty places, portrait
medallions on the party list/compass/map, the game's own HP-bar art and font, vial/bullet
counts, downed/dead/elsewhere states, revive/give-up indicators, and illustrated team loading.
Team, map, trading and question dialogs share the reference's silver/stone treatment.
Sigils substitute for portraits before a live image is available.

- Two actual game clients authenticated and joined the same remote hunt. Both asserted two
  session members and two party members. Real GPU portraits displayed in the HUD and team
  cards. The illustrated loading screen displayed each hunter's joining stage and percentage.
- Inspected the party HUD, team menu and terrain-map menu in-game. A visible layout warning
  in the initial card action buttons was corrected; the final run had no ImGui errors or
  Metal validation findings. Escape/F6/F8 navigation and the existing command bridge remain
  in use. Original seed save hashes stayed unchanged.
- `build_macos.sh --test` passed. Imported atlas tests check geometry/transparency, gray
  treatment and malformed files. A native Metal fixture renders team, HUD, downed, loading
  and trade states at 960x720, 1152x720, 1920x1080 and 3440x1440, checking input ownership,
  restoration of the Mac settings theme, ImGui errors, and unretained GPU resource lifetime.
  The fixture also passes without game art/fonts; game-file visuals were checked separately.
- Follow-up checks cover a zero-progress revive ring, silver checkbox styling and the trade
  inventory's flexible height, which reserves room for quantity/offer controls. The final
  development app includes artwork provenance in its licenses directory.
- The signed packaged app authenticated with its bundled Python/runtime, entered a real
  hunt, and displayed the themed Team and trade menus. Checked the trade footer and return
  controls. Its original selected saves stayed unchanged; no ImGui or Metal errors occurred.
  Deep strict signature verification passed. Focused Metal checks were repeated after the
  final layout/color/ring changes.

Evidence: `out/ingame-coop-ui-2026-10-27/` (initial `host/guest`, corrected
`final-host/final-guest`), `out/coop-ui-fixtures/` (synthetic states), and
`out/coop-ui-full-test.log`. Fixture downed/dead states are presentation checks; the earlier
real revival/PvP gameplay evidence remains separate. The Mac settings UI retains its existing
style, and the backend/protocol are unchanged.

## Windows client parity follow-up, 2026-10-27

Reference: **Kaniel / lKaniel**, `lKaniel/bloodborne-server` at
`8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`, particularly
[`bbport_overlay.cpp`](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/gpu/shim/bbport_overlay.cpp),
[`bbport_settings.cpp`](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/gpu/shim/bbport_settings.cpp),
and its menu translation table, key-guide layout and launcher Map card. Native Metal and
fixture foundations: **Mark Hall**; retained collision-reader lineage: **MikeTat1988 / Hunter's
Atlas**. See the source manifest and provenance for hashes and individual notices.

The identified client gaps now have native implementations:

| Reference behavior | Mac implementation |
| --- | --- |
| Joined map atlas and place labels | Original world grouping/offsets, prepared overview charts, localized place/lamp names, saved exploration updates for other areas |
| Prerendered and live 3D | Four cached oblique angles; native Metal depth composition of prepared areas, with coarse geometry for other areas |
| Controller map interaction | Analog pan/rotate/tilt, trigger zoom, crosshair, lamp snapping and confirmation, area/whole-atlas/recenter/floor/3D shortcuts, contextual game button glyphs |
| Compass and ping presentation | Top/bottom/ring layouts, compass pings, world-edge arrows, terrain-height map pings with height fallback |
| Display preferences and language | Persistent online preferences, 20 reference language choices/automatic selection, game message/font lookup with fallback |
| World browser and launcher preparation | World details/busy states/forget confirmation; packaged read-only map helper with visible progress/cancel |

`build_macos.sh --test` passed, including both architecture map tests, settings preservation,
launcher checks, module guards and native Metal tests. Subsequent focused checks cover the
rotated-atlas fit correction and LTO artwork/font fix. The presentation fixture covers nine
states at four sizes, including the four map projections and German/Japanese layouts, under
Metal API validation with unretained command buffers. It now requires the embedded artwork and
fallback font to be usable. LTO had reordered end labels: assembly-computed byte counts fix
both the artwork and font lengths. Both library slices report the actual binary sizes
(1,855,877 / 92,185 / 759,676 bytes for art/icons/font).

Real gameplay evidence is under `out/ingame-parity-2026-10-27/`. Two Mac game processes on one
machine connected to **bloodborne.phobos.services**, authenticated and installed **62/62**
hooks. The host used German menus and the ring compass, while the guest used English and a
bottom compass. Actual SDL input exercised stick pan, trigger zoom, whole-atlas framing,
controller glyph selection, live-map rotation and crosshair pings. Peer logs record matching
map IDs and world coordinates for the delivered pings. The map displayed German place/lamp
names, prerendered terrain, and multiple areas in the live Metal view. The standalone packaged
helper also prepared a real Central Yharnam flat/oblique pair; first oblique generation took
14.5 seconds.

The source-based `final-host`/`final-guest` gameplay scripts additionally verified:

- One offer transferred Oedon Tomb Key, Hunter Axe +5 and Charred Hunter Garb, with source and
  recipient inventory checks and the weapon's socketed gem/effect retained.
- A lethal fall in spectator death mode hid the guest's body and moved its camera to the host.
  Holding Circle woke the guest at its own lamp with 594 HP, without a map reload.
- The host requested a group trip, the guest voted No, and both remained in Central Yharnam
  with the two-member session intact.

An earlier `proof-guest` fall run failed while its nearby enemy remained active; its host also
suffered a real death. That failed run is retained. The successful repeat pauses that enemy's
AI and lets the positions settle, matching the existing source test approach. No death-core
change was made to manufacture a pass. This does not certify every instant-death/campaign path.
Original seed saves and the source eboot hash remained unchanged.

The final signed package repeated authentication, 62/62 hook admission and entry into a
remote hunt (`PACKAGED_PARITY_PASS`) using its bundled runtime, Metal host, scripts and
Python. Inspected the German live map and controller glyphs after rotating/tilting and
fitting the complete atlas; framing includes the prepared geometry's height bounds. The
world browser's scaled rows and host-rule labels were also checked in the packaged game.
No Metal/ImGui validation findings occurred. Deep strict signature verification passed
after gameplay; the original selected saves and game eboot hashes were unchanged. Evidence:
`package/final-world-browser.png`, `package/final-rotated-atlas.png`,
`package/final-controller-atlas.png` and the final `validation.json`. All test clients were
stopped after the checks.

These results establish the listed native client behaviors and the exercised Mac/Mac paths.
They do not certify Windows/Mac cross-play, complete campaign/boss/DLC/chalice progression,
every retail message/ghost/bloodstain action, or the upstream/backend's unknown retail invasion
kind/sign, tint and reward behavior. The Mac-specific renderer/settings/launcher remain native;
Windows Vulkan/provider-specific renderer features are outside this online-client integration.
