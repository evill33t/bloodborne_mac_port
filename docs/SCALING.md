# Past one process: shards

One shadNet process holds about 1000 players: its one link thread (relay, worlds, lobby) is
65-70% busy there (HOSTED_SERVER.md, "How far one machine goes"). A region runs several
processes, the shards, and a small directory beside them. Players never leave and rejoin: when
they must be on another shard, their game's link moves there by itself, as it does after a server
restart, and the team screen goes on.

```
           launcher ── HTTPS GET /v1/shards, /v1/where ──┐
                                                         ▼
 game ──UDP link──▶ shard eu-1 ──POST /v1/heartbeat──▶ directory (stateless, N replicas)
   │  Redirect                shard eu-2 ──────────────▶     │
   └────────────────▶ shard eu-3 ──────────────────────▶   Redis (TTLs, leases, claims, inboxes)
```

## Shards

A shard is today's hosted shadNet with four more settings (below). Each has its own UDP port (and
in production its own address: the HTTP ports follow the link, see "The game"), its own data
folder (SQLite accounts, world folders) and the same link key as the others (`link_key.bin`
copied to every shard: the region's identity; see "Trust").

Each world has one writer, the shard whose folder holds it. Worlds do not move while their shard
runs: a world moves only when its shard is drained (below). So no shared volume and no object
storage on the hot path; a shard's disk is as before, and so are its backups.

## The directory (`server/directory/`)

Python (stdlib `http.server`, `redis-py`), stateless: every replica reads and writes one Redis.
Port 8080. Shards authenticate with `Authorization: Bearer $DIRECTORY_TOKEN`; the two GETs a
launcher uses are open.

| Call | What |
| --- | --- |
| `POST /v1/heartbeat` (shard, every 2 s) | its name, public UDP address, link key, capacity, whether it drains; its signed-in players (online id, lobby member, name), its teams' codes, the worlds it lists, the players that started a world there. Answers the region: every healthy shard and its load, the other shards' players and codes, its worlds whose lease another shard holds, and its inbox (invites from other shards) |
| `GET /v1/shards` | healthy shards: name, address, key, players, capacity, draining |
| `GET /v1/where?player=ID` | the shard the player is on now, else the one it last played a world on; with its address and key |
| `GET /v1/world?id=W` | the shard holding world W's lease |
| `POST /v1/account` (shard) | claims a new online id for the shard making its account (SET NX, no expiry); answers the holder |
| `POST /v1/invite` (shard) | an invite for a player on another shard, into that shard's inbox |

Redis keys: `shard:<name>` (JSON, 10 s TTL), `online:<id>` and `code:<code>` (10 s),
`players:<shard>` (10 s), `home:<id>` (90 days), `lease:<world>` (30 s), `account:<id>` (none),
`inbox:<shard>` (a list, 60 s).

A world's lease: the shard that lists a world claims `lease:<world>` with `SET NX EX 30` in every
heartbeat and renews it only while it still holds it (a compare-and-expire Lua script, so a lease
that just expired and was taken by another shard is never extended by the old one). The directory
answers each shard the worlds it lists but does not hold; a shard never starts those (rule (b)).
A shard that dies keeps its worlds' leases 30 s; nobody else lists them (their files are on its
disk), so nobody takes them until an operator moves them.

Several replicas need nothing more: every write is a single Redis command or script. Redis is the
one stateful piece; its loss costs at most 10 s of presence (the heartbeats write it again), the
account claims and the homes (persist it: AOF).

## Accounts: claims and region tickets

Each shard keeps its SQLite accounts. Two rules make an id one player's across the region:

- **A new id is claimed.** A shard about to make an account (an id it does not know) first claims
  it in the directory (`account:<id>`, SET NX). An id another shard claimed is not made here: the
  hello is answered with a Redirect to that shard (no ticket), which checks the secret. The
  directory unreachable: no new accounts (known ones still sign in).
- **A move carries a region ticket.** A shard that sends a signed-in player elsewhere gives it a
  ticket: its online id, SHA-256 of its secret, the target shard, an expiry (2 min) and the
  requests to run there (join this team code), HMAC-SHA256 under the region secret every shard
  holds. The game echoes it, opaque, in its hellos to the new shard. The target takes a ticket for
  itself whose id and secret hash match the hello, makes its own copy of the account (the same
  secret, so the game's TCP sign-in through the tunnel works there too, and later hellos there
  need no ticket) and runs the requests once the player's lobby hello arrives.

