# A hosted server

Today a host player runs shadNet at home and friends connect over Radmin. A hosted server is one
shadNet on the internet that strangers use: every team's world on one machine, kept apart. This
page is what the `hosted-server` branch adds for that, what the load tests showed, and what is
still missing before a public launch.

## `Hosted=true`

Off by default: a host's own server behaves as before. On (`shadnet.cfg`):

| What | Where | Why |
| --- | --- | --- |
| Passwords checked on a thread pool, not the link's thread; a wrong one is not hashed again for 30 s; at most 512 checks queued | `link_server.cpp` `SignIn` | each new id cost ~3 password hashes (100k rounds) on the thread that relays every game: 30 new players froze pings for 1.7 s |
| Hellos an address (4/s, burst 20), games an address (8), new accounts an address an hour (5) | `admission.h` | bots making accounts; one PC flooding hellos |
| Each link's datagrams (1500/s) and bytes (400 KB/s); past them dropped, 5 seconds past them in a minute bans the address for 15 min | `admission.h` | a game sends ~60-200 datagrams and ~10 KB a second |
| `banned.txt`: one address or player id a line (`until=<unix s>` ends one), read every 30 s; a linked game it bans is unlinked then, not only at its next hello (only its bans: an address's rate ban holds its next hellos, it does not unlink every game behind it) | `link_server.cpp`, `admin.h` `KickBanned` | the operator's bans |
| `HostedBuild`: the build every game must say | `server_link.cpp` | otherwise the first game to link decides it |
| Lobby and party names must be the player's own id (hex, as the games say them) | `bloodborne_world.cpp` `OwnName` | anyone could act in the lobby, or say hello into a party, as another player |
| A party hello must be for the world the player's team plays | `bloodborne_world.cpp` `OnHello` | any player could join any world by its key |
| A world is its players' (`TeamWorldInfo.members`): listed to them only, forgotten only by them (each for itself); a player in more than `HostedWorldsPerPlayer` leaves its oldest at a Start (one nobody keeps is put aside) | `team_lobby.cpp` | every world was listed to everyone; anyone could forget anyone's; 16 worlds server-wide; Create/Start/Leave again and again grew the list without end |
| The HTTP ports (20443 the game's services, 31315 summons and WebAPI) answer only addresses with a signed-in link, and only as a player linked from there (the backend's `UserId` or `PlatformAccountId`, the summon's `UserId`, the bearer token's account); 403 otherwise. GETs (ss.info, status) act as nobody and stay open | `http_gate.h` | the player id came from the request: anyone reaching the ports could act as any player |
| Crash reports (`/bb/report`) off unless `HostedReports` (then 8 MB each); the stats page off unless `HostedStats` | `webapi_server.cpp`, `server.cpp` | a report costs a password hash; the stats page lists player ids |
| The lobby goes out at most every 250 ms, only to players whose own state changed, listing their team, invites and 32 others | `team_lobby.cpp` `StateFor`, `bloodborne_world.cpp` `SendLobby` | every lobby change sent every player the whole server's list: O(n^2), 216 Mbit/s at 120 players |
| The lobby's player search lists the players signed in now, and one not signed in only by its whole name; the accounts are not searched (`SearchPlayers`). At most 10 friend requests and 20 searches a player a minute; one who declined a request is not asked by that player again for 10 minutes | `bloodborne_world.cpp` `OnFriendRequest` | two letters listed every account on the server, offline ones by their player id; a request reaches the other's game, so a stranger could ask again and again |
| A trade only between members of one started team | `bloodborne_world.cpp` `TakeTrade` | trades go by member name; a stranger in a party could be offered goods or answer for another |
| Party worlds in memory: at most `HostedPartyWorlds` (2048) | `bloodborne_world.h` `Limits` | every team's world on one machine |
| The operator's commands on a UNIX-domain socket only the server's user opens (`HostedAdminSocket`, never a network port; `server/hosting/admin.sh`): list players, kick, ban/unban by id or address with an optional end (written to `banned.txt`, so restarts keep them), list worlds (members, last played, bytes on disk, measured off the link thread) and those put aside, restore one (its folder moved back at once), stats (players, worlds, link thread load, refusals); a second server started on a live socket is refused it instead of taking it | `admin.h`, `link_server.cpp` | only `banned.txt` and a restart before; a forgotten world could only come back by hand |
| Worlds nobody started for `HostedRetireDays` days (0, the default: never) are put aside once a day, as a forgotten world's folder (writer thread, logged, never deleted); a world put aside keeps its line beside it (`kept_world.pb`, with the players it had when it went: a forgotten one names its last player), so `restore` gives it back to them | `bloodborne_world.cpp` `Service::Retire` | every world ever played stays on disk and on its players' lists |
| A summon request (create, get, delete, request) must name its player (`UserId`) | `webapi_routes_bloodborne.cpp` `Refused` | one naming nobody passed the gate and could remove or claim another player's sign by its `SessionId` |
| A region (`DirectoryUrl` / `DIRECTORY_URL`, `ShardName`, `ShardPublicAddress`, `DirectoryToken`; [SCALING.md](SCALING.md)): heartbeats to the directory, new ids claimed region-wide, a game's link sent on (Redirect, with a signed region ticket) to the shard of the team code it joins, the friend it invites or accepts, the world it starts, or when this shard is full or draining | `link_server.cpp` `SetupRegion`, `region.h`, `team_lobby.cpp` | one process holds ~1000 players; friends on different shards must still find, invite and play with each other |

Two fixes are on for everyone: a TCP packet size under its header no longer loops the session for
ever (`client_session.cpp`), and a datagram from one game no longer walks every member's stream
(`Service::FlushSome`; that walk was the limit past ~600 games). The worlds' disk writes (journal
records, Compact, trades, team worlds, the world list) run on a writer thread
(`bloodborne_world.h` `WorldWriter`): one queue in order, the journals fsynced together every
100 ms or 256 records and always before a world.pb is written; SIGTERM/Ctrl+C drain it before
exit. A power cut can lose the last ~100 ms of records (never their order, never the journal).

