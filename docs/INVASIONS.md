# Invasions

A solo player invades a team's world as a hostile (red) phantom. This page is what the **server**
does today (phases 1 and 2 of the design, server side only), the messages and settings, and what
the **game** still has to do before an invasion works end to end. The design and its reasons:
`research/hosted-server/research-invasions.md` (section 3, "Design").

Nothing changes for anyone unless the server's operator turns invasions on (`Invasions=true`) and
a team's leader allows invaders. Current games never see an invasion message: only a player who
asks to invade, and teams whose leader allowed invaders, hear of one. It works the same on a hosted
server and on a server at home.

## What the server does

### Matching

A player in no team sends `InvadeRequest`. The server refuses it at once (`InvasionEnd REFUSED`)
when invasions are off or the player is in a team. Otherwise it looks for a world now and then
every second, for up to 5 minutes (`NOT_FOUND` after that). A team world may be invaded when all
of these are true (`coop/invasions.h`, `InvasionRefusal`):

- its team's leader allowed invaders (`Team.allow_invaders`), the team is started and its host's
  session is open;
- it has no invader, on the way or in it (one at a time);
- fewer than `InvasionMaxPlayers` (4) of its games are in it (the game has 4 player slots);
- no member's vitals say a boss fight is running (`Vitals.boss`);
- a member is out in the world: not loading, not in the Hunter's Dream (`0x15000000`), not in a
  chalice dungeon (`0x1Dxxxxxx`), and within the level band (20 levels + 20% of the member's,
  only when both levels are known: `InvadeRequest.level`, `Vitals.level`);
- the world is not resting: 15 minutes after any invasion of it, 1 hour for the same invader.

When several worlds fit, one is picked at random. The invader and the team's games get
`InvasionNotice` (`in_ms` = 10 s). 10 s later the invader gets `InvasionStart` and the team gets
`InvaderJoined`. `InvasionStart` carries the target host's `TeamSession` with the invader's `kind`
and `sign`, the target's map and last position (to travel there first), and the team's enemy
owners. The owners are sent again to the invader as they change. The sign is `InvasionSign`, or the
first free slot byte below 4 if a team guest holds it.

### Routing

`Service::Member` has `visiting` (the visited world) and `hostile`. The invader keeps its own party
(usually none). `Hub::SameTeam` treats a visitor and the visited party's games as one team
(`Hub::SetVisiting`, `Service::VisitingOf`). `Service::RoutesPacket` sends their packets through
the hostile table (`coop/packet_routes.h`, `RoutesHostilePacket`), strict for now:

| Policy | Channels |
| --- | --- |
| Pass, both ways | handshake 0x07, 0x19, 0x10, 0x11, 0x3F, 0x42; character data 0x08, 0x0A-0x0D, 0x13, 0x3B, 0x3C, 0x43 |
| Same map only (both maps known) | 0x06 player state, 0x14 hits on players, 0x12/0x37/0x30 throws, 0x24/0x25 forced animations, 0x3E/0x40 bullets |
| To the invader, from its map's enemy owner only | 0x04, 0x05, 0x29, 0x36, 0x3D, 0x41, 0x17 |
| Dropped, both ways | 0x15/0x16/0x34 enemy hits and hand-over, 0x0E/0x0F/0x31 events, fog and doors, 0x2A/0x2B/0x2D/0x2E/0x2F objects, 0x2C items, 0x27, 0x39 Insight |

Any other channel follows the native table. While it visits, the invader reaches no other game.
The team's enemy owners are read from the visited world.

### Kept out of the world