Trade-offs. Tickets need no new service on the sign-in path and nothing synchronous between
shards; the cost is copies: a secret the player never changes (the launcher makes it once) is
stored on every shard it played on, and a ban must reach every shard's `banned.txt` (the deploy
mounts one file). A shared accounts store (the Postgres schema in `server/hosting/schema.sql`) is
the long-term answer: one row per account, bans and friends in one place, no claims. It is ~3-4
weeks (HOSTED_SERVER.md); the tickets stay useful then for carrying the requests.

The region secret is `REGION_SECRET`, or derived from `DIRECTORY_TOKEN` when unset (every shard
and the directory hold that one anyway). With it, the directory could mint tickets too: set a
separate `REGION_SECRET` if the directory runs with less trust than the shards.

## Redirect (link message kind 11)

Server to game: "continue on shard X": its IPv4 address and port, its link key, the ticket, a
reason. Sealed like everything else: either as the answer to a hello (Noise's second message, in
place of a welcome: rules (c) and "id claimed elsewhere"), or on the link's session after a
welcome (rules (a) and (b)). The game switches the link's endpoint, pins the key, keeps the
ticket for its hellos and says hello there: cookie, Noise handshake, the same id and secret. Its
Coop and Rpc streams see a server that restarted (a new instance), which they already handle: the
game says its lobby hello again, its shadNet client signs in again through the tunnel. No game
restart, no player action.

The old shard resends the Redirect every 500 ms for 3 s (a lost datagram), then forgets the link.

## When a shard redirects

| | Case | What |
| --- | --- | --- |
| (a) | Join by code, the code not on this shard | the directory's codes say the shard; the joiner goes there with a ticket asking `JOIN_CODE code`: it arrives in the team |
| (a) | Invite by name, the name not on this shard | the directory's players say the shard; the invite goes to its inbox and the invitee sees it as usual (team `@shard/code`). Its Accept sends it to the leader's shard with `JOIN_CODE`. The joiner always moves to the leader, before the team forms |
| (b) | Start of a world whose lease another shard holds | every member goes there: the leader with `WORLD w` and `CREATE` keeping the team's code, the others with `JOIN_CODE`; they press Ready and Start again there |
| (c) | A hello at a full shard (players >= `ShardCapacity`) | the least loaded healthy shard under 90% of its capacity, if any; with a ticket. A hello with a ticket for this shard is never sent on |
| | A draining shard (`ShardDraining=true`) | a hello as at a full one, and every player not in a started team at once |

The launcher picks the shard first (below), so (c) is for launchers that picked stale and for
rushes. The directory's view is 2-4 s old: a code made a second ago is not found on another shard
yet (the player tries again; a wrong code counts as one of its five a minute).

## The launcher

Host's IP address may be a region's directory, `https://eu.example.com`. At PLAY the launcher asks
`/v1/where?player=<its id>` (a returning player goes back to where its worlds are), else
`/v1/shards` and takes the least loaded one not draining, and starts the game with that shard's
address and port. One id for the region (coop.json `server_ids` keyed by the URL).

## Trust

