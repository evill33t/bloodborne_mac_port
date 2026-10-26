# Seamless co-op on macOS

This branch integrates the online client from
[lKaniel/bloodborne-server](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1),
pinned at `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`. The default server is
`bloodborne.phobos.services`. This repository builds the client; the backend runs separately.

## Start playing

Build dependencies with `bash scripts/macos/bootstrap_deps.sh`, then run
`bash build_macos.sh`. Both online modules are included by default. Choose **Seamless Co-op**
in the launcher's **Online** tab and enable **Play online**. Select Continue or Load Game to choose your character; its team
screen opens before entering the world. A new configuration selects
Seamless; existing explicit THD/shadNet choices and accounts retain their pool. Offline play
remains the default until online play is enabled.

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
| F8, or hold the controller touchpad | World map, lamps, saved exploration, cleared areas, teammates and own bloodstain; travel to lit lamps |
| G, or double-tap the touchpad | Ping where the camera aims; right-click the map to ping a location |
| A single touchpad tap | The game's original gesture menu, after the double-tap interval |
| F7, or L3 + Square | Free camera; use again to return |
| Team menu > Trade items | Offer carried goods, equipment or Blood Echoes; confirm the offer, then the recipient accepts or declines |
| Attack a downed teammate | Build revive progress; holding Circle while downed gives up |
| Options during a cutscene | Vote to skip |

The client retains the pinned reference's shared world/events/enemy ownership and HP,
independent travel and session restoration, team HUD/portraits/compass/pings, down/revive/wipe
and spectating, rot, trading (including equipment payloads), chalice sharing, bloodstain
retention, boss/event adaptations, shared trophies, optional enemy scaling/harder enemies,
team duels, and invasion requests. Incoming trophies are recorded in the co-op save folder's
`trophies.log`, using the port's existing `id timestamp` format, without echoing them back.
The map uses a coordinate chart and exploration shading; it does not bundle a terrain atlas.

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
4, with hosted admission, proof-of-work registration and build checks enabled. Invasions are
disabled by the upstream server default on this deployment. The client exposes the requests
and allow-invaders switch; enabling that server feature and testing an actual invasion remains
separate work. No Windows/Mac interoperability claim follows from the Mac/Mac test.

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

`build_macos.sh --test` includes the launch identity/save/route checks, actual dylib ABI and
fail-closed image checks, Mac socket readiness/Mach protections/touch gestures, shared trophy
persistence, existing online imports/codec/session tests, runtime pad tests, launcher model
checks and process-ring tests. See [source provenance](../gpu/bbseamless/PROVENANCE.md).

This is bounded gameplay validation, not a complete campaign or DLC/chalice/boss/invasion
playthrough. Those source paths are integrated, but their complete in-game behavior and a
mixed Windows/Mac session remain unverified.
