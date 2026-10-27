# Seamless co-op on macOS

This branch integrates the online client from
[lKaniel/bloodborne-server](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1),
pinned at `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`. The default server is
`bloodborne.phobos.services`. This repository builds the client and includes the separately hosted [server component](../server/LOCAL_FORK.md).

## Start playing

Build dependencies with `bash scripts/macos/bootstrap_deps.sh`, then run
`bash build_macos.sh`. Both online modules are included by default. Choose **Seamless Co-op**
in the **Game mode** selector at the top of the launcher, then press **Play**. This enables
co-op on `bloodborne.phobos.services`; its server and host rules are in the **Online** tab.
Select Continue or Load Game to choose your character; its team screen opens before entering
the world.

Choose **Regular** to return to normal gameplay and your normal save folder. Switching modes
keeps your co-op saves, server settings and accounts. The choice is remembered for the next
launch, including when **Show launcher every time** is off. New configurations start in Regular
with online play off. Existing regular online pools and accounts retain their selection;
The Hunter's Dream and shadNet remain available in the **Online** tab.

For a command-line launch, set the following in `bbport.ini`:

```ini
online=1
online_pool=seamless
seamless_server=bloodborne.phobos.services:31314
seamless_host_rules=off
seamless_harder_enemies=off
```

Registration is automatic. A persistent account belongs to each server and is stored in an
owner-only file under `seamless/identities/` in the data folder. Keep this file when moving or
backing up the client; deleting it creates a different account. Credentials are not printed.
Co-op saves live under `user-seamless/<server-hash>/` and are seeded once from a copy of the
existing `user/savedata`, or the launcher's custom save folder when selected. Offline saves
are preserved. An explicit `BB_USER_DIR` selects that folder instead. `BB_SAVE_DIR` is used as
the seed and then routed to the chosen co-op folder's `savedata`. Game assets remain unchanged:
checked event copies and links form a temporary
private game view, removed on exit. Generated event caches include input and output hashes.

Co-op uses the reference's fixed 60 FPS game timing. The renderer still runs in the native
arm64 Metal host; game hooks and networking run in the x86-64 Rosetta game process.

## Controls and features

| Control | Action |
| --- | --- |
| F6, or Co-op Team in the settings overlay | Team creation, invites by name/code, online hunters, readiness, world selection, join/leave and death mode |
| F8, or hold the controller touchpad | Rendered terrain map, lamps, saved exploration, cleared areas, teammates and own bloodstain; travel to lit lamps |
| G, or double-tap the touchpad | Ping where the camera aims; right-click the map to ping a location |
| A single touchpad tap | The game's original gesture menu, after the double-tap interval |
| F7, or L3 + Square | Free camera when **Team > Online display > Free camera hotkey** is enabled; use again to return |
| Team menu > Trade items | Offer carried goods, equipment or Blood Echoes; confirm the offer, then the recipient accepts or declines |
| Attack a downed teammate | Build revive progress; holding Circle while downed gives up |
| Options during a cutscene | Vote to skip |

The co-op screens follow the Windows reference's presentation: framed hunter cards, ready and
leader marks, medallions, silver dividers, and a shared loading screen. Live portraits appear
in the party HUD, compass and map when received; hunter sigils fill in before a portrait is
available. The HUD uses the game's own HP-bar art with vial/bullet counts and distinct downed,
dead and elsewhere states. Downed players get a revive ring and give-up progress; duel
countdowns and connection/invasion notices remain visible without a permanent debug panel.

Team, trade, map and invitation dialogs use the player's game menu textures/font, with
fallbacks when files cannot be read. These styles apply only to co-op; macOS settings retain
their existing presentation. Standard ImGui controls keep keyboard/controller navigation;
the primary hunt or return action gets initial focus. Custom co-op artwork is embedded in
both GPU library slices, so the packaged app has the same presentation. Original authors,
sources and license details are recorded in [provenance](../gpu/bbseamless/PROVENANCE.md).

The client retains the pinned reference's shared world/events/enemy ownership and HP,
independent travel and session restoration, team HUD/portraits/compass/pings, down/revive/wipe
and spectating, rot, trading (including equipment payloads), chalice sharing, bloodstain
retention, boss/event adaptations, shared trophies, optional enemy scaling/harder enemies,
team duels, and invasion requests. Incoming trophies are recorded in the co-op save folder's
`trophies.log`, using the port's existing `id timestamp` format, without echoing them back.
The map defaults to **Rendered terrain**. It draws the player's own MSB placements, FLVER
geometry and BC1/BC7 textures, with baked lighting, color grading, point lights and collision
floors. No game assets or premade terrain atlas are bundled.

