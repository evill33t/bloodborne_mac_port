# Yharnam Together v1.5.0 integration

Reference: [Amanises-Yharnam-Together/BBSC-Yharnam-Together v1.5.0](https://github.com/Amanises-Yharnam-Together/BBSC-Yharnam-Together/releases/tag/v1.5.0),
`0bb87c1b80ca2dfc01d75931cc3cb55873c057e2`, published 12 November 2026.
Previous source: v1.4.0, `efa0ad265d840d9195e55db2e3216f46f7daedd0`.
Original co-op/server authors include Kaniel / lKaniel, Amanises, Vitalii Shevchuk,
Yharnam Together and shadNet/shadPS4 contributors. Native Metal overlay/fixture architecture
retains Mark Hall's foundation. Original headers, dependency notices and source hashes remain.

## Mac integration

The pinned client/server changes include the guest's single reload into full-player state,
per-character world isolation and save rollback fixes, kept leader/team settings, custom enemy
strength, optional friendly fire, and upstream progress/enemy/chalice/join fixes. The server
also carries the new admission/flood handling. Our native runtime and Metal architecture are
preserved; Windows engine/Vulkan/AMD/overlay-specific work is not copied into the Metal port.
The existing Travel Together lock-order and durable invasion reward/save/receipt fixes remain.

The native team settings add friendly fire and send the new ninth rule field. UI record
version 8 carries updated lobby data and explicit sign-in refusal diagnostics. Team rules and
joining/invasion/death choices are kept under the selected per-server/account user folder.
The launcher no longer offers party rules on either host and strips inherited legacy rule/
strength overrides. Legacy INI values are retained for older builds. Fixed co-op timing remains
60 FPS by default; selecting retail 30 FPS is retained. Unlocked co-op is not enabled here.

The new shared title predicate covers all nine retail title IDs. Stale regional metadata is
accepted only when our native adapter independently verifies the 1.09 executable; all hook
sites are still checked before writes. The Metal overlay presents the server's refusal reason
as text, without treating it as a format string. Public authentication remains native verified
HTTPS with Keychain sessions and per-account saves; its shipped transport pin is unchanged.

## Validation

The production optimized build uses `BB_BUILD_TYPE=Release BB_LTO=ON BB_PRODUCTION=ON`.
The canonical runtime/native GPU/Metal suite and native launcher model tests passed. Thirteen
launch tests cover public identity/session isolation, no stale launcher-rule overrides and
30/60 FPS handling. The bounded travel test also exercises persisted eight/nine-field rule
round trips, friendly fire and malformed input rejection. Existing keyboard/controller Travel
Together tests and Metal presentation fixtures passed with the updated record layout and
server-refusal text. Backend Python tests passed (34 tests; nine optional compiled parity
cases skipped). Client/server import manifests retain original and adapted SHA-256 hashes.

Live and deployment evidence for this update is retained locally under `out/bbsc-v150/`.
Test games use copies of the previous release's validation saves and check original save,
executable and event-file hashes. Public test credentials are owner-only and excluded from
Git, packaging and public validation reports. No mixed Windows/macOS, five-player, full
campaign/DLC/chalice or long-session compatibility claim is made.

Native HTTPS recovery/sign-in reused our disposable public test account without storing its
password. The production Mac game advertised its actual v1.5.0 source/content fingerprint,
received the pinned public welcome, logged in and received authenticated HTTP 200 responses.
The public fingerprint warning remains visible. The original seed saves, executable and
checked event files matched after this bounded test. This is connection proof, not mixed-
platform or campaign gameplay proof.

## opc deployment

All 20 focused server CTest suites passed in the ARM64 image, including admission, password
accounts, lobby/duels and durable world/reward tests. A network-isolated disposable container
then accepted two `dev` and two `1.5.0` simulated clients over actual UDP, with four playing
and zero refusals. Qt's normalized whitespace in the existing release list is accepted by
the preflight without changing the permitted old labels before the update.

Only `docker-bloodborne-server-1` was replaced, following a stopped-data backup. All 38
existing worlds, private automatic accounts (`SignIn=open`), the pinned transport key and
static configuration were retained; unrelated container IDs were unchanged. The service was
healthy afterward. It checks protocol 9 and permits `dev,1.5.0`; the image reports its actual
source release via `BB_RELEASE_VERSION=1.5.0`.

Image: `bloodborne-server:v1.5.0-mac-arm64`,
`sha256:c362e52628894fde83ee45780ede93f40e80f92c3b68c1c7d2c8686694ddbbec`.
The previous image and private closed-data backup remain on opc under
`/docker/bloodborne-server/rollbacks/v1.5.0-20261112/`. Do not restore that snapshot over later
player progress. Deployment and isolated-client reports remain in the local validation folder.