Caps, also for everyone (one game must not grow the server's memory without end; each is far
above what a game sends, so only a broken or hostile one meets it):

| What | Cap | Past it | Where |
| --- | --- | --- | --- |
| A Coop message (the largest a game sends: its TeamWorld, every world flag on; a portrait is 48 KiB) | 1 MiB | the member's stream is dropped; its game starts over and says hello again | `bloodborne_world.h` `Limits` |
| A stream chunk | 1001 bytes (what a Send makes) | the same | `coop_stream.h` |
| Chunks held out of order | 1 MiB a member (Rpc: 256 KiB) | dropped, they come again as resends | `game_link.h` |
| Unreliable channels remembered | 16 | dropped (the games send none on these streams) | `game_link.h` |
| What the server queued to a game not acked | 8 MiB | the member is dropped | `Service::FlushSome` |
| An Rpc message (the game's bridge reads 16 KiB) | 64 KiB | the tunnel is dropped; the game connects again | `link_server.h` |
| Rpc bytes before the TCP port answers / shadNet's answers not acked | 1 MiB / 9 MiB | the connection ends | `link_server.cpp` |
| Message fields: names 64 bytes, characters 96, party key 256, portrait 128x128 RGB, join data 4 KiB, map state 1250 bytes, 4096 shown characters, HP enemies, flag events and echoes, 32768 kills, 250,000 team world flags, 64 trade items, trophies, revive hits | | the message is dropped (logged) | `Service::Oversized` |
| Party worlds in memory | 1024 (hosted: `HostedPartyWorlds`); one nobody is in for 5 min goes back to disk only (where its members stood is kept for when a new hello reads it again, for a day and 4096 worlds at most); at the cap the 4 oldest such go | a hello for a new world is refused | `Service::Collect` |

Also for everyone: a summon removal touches only its party's signs, the summon broker keeps at most
4096 signs, and an address sends at most 20 crash reports, then one a minute. A host can turn the
HTTP check on for its own server with `HttpLinkedOnly=true`: the games call the HTTP ports at the
same address as their link, so a LAN or Radmin game passes.

Also for everyone: each account's virtual address (10.x.y.z, a 24-bit hash of its id that the games
compute too) is stored with it, one account each (`database.cpp`, migration 3). A new id whose
address another account has is refused, so nobody squats a known player's address; the link's
refusal starts with `address taken:` (the launcher should make a new id). Accounts that already
shared an address keep signing in, logged at start. Home servers get it too: an id sharing an
address could never link while the other was linked anyway.

