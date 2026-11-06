# Yharnam Together v1.4.0 integration

Reference: [Amanises-Yharnam-Together/BBSC-Yharnam-Together v1.4.0](https://github.com/Amanises-Yharnam-Together/BBSC-Yharnam-Together/releases/tag/v1.4.0),
`efa0ad265d840d9195e55db2e3216f46f7daedd0`, published 6 November 2026.
The previous source base was v1.3.1, `5d2305ee611ef1e03be39ff925f0da75c9a70393`.
Original authors include Kaniel / lKaniel, Amanises, Vitalii Shevchuk and the shadNet/shadPS4
and Yharnam Together contributors. Existing source and third-party notices are retained.
The native Metal overlay and fixture architecture retain Mark Hall's foundation.

## Included changes

The imported client and separately hosted server include five-hunter teams, team duels,
quick match, spectator following, leader-controlled team rules, phantom-glow control,
co-op memory-window/worker optimizations and the upstream session, world, enemy and progress
fixes. The Mac adapter carries five team/load rows, four companion portraits/bars, team-duel
selection and invitation details, and leader rule editing through version 7 UI records.
The runtime supplies its actual mapping-table generation through module ABI 5, so an unmap
or protection change invalidates the imported readable-window cache.

Our Travel Together lock-order fix and durable invasion reward/save/receipt handling remain.
The new server password-account and fair-play source is included without replacing those
extensions. Original and adapted hashes are recorded in the client and server manifests.
Windows Discord named-pipe integration remains a no-op on macOS; Mac Discord status/join is
not implemented. Production co-op builds disable imported autoplay and developer free camera.

## Public host

The native launcher's Online page offers **Private / local server** and
**seamlesshunt.com (Global)**. Private remains the default. Public signup, login, password
change/recovery and session revocation use the fixed, system-verified HTTPS endpoint;
redirects are refused. Passwords are transient. Only the signed session goes to Keychain,
under service `bbport-seamless-global` and account `seamlesshunt.com:<account-id>`.
INI settings contain the public ID, display name and expiry, never its password/session.
Global saves are scoped to both endpoint and account ID, without changing existing private
server save namespaces. Both known hosts pin their published encrypted-transport keys.
The client advertises the genuine source release `1.4.0` and its own source/content hashes.

Live validation on 6 November: native Swift HTTPS signup and sign-in returned the same
isolated account. The production Mac game then completed the public pinned-key UDP welcome,
shadNet account login, bearer-token exchange and authenticated HTTP 200 service calls. Its
team screen reported **Connected** and offered five places and quick match. No strangers
were invited or joined. The public server warned about the Mac build/event/mod fingerprint
differences; this warning is preserved. Connection acceptance does not establish mixed
Windows/Mac gameplay, fair-play review outcomes or full campaign compatibility.

## opc deployment

The ARM64 image `bloodborne-server:v1.4.0-mac-arm64` was built on opc with 20 passing focused
CTest suites, including password signup/region integration, signed accounts, config, team
lobby/duels and durable rewards/world persistence. An isolated container accepted two clients
for each of `dev` and `1.4.0` over real UDP; all four entered simulated play with zero refusals.

The production Bloodborne service alone was restarted after a stopped-data backup. It uses
`SignIn=open` explicitly to preserve private automatic identities, `CheckServerBuild=true`
and `CompatibleClientVersions=dev,1.4.0`. The image sets `BB_RELEASE_VERSION=1.4.0`.
The service was healthy afterward; all 37 existing worlds, the pinned transport identity,
static configuration and unrelated container IDs were preserved.

Image ID: `sha256:eb1002335cf4538deef56918df5769eeac1f1a4401a1edee1c2c6cf5fe111802`.
Rollback evidence and the closed-data archive remain private on opc under
`/docker/bloodborne-server/rollbacks/v1.4.0-20261106/`. Do not restore this snapshot over later
player progress. The previous image remains available.

## Local validation

- `BB_BUILD_TYPE=Release BB_LTO=ON BB_PRODUCTION=ON BB_BUILD_JOBS=6 bash build_macos.sh --test`
  passed, including runtime/save transaction, socket/module, Metal and native GPU split checks.
- Native launcher build/model tests passed. Twelve Python launch tests cover public login
  requirements, account save isolation, targeted Keychain lookup, expiry, host pinning,
  version, local routing and phantom glow.
- Actual Metal presentation fixtures passed at four window sizes with five hunter/load rows,
  localized text, team-duel invitations, unretained resources and input ownership. Keyboard
  and controller Travel Together confirm/cancel/stale-snapshot regressions passed.
- The bounded production travel-lock fixture passed. Memory-window checks reject stale/odd
  generations, inadequate permissions, zero sizes and overflow. Module initialization rejects
  hosts without the generation callback.
- Server import verification passed for 241 source files, four historical local patches and
  pinned protobuf. Python backend service checks passed (34 tests; nine optional compiled-server
  parity cases skipped locally). Optional Redis directory tests were skipped without fakeredis.

Private local evidence is under `out/bbsc-v140/` (not distributed), including server build/
deployment reports, native HTTPS smoke, game logs/screenshots, Metal fixtures and build logs.
The disposable account's session/recovery data is owner-only and excluded from Git and release
packaging. Copied saves isolate game writes; original seed saves and eboot hashes are checked
before/after each game run. No five-client or mixed-platform campaign claim is made.
