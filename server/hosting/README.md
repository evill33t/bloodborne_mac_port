# Hosting shadNet on a public server (prototype)

Ops files for running ONE public shadNet for roughly 100 to 1000 players. They are a prototype:
each file says what has and has not been tested. They run shadNet with `Hosted=true`
(docs/HOSTED_SERVER.md: rate limits, bans, team-only worlds). shadNet still keeps its data in
SQLite files and protobuf world folders.

| File | What it is | Tested? |
| --- | --- | --- |
| `Dockerfile` | The one image (this compose file, the Helm chart in `deploy/helm/yharnam`, CI). Multi-stage: Qt 6.8.4 from conda-forge plus QtHttpServer 6.8.3 from source (or aqtinstall with `--build-arg QT_FROM=aqt`), protobuf from the submodule; the runtime is Ubuntu 24.04 with only shadnet and the libraries it loads, user 10001, a healthcheck (`healthcheck.sh`). Context: the repository root | Built and run 2026-10-23 (docs/DEPLOY.md): starts, makes `link_key.bin`, healthy, bbload games link and play. The aqt path was not built (download.qt.io blocked there). **Ubuntu 24.04's apt Qt (6.4.2) does NOT build shadNet** (`src/webapi_server.cpp` needs Qt 6.8's QHttpServer API). |
| `entrypoint.sh` | Seeds `/data` (cfg files, public-host `shadnet.cfg`, or the chart's config with `SHADNET_CONFIG`), picks a shard's name, address and key by its ordinal under Kubernetes, then execs shadNet | Yes (docker run and kind) |
| `docker-compose.yml` | shadnet (host network) + backup sidecar; `postgres` and `redis` profiles | `docker compose config` only |
| `Dockerfile.backup` | Ubuntu 24.04 with sqlite3, rsync, restic, rclone, pg_dump; runs backup.sh hourly (the chart runs it as a CronJob) | Built; backup and restore run in the kind test (restic to a local repository) |
| `backup.sh` / `restore.sh` | Online snapshot → restic (S3/B2/SFTP) or rclone; restore with checks | Tested locally on a fake data folder with `BACKUP_TOOL=none` / `restore.sh dir`. restic/rclone paths not run. |
| `shadnet.service` | systemd unit (Restart=always, limits, hardening) + backup timer in comments | `systemd-analyze verify` only |
| `schema.sql` | Draft Postgres schema: accounts, sessions, bans, worlds, membership, snapshots, audit | Loads on PostgreSQL 16 (checked). Nothing uses it yet. |
| `env.example` | Settings the compose file reads | n/a |
| `admin.sh` | The operator's commands (below) to the running server, in its container or on the host | `sh -n`; the commands themselves are covered by `test_admin` |

## Data layout (`SHADNET_HOME` = `/data` or `/var/lib/shadnet`)

```
shadnet.cfg  worlds.cfg  scoreboards.cfg  domains_banlist.txt
db/shadnet.db            accounts, friends, scores        (SQLite, WAL)
db/backend.db            game backend content store       (SQLite, WAL; BackendStore=)
score_data/*.sdt         score attachments
bloodborne_worlds/
  worlds.pb              the world list
  <world>/world.pb       snapshot; world.pb.1-3 older snapshots
  <world>/journal.pb     append-only, fsynced records since the snapshot
  <world>/trades.pb  team_world.pb  backups/world-YYYYMMDD.pb (7 days)
  suspects.jsonl         cheat watch: every finding, one JSON line each (admin.sh suspects)
  forgotten/             worlds taken off the list (never deleted): forgotten or retired;
                         each keeps its line in kept_world.pb (admin.sh restore)
admin.sock               the operator's commands (HostedAdminSocket; owner-only)
banned.txt               bans: the operator's lines and admin.sh ban's
restore-aside/           what restore.sh moved out of the way
```

## Quick start (Docker)

```bash
git submodule update --init server/externals/protobuf
cd server/hosting
cp env.example .env              # fill in RESTIC_REPOSITORY and the keys
mkdir -p secrets && openssl rand -base64 32 > secrets/restic-password   # keep a copy OFF the server
docker compose up -d --build     # builds from the repository root (Dockerfile.dockerignore)
docker compose logs -f shadnet
```

Firewall: allow inbound 31314/udp (the games' link), 31315/tcp and 20443/tcp only. 31313 stays
on 127.0.0.1 (`TcpLocalOnly=true`: the games' calls come through the link). Keep 31320 (stats),
5432 and 6379 closed. The link is encrypted (link version 4); the HTTP ports (20443, 31315)
are plain HTTP, open only to linked games' addresses when hosted (docs/HOSTED_SERVER.md). For
Kubernetes and several regions, see docs/DEPLOY.md.

## Operator commands

A hosted server listens for the operator's commands on a UNIX-domain socket in its data folder
(`HostedAdminSocket`, default `admin.sock`; empty turns it off). Only the server's own user (and
root) can open it; it is never a network port, so there is no token to leak. `admin.sh` sends one
command and prints the JSON answer (`docker compose exec shadnet /opt/shadnet/shadnet --admin ...`
underneath; `SHADNET_BIN`/`SHADNET_HOME` for a systemd install).

| Command | What it does |
| --- | --- |
| `players` | Linked games: id, address, build, team, world, map, ms since last heard |
| `kick <id\|address>` | Unlinks it now. Its game links again by itself (unless banned) |
| `ban <id\|address> [90s\|30m\|12h\|7d] [note]` | Written to `banned.txt` (`<who> until=<unix s> # note`), in force at once, and kicks it. No duration: for ever. Survives restarts; an ended ban is ignored and dropped at the next write |
| `unban <id\|address>` | Its lines out of `banned.txt` (the operator's hand-written ones too; comments kept) |
| `bans` | `banned.txt`'s entries, with their ends |
| `worlds` | Listed worlds (id, name, members, last played, created, folder, bytes on disk, games in it) and the folders in `forgotten/` |
| `restore <folder>` | A folder from `forgotten/` back on the list, to its players, as played now. Worlds put aside before this build come back with a new id and no players (hosted: nobody sees them) |
| `retire` | Retirement (below) now |
| `stats` | Players, worlds listed and in memory, the link thread's busy share (last 10 s), hellos refused, datagrams dropped, accounts refused, strike bans |
| `suspects [id\|name]` | Cheat watch (docs/CHEAT_WATCH.md): the players seen with cheats, the save editor or an edited save since the server started, the surest first, each finding with its level (`certain`, `likely`, `info`), text, character and how many runs. Reports only: nobody is banned for it. `suspects.jsonl` in the worlds' folder keeps every finding across restarts |

```bash
./admin.sh players
./admin.sh ban 203.0.113.7 7d flooding hellos
./admin.sh worlds | jq '.aside[].folder'
./admin.sh restore 6f2a...c1-20270115-031500
```

`HostedRetireDays=N` (default 0: off) puts aside, once a day (the first a minute after the start),
every world nobody started for N days that no team has picked and no game is in: the same move as
a forgotten world's (on the worlds' writer thread, logged, never deleted). `restore` brings one
back.

The team rules (enemy strength, Rot, travel together, boss tempo, friendly fire) are the team's: its leader sets
them on the game's team screen. A team starts with `TeamDefaultRules` (any server), `;`-separated:
`enemies=auto|fixed`, `hp=`, `damage=`, `poise=` (1 to 10; absent: the standard 3, 1.5, 3), `rot`,
`travel`, `vote`, `tempo=0|1|2`, `friendlyfire`. Empty (the default) is Auto enemies and nothing else; a setting it
does not understand is logged and the default kept. Example: `TeamDefaultRules=enemies=auto; hp=2.5`.
A leader whose launcher has its own rules (a local server's Co-op steps) starts with those instead.

## Backups

`backup.sh` runs hourly in the sidecar (or from the systemd timer):

1. `sqlite3 .backup` of every `db/*.db`. This is an online, consistent copy. `PRAGMA integrity_check` must pass.
2. World folders are copied **journal first, then `world.pb`**. The loader skips journal records
   below `world.journal_from`, and `world.pb` is only ever replaced whole (QSaveFile). So a
   world copied after its journal can never be older than the journal's start. Copying in the
   other order could pair an old world with a journal that a compaction emptied in between.
3. Config, score files, the ban list, and `pg_dump -Fc` once Postgres holds anything.
4. A SHA-256 manifest, then `restic backup` and `forget --prune` (48 hourly, 14 daily, 8 weekly,
   12 monthly). A 1% read-back check runs daily and a 10% check on Sundays. `rclone` mode
   uploads dated `.tar.zst` files instead and deletes those older than 30 days.

Targets: Backblaze B2 or any S3 (`s3:https://...`), or a Hetzner Storage Box (`sftp:`; the
sidecar then needs an ssh key and config). Expect only a few GB even at 1000 worlds. That
is a guess, based on world.pb being tens to hundreds of KB.

Recovery point: up to 1 hour if the whole machine is lost. On the machine itself the journal is
fsynced per record and keeps `world.pb.1-3` plus 7 dailies, so a bad write loses much less.

## Restore drill (do it before launch, then monthly)

1. On a spare VM (not production), install restic and sqlite3 (or run the backup image).
2. `restic snapshots --host shadnet`, then pick the newest snapshot and one from a week ago.
3. `DATA_DIR=/tmp/drill restore.sh restic <id>`. This checks the manifest and runs
   `integrity_check` on each db.
4. Start shadNet on the restored folder (`SHADNET_HOME=/tmp/drill ./shadnet`). In the log, check
   that every world loads and that no `world store ...: does not read` lines appear. Count
   the accounts with `sqlite3 /tmp/drill/db/shadnet.db 'select count(*) from account'`.
5. Sign in with a test account and join a restored world.
6. Write down how long it took and the newest record time. If it took more than about 30 minutes, fix the procedure.

To restore one world on production: stop shadNet, then run
`RESTORE_ONLY=world:<folder> restore.sh restic latest`, then start shadNet. The live folder goes to
`restore-aside/` (not next to the others, because shadNet takes any folder in
`bloodborne_worlds/` as a world).

## Scaling notes

* **Socket buffers.** Every game's datagrams come to one UDP socket, and the kernel drops what
  overflows it while the link thread is busy. The load tests on a 4-vCPU box stayed comfortable up
  to 300-400 players only with big buffers, so on a public server set
  `net.core.rmem_max=33554432` and `net.core.wmem_max=33554432` (`/etc/sysctl.d/90-shadnet.conf`,
  then `sysctl --system`). shadNet asks for 16 MiB and warns at its start when the system gave
  less; the sysctl holds on the host, not in a container.
* **Players on a box.** `ShardCapacity` (350 by default, from those tests) is where a region sends
  new players on to the next shard; 1000 ran fine on three shards. The cap is soft: with no shard
  under 90% a hello stays where it came.
* One process, one Qt event loop for the UDP link/relay and the worlds. The journal `fsync`
  runs on that loop, so put `/data` on local NVMe, not a network volume.
* Each TCP client gets its own thread and its own SQLite connection (`server.cpp`,
  `client_session.cpp`). That is why `pids_limit`/`TasksMax` are set to 4096.
* Postgres and Redis are not needed for one machine. See docs/HOSTED_SERVER.md.