Fuzzing: `test_fuzz_decoders` (in ctest) feeds the link's datagrams, game frames, our messages
(`DecodeEnvelope` -> `Service::Take`) and the TCP sessions' packets a few thousand seeded random and
mutated inputs each. With clang, `-DSHADNET_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++` builds the same
targets for libFuzzer with ASan/UBSan (`fuzz_link`, `fuzz_game_frame`, `fuzz_world`,
`fuzz_session`; see `tests/fuzz/libfuzzer_main.cpp`; run with
`ASAN_OPTIONS=detect_container_overflow=0`, protobuf and Qt are not instrumented); crashing inputs
go in `tests/fuzz/regressions/<target>/`, which the ctest replays. It found one crash, fixed for
everyone: a TCP packet's blob size past 2 GB passed `StreamExtractor`'s bounds check and read far
past the packet, from any client before signing in. Four minutes of each target since: clean.

Settings: `Hosted`, `HostedBuild`, `HostedBanList`, `HostedLinksPerAddress`,
`HostedHellosPerSecond`, `HostedNewAccountsPerHour`, `HostedSignupWorkBits`,
`HostedSignupWorkMaxBits`, `HostedSignupsPerMinute`, `HostedDatagramsPerSecond`,
`HostedBytesPerSecond`, `HostedLobbyPlayers`, `HostedLobbyIntervalMs`, `HostedWorldsPerPlayer`,
`HostedPartyWorlds`, `HostedStats`, `HostedReports`, `HostedAdminSocket`, `HostedRetireDays`,
`HttpLinkedOnly`.

Invasions (a solo player invading a team world whose leader allows it) are off unless
`Invasions=true`, hosted or at home; the server side, its settings and what the games still need:
[INVASIONS.md](INVASIONS.md).

The games need no change for any of this but the link's handshake below (the emulator already says
its id in hex as its names; check the port does too).

A friend the lobby does not list (it lists 32 players), on every server: each team has a 6-letter
code (no 0/O, 1/I; `coop/team_code.h`), shown to its leader on the team screen, made again with
New code. Join by code puts the player straight into that team (the leader handing out the code is
its invite, the typing the accept, so the leader has nothing more to answer). Invite by name sends
the usual invite to the one signed-in player of that name as the lobby shows it (case aside;
several of that name: refused), who is then listed to the inviter. Wrong codes and names count
per player: past 5 in a minute every lookup waits (`TeamLobby::LookupTries`). Both are typed in
the window's text entry (the game's name dialog's) and need this build on both sides
(`TeamRequest` actions 12-14, `Team.code`).

