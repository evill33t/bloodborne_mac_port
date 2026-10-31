# Seamless Co-op v1.3.1 integration

The Mac client and separately hosted server are updated from Kaniel/lKaniel's
[bloodborne-server v1.3.1](https://github.com/lKaniel/bloodborne-server/releases/tag/v1.3.1),
source `5d2305ee611ef1e03be39ff925f0da75c9a70393`. This is a three-way source update from
`8d5e54f`, preserving the Mac platform adapters and the local authenticated invasion payouts,
commit-before-send ordering, durable ACKs and crash-atomic native save receipts.

## Changes

- Client gameplay: boss ownership/idle fixes, guest synchronization, shared NPC drops and
  chalice removal, supplies after death/travel, host handover, Rom entry, playthrough/world
  boundaries and endings, kill accounting, event/downed-state and banners/shop fixes.
- Boss tempo: optional Vanilla/Aggressive/Relentless launcher choice, passed through the
  existing host-rule protocol. Vanilla remains the default; guests cannot overwrite host rules.
- Native Metal interface: friends, friend requests and account search, filtered online hunters,
  open teams and join requests, join modes and lamp reset with confirmation. The new silver
  settings artwork opens online display preferences. The fixed-width cross-process snapshot
  is version 5 because the embedded lobby ABI expanded.
- Server: matching lobby/world behavior and persistent SQLite friends, retaining local reward
  capabilities and durable mailbox semantics. `CheckServerBuild=true` and
  `CompatibleClientVersions=dev,1.3.1` admit the Mac dev label and Windows 1.3.1 label; protocol
  mismatches remain refused. Old Windows 1.3.0 is no longer in this deployment's allowlist.
- Renderer: the component-wise mip/layer containment correction (shadPS4 #5524), and packet
  lengths captured before guest signaling can reuse command buffers (#5541). The existing
  native render/query guards remain. The reference's discrete-VRAM collector hash model does
  not apply to this Metal cache; its Windows/DLSS, installer and certificate-store changes
  are not imported into the Mac backend.

The two source manifests record v1.3.1 originals and hashes of retained local adaptations.
Server reward format-patches remain historical records of the local extension base.
No game assets, server keys, account databases or saves are included. Per-server identities
and isolated co-op save directories retain their existing locations.

## Verification and deployment

Use `BB_BUILD_TYPE=Release BB_LTO=ON bash build_macos.sh --test` and
`python3 server/scripts/verify_import.py`. The server Docker build runs eleven focused
suites including the new SQLite friendship persistence test, existing reward durability tests
and version/protocol admission checks. A throwaway container with private tmpfs data exercises
startup and simulated dev/1.3.1 clients over UDP before deployment.

The deployed service retains `/docker/data/bloodborne-server`, its pinned `link_key.bin`,
worlds, accounts and payout ledgers. The rollout stops only the Bloodborne service, backs up
its closed persistent data/configuration, changes only that service's image/build source and
compatible release labels, then verifies health and unrelated-container identities. The
previous image and stopped-data snapshot remain available for recovery. Restore data only
with an explicit recovery plan: rolling player data back can undo legitimate post-update play.

Final release validation records the signed archive hash, source revision, Apple notarization,
Gatekeeper checks and exact gameplay coverage. Simulated clients establish transport and
admission behavior; they do not establish mixed Windows/Mac gameplay or all boss scenarios.

## Authors

Kaniel/lKaniel, Vitalii Shevchuk, shadNet/shadPS4 and their contributors supply the original
co-op and server. The upstream source credits Amanises for friends/open teams and Provi for
lamp reset. Mark Hall/Wooloomooloo2 and contributors supply the native Mac/Metal foundation.
Ronny Stiftel's fork retains its local platform and durable reward extensions. Original GPL
and third-party notices remain in sources and release packaging.