Every shard of a region holds the same link key, the region's, and it ships in the launcher
(`KNOWN_SERVER_KEYS`, keyed by the region's URL), so whatever address the directory names, only a
shard of the region can answer the game. A directory (or someone on its path) can send a game to
the wrong address, not to a server that reads it: the worst it can do is deny service, as DNS
could. A Redirect's key is trusted because it arrives sealed on a link to a key the game trusts.

Shards with keys of their own work for redirects (the key travels in them); for the launcher's
first pick the directory's key would then have to be trusted over HTTPS alone. To do that
properly the directory would sign `/v1/shards` with an Ed25519 region key the launcher pins (not
built: one key for the region is simpler and as strong).

## Draining and moving a world

To take a shard out: `ShardDraining=true` (restart, or the admin socket later): it leaves the
directory's picks, sends idle players to other shards at once and the others when their team
ends. Then, with the shard stopped (its leases lapse in 30 s): copy each world folder to the shard
that takes it, put it in `forgotten/` there and `restore` it (admin): it lists it again and claims
its lease. Not automated yet.

## Settings

| shadnet.cfg | Environment (wins) | |
| --- | --- | --- |
| `DirectoryUrl` | `DIRECTORY_URL` | empty: no region, a lone server as before |
| `ShardName` | `SHARD_NAME` | unique in the region |
| `ShardPublicAddress` | `SHARD_PUBLIC_ADDRESS` | `ip:port` the games reach this shard's link at |
| `DirectoryToken` | `DIRECTORY_TOKEN` | the shards' and the directory's shared secret |
| `RegionSecret` | `REGION_SECRET` | the tickets' key; default derived from the token |
| `ShardCapacity` | `SHARD_CAPACITY` | default 800 |
| `ShardDraining` | `SHARD_DRAINING` | |
| `ShardHeartbeatMs` | | default 2000 |

A shard needs `Hosted=true`. The directory: `REDIS_URL`, `DIRECTORY_TOKEN`, `PORT` (8080).

## Measured (`server/loadtest/run_shards.py`)

```
python3 server/loadtest/run_shards.py --shadnet build/shadnet --bbload build/bbload \
    --players 1500 --shards 3 --seconds 300 --ramp-ms 40     # --shards 1: the comparison
```

It starts a `redis-server`, the directory, the shards (one link key, `Hosted=true`, cheap signup
work) and bbload with `--shards`: each team's leader on one shard, its members on the others,
joining by code (even teams) or by invite and Accept (odd teams), so two of every three players
are sent on. bbload follows Redirects as the game does (the key pinned, the ticket in its hellos,
a new stream), and reports the redirect-to-welcome time.

On this box: **4 cores, shared** with other builds (load average 7-14 during the runs), the
simulated players on the same cores as the servers. 1500 players, 3 to a team, 300 s:

| | 3 shards | 1 shard |
| --- | --- | --- |
| Playing at the end | 1449 of 1500 (51 still joining) | 1500 |
| Sent on (Redirect) | 990, to the welcome p50 6.4 s, p99 23 s (during the join rush; 1.0 s p50 at 450 players) | - |
| Link thread, each shard | 18.4 / 18.5 / 18.8% avg, 47-55% max | **65.5% avg, 99.9% max** |
| Process CPU, each shard | 35% | 78% |
| UDP receive drops (the system's) | 2.2 M | 7.3 M |
| Relay p50 / p99 | 443 ms / 2.1 s | 171 ms / 1.5 s |

What it says: the link thread, one process's limit, is split three ways (65% of a core at 1500 in
one process becomes ~19% in each of three), so a region grows by adding shards; the relay delay
here is the box's, not the servers': 1500 simulated games and 3-6 servers and the directory on 4
busy cores (load average 14 in the 3-shard run against 7), with the players' own pings at the same
hundreds of ms. A fair number for the relay wants the players on other machines.

Also seen and fixed in the runs: a sent-on game's sign-in queued behind the join rush's new
accounts (password hashing) took 13 s; it now goes first on the sign-in pool (1.0 s p50, one hello
retry). A directory that did not answer a claim in 3 s held the hello's refusal 30 s; now 5 s.

Seen and not fixed: the Python directory, starved on this box, answered some heartbeats after a
shard's 10 s presence lapsed, so shards dropped out of the others' views for a moment (the move
waits: "stays: shard X is not in the view"; the joiner's next JOIN_CODE goes). In production give
it its own cores and two replicas; a longer presence (`PRESENCE_S`) is the knob.

## What is left

- **The game side is untested.** `p2p_sockets.cpp` (Redirect: the endpoint switched, the key
  pinned, the ticket in the hellos) and `http.cpp` (`FollowLinkHost`: the HTTP ports follow the
  link's host) do not compile on this box (blocked downloads). The shared `net_link.h` is covered
  by its gtest (compiled alone) and bbload mirrors the client. First test on the PC: two games on
  two shards, one invites the other by name.
- Each shard needs its **own IP address** in production: the HTTP ports (20443, 31315) are fixed
  numbers and follow the link's host; only the link port differs per shard locally.
- **Friends lists** (shadNet's own friends, TCP) stay per shard's SQLite: finding and inviting by
  name works region-wide (the directory's players), a friends list made on one shard is not seen
  on another. The shared accounts store (Postgres) is the fix.
- Bans per shard (`banned.txt`: mount one file); the account copies a ticket makes.
- Draining by restart only (no admin command yet); moving a drained shard's worlds by hand.
- No signed `/v1/shards` (the launcher trusts the region's key, shipped; see "Trust").
- The launcher's region path (`region_settings`) is unit-tested, not run against a live region.
- The directory's view is 2-4 s old: a code made a second ago is not found on another shard yet.