In **F8 > View**, choose **Rendered terrain**, **Prerendered**, **Live 3D**, or
**Coordinates**. Prepared areas join one atlas using the reference's world offsets, with
localized place and lamp names. The selected area stays sharp; other areas use cached overview
pictures. The live Metal view combines prepared geometry with a shared depth buffer. Far areas
use simplified geometry; generation is asynchronous, so newly requested areas appear as ready.

On the map, the left stick or WASD pans; L2/R2 or -/= zooms. The right stick rotates/tilts live
3D and zooms a flat view. Q/E or L1/R1 changes area, Triangle/Tab frames the whole atlas,
Options/C recenters, L3/V switches 2D/3D, and R3/F switches automatic floors, roofs and floors.
A central crosshair snaps toward nearby lit lamps; Cross/Enter or clicking a lit lamp asks before
travelling. Square or right-click places a ping on the area's sampled terrain height, with the
player's height as fallback where no surface is available. Mouse dragging pans, the wheel zooms,
and a right drag rotates live 3D. The guide switches between keyboard caps and game button art.

**Team > Online display** controls compass visibility/position (top, bottom, or around the
middle), bloodstain visibility, team pings, the ping key, the free-camera shortcut and menu
language. The reference's 20 language choices and automatic game-language selection are
available. Item, lamp and area names come from the corresponding game message files, with an
English fallback when that language is absent. The world browser shows creation/last-played
information, lamp counts and characters, disables busy worlds and confirms forgetting one.

**Fog of war** reveals explored ground and areas around the player and lit lamps; cleared areas
show fully. The UI receives saved exploration for the atlas's other areas in the background.
View and fog choices persist as `map_view=0|1|2|3` (coordinates/terrain/live/prerendered) and
`map_fog=0|1`. The default remains rendered terrain.

The launcher's **General > Rendered maps > Prepare maps…** generates maps before playing,
shows prepared/total progress and supports cancellation. `bbmap-prepare` is packaged with the
app. It uses the same cache and never writes to game files. An area's first in-game request
also generates it on a utility worker. Compressed `.bbmap` and `.bbmesh` files live under
`map/metal-v1-<asset-signature>/` in `BB_GPU_USER_DIR`; prerendered angles use `_v1` through
`_v4`. The signature includes map-file names, sizes and modification times, following mod-view
links. Texture edits preserving both size and timestamp require manual cache removal.
Full-resolution charts use a 128 MiB LRU budget (one oversized chart may exceed it); overview
pictures and coarse geometry remain for the atlas. The selected area's full mesh is retained
separately. Metal resources stay alive through command-buffer completion.

The team leader's launcher settings choose rot, group travel, travel voting and enemy scaling.
The harder-enemy field accepts `off` or three multipliers from 1 to 10 (HP, damage, poise).
Trade offers reserve the sender's items until answered; stale offers and unavailable inventory
are checked again on the game thread. World deletion requires an explicit confirmation.

## Server and compatibility

| Service | Default endpoint |
| --- | --- |
| Authenticated encrypted co-op link | UDP `bloodborne.phobos.services:31314` |
| Web API | HTTP `bloodborne.phobos.services:31315` |
| Game discovery, messages, ghosts and bloodstains | HTTP `bloodborne.phobos.services:20443` |

Clients use an ephemeral outbound UDP socket; they do not require a router port forward.
Hosted mode tunnels the internal RPC service over the authenticated UDP link; TCP 31313 stays
on the server's loopback interface. The default host's link public key is pinned:
`1562e07491a27d6c7aea2ed37364b8839a4a924886afae1ff6eae5c483ea3424`.
HTTP endpoints retain the upstream linked-client admission rules. They use the upstream HTTP
protocol; only the co-op link is encrypted. Existing THD HTTPS verification remains enabled.

`seamless_server` or `BB_SHADNET_SERVER` overrides the link endpoint.
`BB_SHADNET_WEBAPI` and `BB_SEAMLESS_SERVICES` override its HTTP endpoints.
`BB_SHADNET_SERVER_KEY` can explicitly pin another server. A custom host never inherits our
host's key. `host_overrides.custom.json` in the chosen save folder takes precedence over the
generated HTTP routes. Advanced explicit credentials require both `BB_SHADNET_NPID` and
`BB_SHADNET_PASSWORD`.

The server runs the pinned development protocol: release `dev`, frame protocol 9, link version
4, with hosted admission, proof-of-work registration and build checks enabled. Invasions were
enabled on our server on 2026-10-27. A leader opts in using **F6 > Allow invasions**; a player
outside a team uses **F6 > Search for an invasion**, then **Leave invasion** to return.
The server retains its level/area/slot restrictions and cooldowns; no extra firewall ports are
needed. Turning a hunt's allowance off also ends a visit.

