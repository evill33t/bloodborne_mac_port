# Seamless co-op client checkpoint

Paused at the owner's request on 2026-10-26. Branch: `codex/seamless-coop-client`.

The intended scope is every online client feature from
[lKaniel/bloodborne-server](https://github.com/lKaniel/bloodborne-server), connecting to a
separately hosted backend. No backend executable, deployment, game asset or save is included.

## Current state

- Started from upstream `master` at `c5fb32273ebbf40207e90344bb695c344f622de2`.
- Merged the published online foundation from PR #78 at
  `f83caab12b6faa07ff34f8e1f3434c77baeacc59`, retaining its ancestry and authorship.
- Imported the reference client's networking, game hooks and shared wire/value code from
  `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1` into `gpu/bbseamless/`.
- Added an initial standalone CMake target and host callback adapter. They are a prototype:
  **the new module does not build yet and is not connected to the runtime, launcher,
  packaging or Metal overlay.** The regular build does not include this target.
- Existing online behavior remains the PR #78 implementation. The imported seamless features
  are not available in the game on this branch yet.

## Intended feature coverage

The reference includes team creation/invites/readiness and joining a running team, shared
world/event/enemy state, independent travel, party HUD and portraits, compass/map/pings,
down/revive/wipe or spectating, item/weapon/echo trading, shared chalices, retained bloodstains,
optional harder/scaled enemies and host rules (rot and group travel/voting). Its client code
also includes invasion/duel support. These are targets for integration, not validated features
of this Mac branch.

The client must run in the x86-64 game process, using the existing native arm64 Metal GPU
host. Port the necessary snapshots/actions across that process boundary; do not replace the
renderer or load x86-64 game hooks into the GPU host. Keep offline play and the existing THD
backend selectable. Use an isolated co-op save directory and generated overlays from the
player's files. Refuse incompatible executable/hook/patch combinations before mutation.

## Server configuration and next step

The reference defaults to `127.0.0.1:31314` for the co-op link and
`http://127.0.0.1:31315` for the Web API. These are local-machine defaults, not a hosted public
co-op service. The project documents player-hosted servers and TCP 31313, 31315, 20443 plus
UDP 31314. A remote Mac client must use the actual host address, not its own loopback.

Our seamless client defaults to `bloodborne.phobos.services:31314`, with its Web API at
`http://bloodborne.phobos.services:31315` and game services at
`http://bloodborne.phobos.services:20443`, requested by the owner on 2026-10-26. The optional
module applies its network defaults only when no explicit environment override is present.
These defaults do not enable the unfinished module or change the existing THD backend.
Hosted mode keeps TCP 31313 on loopback; public clients need UDP 31314 and TCP 31315/20443.
The default host's public link key is pinned in `gpu/bbseamless/client.h`, read from the
deployed server over authenticated SSH. Explicit server-key overrides survive, and a custom
server does not inherit our host's key.

The backend is now deployed on SSH host `opc`, using the existing `/docker/docker-compose.yml`
(the active file uses `.yml`, not `.yaml`) and service `bloodborne-server`. It is built natively
for ARM64 from the pinned reference revision above. Operational files and the runbook are under
`/docker/bloodborne-server`; persistent state is under `/docker/data/bloodborne-server`.

Validation on 2026-10-26: eight focused upstream checks passed; an isolated 20-second bbload
simulation linked three players, started their team and relayed 2,768 packets without refusals
or measured loss. The live service is healthy, advertises `bloodborne.phobos.services`, returns
HTTP 200 for game discovery and WebAPI status, and refuses unlinked HTTP mutations with 403.
External checks through that hostname passed on TCP 20443/31315 and UDP 31314; the UDP cookie's
public key matches the key read over SSH. A controlled restart preserved the server identity;
an initial consistent data snapshot and the original Compose file were backed up under
`/docker/backups`. All 49 existing containers retained their IDs and start times.

Hosted mode, account/HTTP admission and build/protocol checks remain enabled. The reference is
a development build (release `dev`, co-op frame protocol 9, link version 4). The separate Mac
client integration still needs completing; no live game or Windows/Mac co-op is established by
the server checks. Resume the client against this pinned server protocol, retaining its
authentication and build/asset checks. The host firewalld rules and Oracle ingress allow only
the three public game ports; TCP 31313 remains bound to loopback and stats are disabled.

## Validation and remaining work

Standalone CMake configuration succeeded with the existing x86-64 dependencies, a temporary
Zydis 4.1.1 source checkout and a temporary Xbyak checkout. Compilation failed on incomplete
header dependencies (`orbis_error.h`, `error_codes.h`, `bbport_settings.h`) and the Linux
`sys/epoll.h` interface. No successful client library, runtime build, sign-in, game launch or
multiplayer test is claimed. Local build log: `out/seamless-build.log`.

On resumption: complete the dependency closure and reproducible pinned bootstrap; adapt
readiness/epoll and Mach memory/protection queries; finish module ABI/loading, checked hook
admission and guest callback lifetimes; port event/HUD generation and isolated saves; wire
team/HUD/map/trade/host-rule actions into the Mac UI and packaging; run focused transport and
hook tests, then authenticated gameplay and a real multi-client session against our server.
Matching the protobuf schema alone does not establish Windows/Mac compatibility: the reference
also checks build and asset identities.