Friends and open teams (from #412, Amanises), on every server. The team screen's Friends page lists
the player's friends (online first), the requests waiting for its answer and the ones it sent, and
a search by name; friends are shadNet's own friend table (`lobby_friends_db.cpp`), the one the
game's friend list reads, so a lobby friend is a friend there too and a block made there refuses a
request. The lobby keeps each player's friends in memory (`RelationsOf`) until a request changes
them or the player signs in to the lobby again. The leader picks how the team is joined
(`TeamRequest.OPEN`, `Team.join`): Invite only (the default: not listed, as before), Ask to join
(listed; the leader accepts or declines) or Open to all (listed; anyone joins at once, a started
hunt too, as by its code). Players in no team see the listed teams with room (12 at most) under
Invitations and open teams; each shard lists its own. `TeamRequest` actions 19-27, `Team.join`
12, `LobbyState` fields 7-14.

## The link's handshake and encryption (link version 4)

On for everyone, both sides (`coop/link_auth.h` and `coop/link_crypto.cpp`, shared by shadNet, the
emulator's `net_link.h`, `bbload` and the fuzz target; Monocypher vendored in `coop/monocypher/`,
CC0/BSD-2). Server and games ship together: a game of an older link version gets no answer.

| What | Why |
| --- | --- |
| A hello without a valid cookie gets only a cookie back (keyed hash of its source address and port and a 30 s bucket; nothing kept). The cookie carries the server's public key and the work bits | a spoofed source never sees its cookie: no link, account, password hash, Diffie-Hellman or refusal for it, and no reflection |
| The hello that echoes it is a Noise_NK_25519_ChaChaPoly_BLAKE2b first message: the game's ephemeral key in clear, then the id, build, secret and work nonce encrypted to the server's static key. The welcome (or a `Work` answer) is the second message | the secret and everything after it cross the internet encrypted; only the holder of the server's private key can read the hello or answer it |
| Every datagram after the welcome, both ways, is ChaCha20-Poly1305 sealed (its 9-byte header as associated data), with a 64-bit counter and the 16-byte tag last (24 bytes); one key each way from the handshake | nobody on the path reads frames, Coop messages or Rpc bytes, injects them from a player's address, or replays them |
| A datagram that does not open or a counter seen already (a window of 1024) is dropped without a word and counted | |
| A link's rates (`admission.h`) count a datagram only once it opens; hello limits only hellos with a valid cookie | a spoofed flood from a player's address cannot get that address banned |
| The server's key pair is made on first start in the data folder (`LinkKeyFile`, default `link_key.bin`, owner-only); its public key is in the log at start and from the admin `key` command | moving the file moves the server's identity; deleting it makes every game warn |
| The game pins the key the launcher gives it for that server (`KNOWN_SERVER_KEYS` in `bbport_coop.py` for a public server, else the one kept in coop.json `server_keys`). With no key kept (a friend's home host the first time) it takes the first cookie's key and the launcher keeps it from the game's log; another key later is refused and the launcher says so. The launcher learns a key only from the game's own log line and only when it keeps none for that server; a hello whose id or build has control characters is dropped, so no relayed text can forge that line | a public server's key ships with the launcher, so nobody can stand in for it; a home host is trusted on first use, as SSH does: someone on the path at that first link could stand in for it |
| Every server, hosted or not: at most 20 hellos a second from one address get past the cookie (`Admission::HelloBudget`); hosted, the guard's tighter limit | a hello that opens costs the link thread Diffie-Hellmans |
| Hosted: a hello for an id with no account must carry a proof-of-work: a nonce such that SHA-256(cookie, id, nonce) starts with `HostedSignupWorkBits` zero bits (default 20, ~0.3 s of one core on average). A hello without it gets an encrypted `Work` answer and the game does it on a thread of its own, then says hello again. Checked with one hash before any password hash or account; an existing account never pays. 2 bits more per doubling of the accounts made in the last minute over `HostedSignupsPerMinute` (60), up to `HostedSignupWorkMaxBits` (20 by default, so no rise: a launch rush of 300 real joins pushed it to the cap in a test; raise it while under attack) | a botnet stays within each address's accounts an hour but made 1181 accounts in 30 s; now each costs it a CPU's while, more than the server's password hashes |

`server/loadtest/noise_link.py` is a second implementation on Python's `cryptography` package; the
test vectors in `tests/contracts/server_link_frames.txt` come from it, and both test suites check
that `link_auth.h` makes the same bytes.

Not encrypted: the HTTP ports (20443, 31315). Hosted, they serve only linked addresses as their
own player (`HttpLinkedOnly`), but their bodies cross in clear; put a TLS reverse proxy (Caddy)
in front of them on a public server.

Cost at 300 players, the same run twice on this box (work 8 bits so the joins do not crowd it):
encrypted relay p50 / p99 0.45 / 6.0 ms, 0.17% lost, server 20% CPU; link version 3 (HMAC) 0.49 /
4.8 ms, 0.17% lost, 21% CPU. Encryption costs nothing measurable here.

Attacks beside 300 players with link version 4, hosted, work 20 bits:

| Attack | What happened | Players |
| --- | --- | --- |
| 3000 hellos/s with new ids from 500 addresses, echoing the cookie, no work | ~32,000 cookies and `Work` answers, no account, no password hashed | 300/300, relay p99 ~35 ms (the box ran the bots, 300 games' work and the server) |
| the same, the bots doing the work (one Python thread) | 48 accounts for 32 s of CPU; past 5 an address an hour, refused | 300/300 |
| 30,000 datagrams/s sealed right (the game itself) | banned within seconds | 300/300 |
| 30,000 datagrams/s that do not open, from a linked game's address | all dropped, its game not banned (its pings answered throughout) | 300/300 |

One run of the first attack (of four) left 99 of 300 games outside and the bots got no cookie; it
did not happen again in two reruns (a server of an earlier test may still have held the port).

Earlier, with link version 3 (HMAC tags, the same cookie and work):


| Attack | What happened | Players |
| --- | --- | --- |
| 3000 hellos/s with new ids from 500 addresses, echoing the cookie, no work | before the work: 622 accounts in 30 s (box loaded; 1181 earlier); with it: 0 accounts, ~15,600 `Work` answers, no password hashed, link thread <=28% | no change |
| the same, the bots doing the work (one Python thread, ~0.6 s each) | 50 accounts for 30 s of CPU (in C++ a core makes ~3 a second at 20 bits; past 120 a minute the bits rise, 16x at the cap) | no change |
| 3000 first hellos/s from 500 addresses, never echoing the cookie (all a spoofed source can do) | 57,610 cookies sent; no sign-in, no account; server 22% CPU (the link thread) | no change |
| the same echoing the cookies (a real botnet) | as before the handshake: the sign-in threads busy (server 289% CPU) | no change (link thread 20%) |
| 30,000 datagrams/s with bad tags from a signed-in game's address | all dropped, its game not banned; link thread 29% | no change |
| 30,000 datagrams/s with good tags (the game itself) | banned within seconds, as before | no change |

## Load test

`server/loadtest/`: `bbload` runs simulated games that speak the real link from their own UDP
ports and loopback addresses: hello and sign-in, ping, the team lobby (create, invite, accept,
ready, start), the party hello, vitals twice a second, kill proposals, the map every second, and
game packets to each teammate at the rates the rig measured (enemy owner ~87 a second, follower
~8). Every packet carries its send time and number, so the receiver measures the relay's delay and
losses. `run_load.py` starts a fresh shadNet, runs the players over several bbload processes,
samples the server's CPU (and its link thread's) and memory, and can run `attack.py` beside them.

```
cmake --build build --target shadnet bbload
python3 server/loadtest/run_load.py --shadnet build/shadnet --bbload build/bbload \
    --players 300 --team-size 3 --seconds 60 --cfg Hosted=true \
    [--attack "signin-flood --rate 3000 --seconds 30 --addresses 500"]
```

Results (2026-10-23; one 4-core cloud container running the server and the players together, so
the players' processes take cores from the server; teams of 3):

| Players | Server | In game | Relay p50 / p99 | Lost | Link thread | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 30 | today | 30/30 | 0.2 / 5.7 ms | <1% | ~2% | pings stall 1.7 s while players sign in |
| 60 | today | 54/60 | 0.3 / 10 ms | 2% | ~5% | joins up to 12.5 s |
| 120 | today | 50/120 | 41 / 1577 ms | 65% | 100% | lobby updates: 216 Mbit/s out |
| 120 | hosted | 120/120 | 0.3 / 1.7 ms | 0.1% | 8% | |
| 300 | hosted | 300/300 | 0.4 / 7 ms | 0.4% | 21% | 18 Mbit/s out |
| 1000 | hosted | 1002/1002 | 0.5-0.8 / 10-18 ms | 0 | 65-70% | steady state; ~50 Mbit/s out; 38 MB of memory |

With every hardening item merged (caps, writer thread, HTTP gate, address check), 300 players: relay
0.4 / 11 ms, 0.2% lost. 1002 players arriving over a minute and playing 2 minutes: all in, link
thread 66% on average with peaks at 99%, relay p50 0.8 ms but p99 ~170 ms, 6% lost; the first
prototype build gives the same numbers in that run, so it is the box's limit, not a regression.
One process on one core tops out around 1000 players: plan for fewer per process.

At 1000 the join rush (1000 new accounts within 10 s) takes up to 30-35 s; players arriving over
minutes would not see it. "Lost" counts gaps in each sender's numbers on one map; packets the
server keeps from a player on another map (by design) are not losses. Without the FlushSome fix 1000 players saturated the
link thread (relay p99 9 s).

Attacks beside 300 players (hosted):

| Attack | What happened | Players |
| --- | --- | --- |
| 3000 hellos/s with new ids from one address, 30 s | 5 accounts made, the rest dropped | no change |
| the same from 500 addresses (a botnet) | 1181 accounts made; the sign-in threads busy (server 268% CPU) | no change (link thread 19%) |
| one signed-in game sending 30,000 datagrams/s | banned within ~5 s | no change |

New accounts' work at 300 players joining within 6 s: the extra round trip costs nothing (welcome
p50 2.0 s with 8-bit work and without). At 20 bits one core solves in 316 ms on average (p50 216,
p90 680 ms; a rare one over 2 s), once per account. On this box the 300 simulated games share its 4
cores, so their joins wait for each other (welcome p50 31 s); with the default rate rise a join
rush of hundreds a minute asks up to 24 bits (~5 s each) until it calms.

## How far one machine goes

The link thread (relay, worlds, lobby) is the limit: ~65% of one core at 1000 players here. A
dedicated 4-8 core machine should hold ~1000-1500 players in one process (a guess past what was
measured). Past that, a region runs several shadNet processes, the shards (each its own port,
its own share of the worlds), with a small directory on Redis beside them: built, see
[SCALING.md](SCALING.md) (design, settings, `run_shards.py`, what is left). Players never leave
and rejoin: a shard sends a game's link on to another (link message Redirect) when a friend's team
or world is there. Each world's log keeps one writer, its shard, so no distributed database is
needed for "sync".

Storage: SQLite (accounts) and the world folders (journal + snapshots) are right for one machine.
Postgres helps only with several processes or machines; moving to it is ~3-4 weeks (thread-per-
session connections, SQLite-only SQL). Backups: `server/hosting/backup.sh` (journal before
world.pb, `sqlite3 .backup`, restic to S3/B2/Storage Box), with a restore drill in
`server/hosting/README.md`.

## Deploying it

`docs/DEPLOY.md`: the production image (`server/hosting/Dockerfile`), the Helm chart for N shards a
region (`deploy/helm/yharnam`), Terraform for a region on Hetzner Cloud (`deploy/terraform`), the
link key's and backups' handling, upgrades (a maintenance window), costs, and what was tested.

## Before a public launch

Server only: nothing left; each item is a row in the tables above.

Game and launcher:
- Put the public server's key in `KNOWN_SERVER_KEYS` (`bbport_coop.py`) before release: without it
  the first link to it is trusted on first use.
- ~~Encryption.~~ Done for the link (version 4, Noise NK); the HTTP ports want a TLS proxy (the
  chart has one, but the games speak plain HTTP and the HTTP check reads the connection's address:
  docs/DEPLOY.md, "TLS for the HTTP ports"). Not
  yet run in a real game: the emulator side compiles only on the PC (this cloud box can't fetch
  its dependencies).
- ~~A fresh id when the server refuses one (virtual address collision).~~ Done: the launcher makes
  one for that server only (coop.json `server_ids`, keyed by the address as typed) and starts again.