The invader is never a member of the team's world. It does not go through `WorldFor`/`TakeWorld`
and gets no snapshot, entries, boards, owners, loads or places. The server takes only its map (for
the same-map rule) and its vitals (for its death). Its vitals also appear on the team's vitals
board as a line marked `hostile`, so its body stays shown on the team's screens
(`ReconcileMemberPresence`). That line never counts for wipes. Every other message it sends, such
as kill, flag or HP proposals, echoes or a party hello, is dropped. Each kind is logged once per
invasion. The hub also drops our own SHAD co-op frames (every type but the game's packets) between
the invader and the team.

### Ends

The server ends every invasion itself. It sends `InvasionEnd{reason}` to the invader and the
team's games, takes the invader out of the team's routing, and starts the cooldowns.

| Reason | When |
| --- | --- |
| `INVADER_DIED` | the invader's vitals say dead |
| `TARGET_KILLED` | its target (the session's host) is dead (vital 3, a real death) |
| `PARTY_WIPED` | the team's wipe (a wipe on the vitals board, or a member's `Lifecycle WIPE`) |
| `BOSS` | a member's vitals say a boss fight runs |
| `TIMEOUT` | 10 minutes after the join |
| `INVADER_LEFT` | it cancelled (`InvadeRequest.cancel`), its game restarted, or its link was lost or forgotten |
| `HOST_LEFT` | the target's session closed, or its host's link was lost or forgotten |
| `INVADERS_OFF` | the leader turned invaders off |

An end during the notice works the same way: the invader never joins.

## Messages (`coop/coop.proto`, append-only)

- `InvadeRequest{level, cancel}`: game to server, from a player in no team.
- `InvasionNotice{id, in_ms, invader, target}`: to the invader and the team, "an invader approaches".
- `InvasionStart{id, session, target, map, at, owners}`: to the invader. The session is the target
  host's `TeamSession` with the invader's kind and sign.
- `InvaderJoined{id, member, target, name}`: to the team.
- `VitalsBoard.Member.hostile`: the invader's line on the team's vitals board.
- `InvasionEnd{id, reason, invader, text}`: to everyone involved. A refused or failed search
  (`REFUSED`, `NOT_FOUND`) goes to the asker only.
- `TeamRequest.INVADERS` with `allow`, the leader only; `Team.allow_invaders` in the lobby state.
- `Vitals.level`, `Vitals.boss` and `Vitals.killed_by_invader`: new optional fields. A game that
  leaves them out means "unknown, no band", "no boss" and "not by the invader".

`PROTOCOL_CURRENT` stays 8: the optional reward capability is append-only, and victory grants
are sent only to games that advertise it.

## Settings (`shadnet.cfg`)

| Key | Default | What |
| --- | --- | --- |
| `Invasions` | `false` | the kill switch: invasions are off unless this is on |
| `InvasionKind` | `0xFFFFFFFF` | the invader's GameMan +0x16F0 value in `InvasionStart.session.kind` (**placeholder**: the co-operator's kind) |
| `InvasionSign` | `4` | its GameMan +0x16F4 value in `InvasionStart.session.sign` (**placeholder**: the slot after the guests' 7/6/5, a guess) |

The timings and limits are constants in `coop/invasions.h`: notice 10 s, invasion 10 min, team
cooldown 15 min, invader+team cooldown 1 h, search 5 min, session limit 4.

## Phase 0: the capture the placeholders wait for

Trace a retail Sinister Resonant Bell invasion on the shadPS4 path, using
`SHADPS4_CAPTURE_BLOODBORNE_SUMMON` and the packet trace (NATIVE_NETCODE.md "Phase 0"). Record:

- the u32 written to GameMan +0x16F0 (kind) and the u8 written to +0x16F4 (sign/slot) when the
  invader's claim is parsed (0x01872360). These replace `InvasionKind` / `InvasionSign`;
- the invader's ChrIns +0x78 character type and +0x88 team (21?), and the Lua events that follow
  (`OnJoinMutiplay`);
- the slot use: whether a 5th body (host + 3 guests + invader) is possible. If it is,
  `InvasionMaxPlayers` can go up;
- what ends a retail invasion on the wire (the boss kick 0x27, death, the host's death).

## What the game still has to do

The port's games do this now, untested in game (`docs/invasions/GAME_SIDE_PLAN.md`, "Status"):
all but the Sinister bell, with the native kind/sign (the fallback behind
`invasion_fallback_switch`) and phase 0 still to settle the placeholders. shadPS4 does none of it.

1. **The way in.** Bring back the Sinister Resonant Bell (goods 225) for players in no team: keep
   it out of `BellGoods`. Or add an "Invade" entry to the team or System menu. Either sends
   `InvadeRequest{level}`, and `cancel` when the player stops.
2. **The notice.** On `InvasionNotice`, show "An invader approaches" with a 10 s countdown to the
   team, and "Invading..." to the invader.
3. **The join.** On `InvasionStart`, travel to `map` / `at`, then run `JoinTeamSession` with the session's join data and its
   `kind`/`sign`, plus the seamless stage handoff to the target's map. If the native kind/sign does
   not make a hostile phantom, use the fallback: join with a free co-op slot, `SetChrType` to the
   black type locally, and on every team member's screen write team 21 onto its body
   (`DownedTeams`), kept for the whole visit.
4. **Keep the invader out of the party** on `InvaderJoined`: drop `hostile` vitals-board lines
   from the panel, wipes, downs and revives, and keep them only to show its body.
   `ReviveTarget` and `DownedBodyPicker` ignore hostile bodies. Also: no `DownedTeams`
   save and restore of its team, no enemy-owner part, no shared echoes, no world-log proposals, no
   `ApplyTeamWorldOnLoad` and no personal-world write in the invader's save.
5. **Real death on the invader's kill.** In `HitDamageStored`, a lethal hit whose attacker is the
   hostile player's body is a real death, not a down. Send vitals Dead (3) with
   `killed_by_invader` so the server sees `TARGET_KILLED`.
6. **Warp home.** On `InvasionEnd` (any reason) or its own death, the invader goes home: re-enable
   the `BlockClear2_3` / HostDead warp home for the hostile type, or `MultiPlayStopRequest`
   (0x01ED07A0) and load its own save. Team members show the end, for example "Invader defeated".
7. **Send the new vitals fields.** `Vitals.level` (character level) and `Vitals.boss` (flag 4000
   or the boss runner), so the server can apply the level band and the boss end.
8. **Session hygiene.** Make sure no host-world flags (join 0x10, session flags 9180-9199) persist
   in the invader's save, and that the team's games do not treat the invader as a session guest in
   `TeamGate` / `LoadProgress`.

## Opt-in victory rewards (2026-10-27)

The server issues a reward only for a **joined** invasion that it ends as `INVADER_DIED`
(the defending session host wins), or `TARGET_KILLED` with the victim's
`Vitals.killed_by_invader=true` (the invader wins). The reward and next grant counter become
durable before publication, and the invasion retires before another message is processed, so
duplicate death/end reports cannot issue another reward. A notice cancelled before
joining, environmental host death, party wipe, boss, timeout, disconnect, cancelled search, or
turning invasions off never pays. Guests do not get the host's defending reward.

A covenant-capable Corruption winner additionally receives one Blood Dreg, from the original
native PvP reward lots. See [covenant rewards and primary evidence](INVASION_COVENANT_REWARDS.md).

The payment is one Insight, plus Blood Echoes according to the explicit server policy in
`InvasionVictoryEchoes`: 10% for an invading winner, 30% for a defending host, of
`max(0, floor(0.02 L^3 + 3.06 L^2 + 105.6 L - 895))`. The defeated hunter's level is frozen at
match (invader) or join (host); a death report cannot increase it. Unsupported low levels and unknown/out-of-range levels
(outside 10..544) pay no Echoes. The polynomial is a **server policy**, not a claim that every
low-level or covenant-specific retail reward has been verified. Native game validation and
low-level curve verification are recorded separately by the consuming Mac client.

The protocol remains 8. `LobbyHello.invasion_rewards=5` opts in to these grants. Original
clients omit it and can still invade, defend and reconnect without receiving the unknown item
kind. Opting in requires implementing all of the following:

- `TradeItemKind::Insight=4`, id 0, count 1, empty name/data; it is server-issued and cannot be
  offered, requested, answered, refunded, or otherwise supplied by a client.
- `TradeGrantReason::InvasionReward=2`; its original item shape is one Insight plus at most one
  ordinary Echoes item (kind 1, id 0, positive count). Covenant-capable clients may additionally
  receive one goods-704 Blood Dreg; other goods/items/reasons are rejected.
- Reward grant IDs have bit 63 set. Ordinary party trade IDs do not. The client applies a valid
  grant once per run and sends `TradeGranted{id}` only after applying it.

Solo invaders send no party Hello. Their rewards therefore live in a separate **per-player
mailbox**, reusing `WorldStore` and `TradeDesk`: the store key is the hash of
`invasion-rewards:<authenticated online-id bytes as hex>`. It is calculated by the server;
clients supply no mailbox key. It is never the invaded team's world. A party-free winner can
receive and acknowledge a reward; its high-bit ACK is intercepted before hostile/party routing.
An ACK from another authenticated identity cannot clear that mailbox. Pending grants and the
next grant counter survive restart in `trades.pb`. A successful capable lobby hello resends
pending grants; a legacy hello preserves them without sending them. The server now confirms
file and directory durability through the ordered writer before publishing a grant or ACK removal;
see [the durable server boundary](INVASION_COVENANT_REWARDS.md#durable-server-commit-boundary).
Client inventory/save transaction behavior remains a separate client responsibility.

Both refused offers and late/mismatched answers now refund only valid ordinary items. The old
"return whatever the client claimed" behavior could launder an unknown kind into an application
grant; server-only Insight and unknown/invalid items are dropped instead. Legitimate ordinary
trade/refund behavior remains covered by the existing trade suite.

Original sources and authors: Kaniel / [lKaniel](https://github.com/lKaniel), Vitalii Shevchuk
(as recorded by the source history), and the shadNet
Project contributors, [pinned server source](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server),
[`TradeDesk` / `TradeClient`](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server/coop/trade.cpp),
and [invasion lifecycle](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server/src/bloodborne_world.cpp).
The durable reward mailbox, capability, validation and tests here extend those original
GPL-2.0-or-later sources; they do not change their attribution or claim original authorship.
Covenant-specific reward inventory is described in the linked follow-up; the Bell-Ringing Woman
remains outside this change.
