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

The owner requested that the next step be our own backend on SSH host `opc`, under `/docker`
with `docker-compose.yaml`. No SSH connection, server changes, Docker deployment or firewall
changes have been performed in this checkpoint. Inspect the host and existing compose setup
before preparing that deployment. Pin the server revision and then resume the client against
that protocol; retain its authentication and build/asset compatibility checks.

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
