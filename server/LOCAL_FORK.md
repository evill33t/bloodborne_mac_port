# Server source in the Mac fork

This branch contains the complete shadNet server component and supporting backend template
from [Kaniel/lKaniel's bloodborne-server](https://github.com/lKaniel/bloodborne-server),
including the changes running on `bloodborne.phobos.services`. The backend runs as a separate
Linux service. The Mac app does not launch or package the server.

## Source and changes

Original upstream base: `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`.
Imported deployed source: `9e60cec70e589a199cf86771de4844fe556562d4`.
The imported baseline is retained by its original hashes. Local hosting and server
adaptations are listed by `adapted_sha256` in [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json).

The local server also supports `CompatibleClientVersions` in `shadnet.cfg`. To admit the
Mac `dev` label and Windows `1.3.0` together, set:

```ini
CheckServerBuild=true
CompatibleClientVersions=dev,1.3.0
```

Restart the server after changing this setting. Both release labels must be listed to allow
their difference against the server or linked players. Co-op protocol mismatches remain
refused, authentication is unchanged, and game-file/settings differences still warn players.
An absent or empty list retains upstream's exact-release checks. A legacy, non-fingerprint
`HostedBuild` remains an exact requirement. This setting allows joining the server; it does
not establish that two installations' game assets, patches or gameplay behavior match.

| Original local commit | Already included behavior |
| --- | --- |
| `4201408d919f9787af43a075068947a44e95a231` | Durable authenticated victory-reward mailboxes, solo winners and capability negotiation |
| `fceb97debb950b94df220c9963ccced4ad31fa2a` | Bounded supported-level Echoes policy |
| `ebbef91358246c860bee80f03393fa56de2810a6` | Frozen covenant metadata and original Corruption Blood Dreg rewards |
| `9e60cec70e589a199cf86771de4844fe556562d4` | Commit-before-send and durable ACK ordering, failed-write recovery and trades-only mailbox restore |

[upstream-changes/](upstream-changes/) preserves the four original format-patches, commit
messages and attribution trailers. They are historical records; their changes are already
applied to this source. The patches use paths in the original upstream repository.
[The reward/deployment record](../docs/SEAMLESS_INVASION_REWARDS.md) describes policy,
crash guarantees, native source evidence, deployed image identity and rollback restrictions.

The C++ source, tests, shared protocol, licenses, server tools, original hosting files and
supporting `backend/` sources are included. The original pinned protobuf dependency is a
submodule; unrelated Windows/emulator sources are not required to build this component.

## Build on Linux

From the repository root:

```sh
git submodule update --init server/externals/protobuf
python3 server/scripts/verify_import.py
docker build --build-arg BUILD_JOBS=2 -f server/hosting/Dockerfile -t bloodborne-server:local .
```

The Dockerfile retains Kaniel's multi-stage build and our tested ARM64 selection for
micromamba; its default conda toolchain supports ARM64 and amd64. It builds Qt >= 6.8 HTTP
services and runs the ten focused CTest suites during the build. The optional upstream
`QT_FROM=aqt` path was not validated here. Build dependencies require network access.
The compiled revision defaults to the imported server source revision, rather than the
unrelated Mac repository commit. New server changes should set `SOURCE_REVISION` explicitly.

For a non-Docker build, initialize protobuf, install Qt >= 6.8 with Core, Network, Sql,
Concurrent and HttpServer, then use `cmake -S server -B server/build` and
`cmake --build server/build --target shadnet`. The optional original bbport revision script
is absent in this repository, so that path uses the original CMake fallback revision;
the Dockerfile supplies the pinned revision explicitly.

## Standalone Compose example

```sh
export BLOODBORNE_DATA_DIR=/absolute/path/to/private/server-data
docker compose -f server/hosting/docker-compose.phobos.yml config
docker compose -f server/hosting/docker-compose.phobos.yml up -d --build
```

This is a standalone Linux/host-network example. It is not a replacement for opc's existing
multi-service `/docker/docker-compose.yml`. Bind the existing data directory when migrating:
accounts, worlds, payout ledgers and `link_key.bin` stay together. Starting with an empty data
directory creates a new server identity. Runtime config, keys, passwords, databases and
player data are not included in this Git copy. Existing `shadnet.cfg` is preserved; set
`Invasions=true` there when required. Fresh installations now enable seamless co-op and
invasions in the generated defaults.

Public ingress is UDP 31314 and TCP 31315/20443. TCP 31313 remains local via
`TcpLocalOnly=true`; stats/admin services remain private. This source import does not change
the deployed service or its firewall. Preserve the existing pinned identity and data when
updating; see the linked deployment record before rolling back durable payout code.

## Original sources and authors

- Kaniel / lKaniel, Vitalii Shevchuk and the shadNet/shadPS4 contributors: original server,
  co-op protocol, invasion, trade, world writer, tests and hosted Docker implementation.
- FromSoftware: original Bloodborne 1.09 native reward behavior; no game assets are included.
- Soulsmods/Paramdex contributors and Grimrukh/Soulstruct contributors: read-only native
  covenant evidence, with detailed attribution in the retained original commit messages.
- Qt Company/Qt contributors and Google/Protocol Buffers contributors: runtime/build dependencies.
- Ronny Stiftel: local payout extensions, ARM64 hosting integration and this import.

Original copyright notices and [GPL-2.0-or-later license](LICENSE) are retained, together with
[REUSE.toml](REUSE.toml) and [LICENSES/](LICENSES/). The protobuf submodule retains its own
license and authors. The source manifest records original and adapted file hashes plus the
exact dependency pin, allowing verification against the server snapshot independently of
this Mac branch's commit history.

## Import validation

On 2026-10-27 the imported source built in a separate Docker image on opc's ARM64 host.
All ten focused suites passed: server_link, packet_routes, build_fingerprint, rpc_tunnel,
bloodborne_world, world_writer, admission, http_gate, team_lobby and bloodborne_trade.
The image then passed startup health with networking disabled and a temporary tmpfs data
folder, with seamless co-op, invasions and local-only TCP enabled. The deployed container ID,
start time and healthy status were unchanged. Compose syntax, 221 source-file hashes, four
retained patches and the protobuf pin also verified. Detailed local evidence:
`out/server-import-build.log`, `out/server-import-startup-smoke.json` and
`out/server-import-validation.json`. No additional in-game run was needed for this source
import; the C++ source/protocol/tests are identical to the already deployed/tested snapshot.