The Mac client enables the reference's hostile compatibility fallback because the server's
placeholder native kind/sign otherwise produced a friendly cooperator. Two actual Mac clients
verified matching, notice, entry, native PvP damage, host cancellation and return to the
invader's own lamp with world flags restored. The follow-up adds native red appearance and
server-issued victory rewards. Native retail kind/sign and mixed Windows/Mac invasions
remain unverified. See the dated invasion follow-ups in the
[validation record](SEAMLESS_COOP_VALIDATION.md).

The appearance uses the game's own invasion VFX from effect 9026. A guarded, otherwise unused
runtime parameter row carries that visual independently of the cooperator session controller;
the port's existing full-HP behavior is retained. Only the identified invader receives it, on
its own screen and the defenders' screens, and it is removed when the visit ends. Original
game parameters on disk stay untouched; conflicting modified rows are left alone.

Our server's victory policy awards **one Insight**, plus Echoes calculated from the defeated
player's level: 10% for an invading winner and 30% for the defending host. The documented
previous-level polynomial applies at levels 10–544; unsupported or unknown levels award
Insight only. This is the server's explicit policy, without a claim of exact retail economy.
The server pays a joined invader's death or an invader-attributed host kill; cancelling,
disconnecting, disabling invasions, a boss entry, timeout or party wipe pays nothing.

Rewards use an authenticated player's persistent mailbox and the existing item-grant path.
Updated clients opt in through an appended lobby capability; older clients receive no new
reward item type. Solo reward acknowledgements use the lobby stream even without a party.
Ordinary item offers/refunds cannot issue Insight. Server source and rollback details are in
[the deployment record](SEAMLESS_INVASION_REWARDS.md).

Covenant rewards follow the original native PvP item lots: a winner wearing Corruption gets
one Blood Dreg in addition to Insight/Echoes. Radiance, Hunter and blood-addled Hunter PvP lots
contain no additional item. The server freezes the native oath marker for the visit; changing
it during combat cannot change the payout. Inventory overflow uses the game's storage path
(99 held / 99 stored); a completely full inventory retains the grant until space is available.

Invasion payouts are acknowledged only after a fresh native character save and its receipt
are committed together. The Mac runtime stages the complete online save directory, verifies
the character write, flushes it and exchanges directories atomically. Native backup fallback
also contains the credited character image. Reconnecting after a crash replays an uncommitted
grant or acknowledges an existing matching receipt without awarding again. Keep the complete
per-server save directory, including its hidden receipt folder, when moving it. External save
editing/restoration that removes those receipts is outside this transaction guarantee.

Hook admission checks the supported 1.09 title IDs and original instruction signatures before
mutation. The known 1.09 executable with stale 1.00 SFO metadata is accepted only by its exact
container SHA-256. Unknown images, conflicting hook bytes, missing libraries or ABI mismatch
stop launch with an error. Module identity is derived from its source contents. The cross-
process UI uses versioned fixed-width snapshots and bounded command/model queues; no guest
pointers or x86 callbacks cross into the arm64 renderer.

## Validation on 2026-10-26

Two real Mac game processes joined the same persistent server world and rendered one another.
The test used isolated copies of a fresh base-game save, the player's original game assets,
and the native Metal renderer. Metal validation was enabled for the extended runs.

- Both clients authenticated automatically; all 62 checked co-op feature groups were enabled.
- A 10 Blood Echo trade changed the sender from 300 to 290 and recipient from 300 to 310.
  The visible trade UI also sent a separate offer; declining returned its reserved echoes.
- Actual Metal portrait readback crossed to the game process, was sent to the server, and
  arrived at the other client. Map, lamps, compass and pings displayed live team state.
- A lethal fall put the guest down. The host's attacks completed revival, and the guest
  recovered at half HP. Both clients asserted the down-to-up transition.
- A team duel reached the countdown and fighting state; cancellation restored the saved state.

Local evidence lives under `out/ingame-seamless-2026-10-26/` (excluded from Git). Scripts and
logs distinguish successful assertions from failed fixture attempts; forced death is not a
revive test, and `cut-link` takes seconds. Original seed hashes were checked after each run.
Final reconnect/travel results are recorded in `docs/SEAMLESS_COOP_VALIDATION.md`.

`build_macos.sh --test` includes map format/raster/cache tests and camera/ping/fog tests on
both architectures, the launch identity/save/route checks, actual dylib ABI and
fail-closed image checks, Mac socket readiness/Mach protections/touch gestures, shared trophy
persistence, existing online imports/codec/session tests, runtime pad tests, launcher model
checks and process-ring tests. See [source provenance](../gpu/bbseamless/PROVENANCE.md).

This is bounded gameplay validation, not a complete campaign or DLC/chalice/boss playthrough. Those source paths are integrated, but their complete in-game behavior and a
mixed Windows/Mac session remain unverified.
