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
Full command and packaging results are retained in `out/mac-tests-final.log` and
`out/seamless-package.log`.

## Coverage limits

This is Mac/Mac evidence from one machine and a real remote backend. Mixed Windows/Mac play,
a full campaign, boss completion, DLC, chalice progression, spectating/wipes/rot, group-travel
voting, item/equipment trades beyond echoes, cutscene voting and shared trophy delivery were
not all individually exercised in-game. Their reference implementations and client controls
are integrated; trophy persistence also has a focused native test. Legacy retail messages,
ghosts and bloodstains use the configured backend endpoints, but each was not separately
placed/replayed in this run. The map is a coordinate/exploration view rather than the
reference's terrain atlas. Invasions remain disabled by the deployed upstream server default;
client requests alone do not establish working invasion gameplay.
