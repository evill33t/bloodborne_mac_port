# Seamless red invasions: game-side implementation plan

## Status (branch `invasions-game`, 2026-10-23)

Built in the port's shared sources (`emulator/src/core`, compiled into bbport's `bbnet`), untested
in game. Everything stays idle unless the server sends invasion messages.

- **Done (sections 1, 2, 4, 5, 7, 8, 9a, 10, 11's verbs):** `bloodborne_invasion.{h,cpp}` (pure,
  `test_bloodborne_invasion.cpp`): `InvaderTrip` (ask, notice, travel by `ChooseResumeStep`, join
  with the server's kind/sign, place, home), `InvadedTeam`, `TakeHostileLines` (the hostile line is
  split off in `TakeServerVitals`, so every board reader goes without it), `InvaderBodies`
  (presence keeps the body shown and finds its handles), `TeamSessionMembers`, `InvasionJoinOf`,
  `InvaderHitWindow`, `InvasionHome`. Messages in `TakeLobbyMessage`; `InvadeRequest` and
  `TeamRequest.INVADERS` through the lobby queue; the invader's map and vitals go without a party
  hello. `Vitals.level` (PlayerGameData +0x90), `boss` (flag 4000 while alive) and
  `killed_by_invader`. `DownedTeams` never saves or restores the invader's team; `DownedBodies`
  skips its body; `SessionNames` counts its line. Hit hook always installed; `KeepPartyMemberUp`
  lets the invader's kill through. Invader: keeps every own flag, no echoes, no lamps, no team
  world, keeps the target's enemy owners; death revived in place (`ReviveInvaderInPlace` from the
  `PartyGhostDeath1.Revive` / `SoloPlayDeath2.Respawn` hooks), then `InvaderGoHome`; boss fog
  (flag 4000) and a lost session send it home; its flags and lamp kept in `invasion_home_<slot>`
  before the trip, put back at home or at the next load after a crash. Port UI: "Invade a hunt" /
  "Leave the invasion", the leader's "Invaders: allowed/off", the notice banner; 18 languages. Rig
  verbs `menu-invade`, `menu-invade-leave`, `menu-allow-invaders`, `wait-invader`.
- **Fallback (section 3)** behind `invasion_fallback_switch` (off): co-op kind + server slot,
  `SetChrType(InvaderCharacterType)` kept for the visit, team 21 written on its body on the team's
  screens (only while real revive hits are on).
- **Not done:** phase 0 (kind/sign, `InvaderCharacterType` = 2 is a guess), 9b (the Sinister bell),
  the nameplate's "Adversary" line, map item pickup refusal, `GiveBackPhantomKeyItems` for the
  invader, rig scenarios, shadPS4 (section 12: nothing, as recommended).
- **Riskiest, untested:** whether `JoinTeamSession` with the native kind/sign makes a red phantom;
  which death script runs for the invader (the 8 s fallback revive); the lamp trip home after
  `MultiPlayStopRequest`; restoring every flag; flag 4000 as the boss signal; the invader's game
  keeping the host party role while a session guest.
- **Compiled:** `shadps4_bloodborne_test` (755 tests pass, clang; built test-only without the
  emulator's submodules); every touched emulator source and `bbcoop/coop.cpp` syntax-checked with
  bbnet's flags, with and without `BLOODBORNE_NATIVE_PORT`. `bbport_overlay.cpp` was **not**
  compiled (no Dear ImGui / SDL3 here). Menu text and rig Python tests pass (2 rig failures,
  `wait-hp`, were there before).

Repo: `/home/user/bloodborne-server`, branch `hosted-server`, read-only, 2026-10-23.
Companion to `/mnt/project-files/research/hosted-server/research-invasions.md` (the "research
report"). The server side (InvadeRequest, InvasionStart/InvaderJoined, InvasionEnd{reason},
the team's "allow invaders" setting) is being built separately. This file covers what the
**games** must do. Paths are relative to the repo root. Line numbers are the ones read on
2026-10-23.

Tags: **[verified]** means read in the repo. **[guess]** means not verified; phase 0
(`PHASE0_CAPTURE.md`) should settle it. Estimates are developer-days for someone who knows the
codebase and include a rig scenario run. They exclude phase 0.

---

## 0. Ground truth that shapes the plan

1. **One source tree, two builds.** bbport compiles `emulator/src/core/bloodborne_*.cpp` unchanged
   into its `bbnet` library (`docs/BBPORT.md` "How it is wired"). The port's own UI lives in
   `bbport/gpu/bbcoop/coop.cpp` (the C API from `bbport/gpu/bbcoop.h`) and in
   `bbport/gpu/shim/bbport_overlay.cpp` / `bbport_game_menu.cpp`. **[verified]**
2. **The server world, teams, TeamSession and boards exist only in the port.**
   In `bloodborne_server_world.cpp:48-53`, `server_world_switch` is `true` only under
   `BLOODBORNE_NATIVE_PORT`, and `team_switch = server_world_switch` (`:83`). shadPS4 keeps
   bells, signs, P2P and HTTP boards. **[verified]** Consequence: seamless invasions through
   `InvadeRequest` are **port-only**. On shadPS4 the native Sinister bell path is the only one
   (section 12).
3. **The team join today.** In `TickTeamJoin` (`bloodborne_server_world.cpp:1590`):
   - The host runs `TeamJoin::Action::CreateSession`, which calls `OpenTeamSession()`
     (`bloodborne_join.cpp:1640`, NetSync CreateSession 0x01788ED0).
   - Then `Publish` sends `bbcoop::TeamSession{join, kind=TeamSessionKind 0xFFFFFFFF,
     sign=TeamSessionSign 7}` through the `say` lambda (`:1654-1661`, constants `:168-169`).
   - A guest's `Join` (`:1706-1719`) takes `session->kind()` from the server.
   - **The guest ignores the server's sign.** It replaces it with
     `TeamGuestSign(lobby, member, host)` (`bloodborne_team.cpp:24`, 7/6/5 by team order) and
     then calls `JoinTeamSession(join, kind, sign)` (`bloodborne_join.cpp:1690`).
   - `JoinTeamSession` writes GameMan(=GlobalState) `+0x16F0 = kind` and `+0x16F4 = (u8)sign`
     (`:1725-1726`). It calls JoinSession 0x01789010 and then writes the summon placement
     **from the guest's own spot** (`ReadLocalSummonPlacement`, `:1700-1745`). A team guest
     therefore **joins where it stands**. **[verified]**
   - `TickTeamJoin` returns early unless `in_team` (`team_party` set and `lobby_playing`,
     `:1606-1622`). A solo invader never reaches the join code today. **[verified]**
4. **`TeamSession` arrives** in `TakeLobbyMessage` (`bloodborne_server_world.cpp:259-270`) on
   the stream thread. It is stored in `team_session` under `team_mutex`. New invasion messages
   belong next to it. **[verified]**
5. **Everything team-side that decides "who is a teammate" reads the vitals board.**
   - `ReadVitalsBoard` (`bloodborne_party_death.cpp:1017`) feeds the panel, compass, map,
     downs, revives, the cutscene vote and the boss runner.
   - Character visibility comes from the board too: `ReconcileMemberPresence`
     (`bloodborne_re.cpp:697`) with `ShownOtherPlayers` (`bloodborne_party.cpp:6179`).
   - **A body with no board member near it is hidden** (no model, no collision). If the invader
     is not on the team's board, every team member's game hides it. **This is the first thing
     that would break.** **[verified]**

### Interface asked of the server (sent to the invasions-server agent)

- **InvasionStart (to the invader):**
  - `TeamSession{join, kind, sign, host}` with the invader `{kind, sign}` from config;
  - `target_map` (u32 packed) and `spawn` (Position);
  - the target team's `EnemyOwners` for the length of the visit.
- **InvaderJoined{member, name} (to the team).** Also the invader's Vitals line on the team's
  `VitalsBoard` with a new `bool hostile` on `VitalsBoard.Member`. The recommended design
  (A below) depends on it.
- **InvasionEnd{reason} to both sides.** Reasons used below: `INVADER_DIED`, `INVADER_WON`,
  `HOST_LEFT`, `BOSS`, `TIMEOUT`, `OPTED_OUT`, `LINK_LOST`.
- **Optional `killed_by_invader` from a victim:** a Vitals field or its own message.
- **Routing:** keep the SHAD co-op frames (DirectVitals, DirectKills, world-log frames,
  `bloodborne_boards.cpp:230-260`) apart between invader and team, as for the native channels.

### Two designs for "which body is the invader" on team screens

- **A, recommended: the invader is on the board with `hostile=true`.**
  - Presence keeps working with no change.
  - `SessionNames` / `OnlySessionMembers` (`bloodborne_party_death.cpp:985`,
    `bloodborne_party.h:3406`) keep working, because the invader's online id is among the
    board's candidates.
  - Each consumer filters out hostile lines. One new helper does it,
    `TeammatesOnly(board)` in `bloodborne_party.cpp`.
- **B, game-only fallback:** take the invader's member from InvaderJoined and match bodies by
  identity. Use `MemberIdentityOf(character)` (`bloodborne_re.cpp:837`) and then
  `FindOnlineIdIn` / `BoardMemberOfOnlineId` (`bloodborne_party.h:715-719`), as
  `SessionMemberBoardName` does (`bloodborne_join.cpp:1271`). Presence then needs an explicit
  "never hide the invader" exception.
- **Both designs need** a per-tick cache `invader_handles` (u32 character handles, under a
  mutex, like `bar_bodies` at `bloodborne_party_death.cpp:786`). The hit hook reads it.

---

## 1. New invasion state (shared core), all builds

New file `emulator/src/core/bloodborne_invasion.{h,cpp}` (add it to `bbport/gpu/CMakeLists.txt`
`bbnet` sources and `emulator/CMakeLists.txt`).

- **Invader side (`Coop::Invading()`).** An atomic state machine:
  `None -> Asked -> Travelling -> Joining -> Placed -> Visiting -> GoingHome -> None`.
  It holds the InvasionStart payload and the "home snapshot" (section 8).
- **Team side (`Coop::InvadedBy()`).** The invader's board name, its character name,
  `approach_until_ms` for the notice, and the cached `invader_handles`.
- **Message intake.** `TakeLobbyMessage` (`bloodborne_server_world.cpp:222`) handles
  `has_invasion_start()`, `has_invader_joined()` and `has_invasion_end()` beside
  `has_team_session()` (`:259`). It stores them under `team_mutex` and logs one line each, so
  rig checks can match them.
- **Outgoing.** `Queue()` (`:300`) or `QueueLobby()` (`:179`) carries `InvadeRequest` and
  `InvasionEnd` from the game (invader quits).
- **Ticks:**
  - `TickInvasion()` runs from `TickPartyController` (`bloodborne_re.cpp:972`), before
    `TickTeamJoin` (`:1030`). `TickTeamJoin` returns `true` whenever `team_switch` is on, so the
    invasion tick must not live after it.
  - `TickInvadedTeam()` runs inside `TickPartyDeath`'s tick, or right before it at `:1000`.

Estimate: **2 d** (state, messages, logs, unit tests in `emulator/tests`, the way
`TeamJoin::Tick` is tested).

---

## 2. Joining as the invader (native kind/sign path)

Where: `bloodborne_invasion.cpp`, calling into `bloodborne_join.cpp`.

1. **Travel.** The invader joins in place, so it must stand on `target_map` first.
   - Reuse the guest resume logic: `ChooseResumeStep` (`bloodborne_party.cpp:1526`) and
     `StartTripToLamp` (`bloodborne_join.cpp:707`), then `TeleportLocalPlayer`. These are what
     `ResumeWhereTheGuestLeft` uses (`bloodborne_travel.cpp:1266-1420`; its Travel phase
     `:1395-1410` is the pattern).
   - `StartTripToLamp` writes GlobalState `+0x1528` (the respawn lamp, `:712`). Snapshot it
     first (section 8).
   - **[guess]** A trip to a lamp the invader has never lit works, as party guest trips do.
     Verify on the rig.
2. **Join.** Once settled on the map (3 s stable, as the resume does), call
   `JoinTeamSession(join, start.kind(), start.sign())`. Do this **directly**, not through
   `TickTeamJoin`'s Join case (that one would replace the sign with `TeamGuestSign`).
   - `JoinTeamSession` truncates sign to u8 (`:1726`). That matches the game's own u8 slot
     (`docs/native-netcode/session.md:70`).
   - It also calls `SummonedNotice` 0x01FF58B0 (`:1747`), the "summoned" banner. **[guess]**
     Phase 0 should show whether an invader join wants a different notice or none. If it is
     wrong, skip that call when `Invading()`, with a parameter on `JoinTeamSession`.
3. **Place.** After the join settles (`InPartySession()` true, matching `+0x124 == 6`,
   `bloodborne_re.cpp:473`), call `TeleportLocalPlayer(start.spawn)`.
4. **The team side must not hold the invader in its team flow.** It is never a `TeamMember`,
   gets no TeamLoad row and no Start. That is server work, with nothing to change in the game,
   **[verified]**: TeamLoad rows come from `lobby.team().members()`, see `TeamLoadMembers`
   (`bloodborne_team.cpp:56`).
5. **The host's session must accept a 3rd or 4th member.** **[guess]** The host's CreateSession
   uses `SummonSessionSeconds = 24 h` (`bloodborne_join.cpp:1613`). The game's 4 remote slots are
   the open question (research report risk 2).

Estimate: **3 d** (travel plus join plus placement), **+2 d [guess]** if the lamp trip to an
unlit lamp or the placement fights the summon placement code.

---

## 3. Fallback: join as a co-op phantom, then make it hostile

Use this if phase 0 shows that the native invader kind/sign through `JoinTeamSession` does not
produce a red phantom on team 21. That can happen because only a real Sinister-bell summon
sets it up, or because event 9240 / effect 9020 gates it.

- **Invader's own game.** Join with kind `0xFFFFFFFF` and the server's free slot byte. Then
  call `SetChrType(0, LocalPlayerEntity, InvaderCharacterType)` through `SetChrTypeOffset`
  0x01338DF0 (`bloodborne_re_sites.h:91-92`), the way `PlayGuestAsHost` does
  (`bloodborne_join.cpp:1352-1361`).
  - SetChrType runs the type refresh (vtable +0x1A0, SetTeamTypeDefault, per
    `bloodborne_party.h:3518-3523`). **[guess]** For the invader type that also sets team 21
    locally. Verify with `peek self 88` (PHASE0).
  - Re-apply after every load and trip. The game resets type on reload, as `PhantomHome` shows
    (`bloodborne_team.h:340-360`).
- **Every team member's game, each tick.** Write team 21 onto the invader's body, kept for the
  whole visit.
  - Extend `DownedTeams` (`bloodborne_party.h:3530`, `Tick` at `bloodborne_party.cpp:5928`) with
    a third input, `hostile` (the `invader_handles`):
    - those handles are never put in `saved`;
    - they never get a "restore own team" write;
    - they always get `Hostile` (21) when they read otherwise.
  - Without that, a downed member standing on the invader's spot could hand its saved team back
    to the invader's body (the research report: "must never save and restore the invader's own
    team 21"). Today the code looks bodies up by handle only (`SetDownedTeams`,
    `bloodborne_party_death.cpp:795-824`).
  - Optionally write ChrIns `+0x78` too (the remote copy's type), so the red tint shows.
    **[guess]** The tint may come from the type alone.
- **Hit acceptance on the victim's side (research report risk 5)** depends on the victim's
  table row [21][19/20]. **[guess]** It allows hits, since retail invaders hit hosts. Rig
  scenario `invade-hits` (section 13) must prove that 0x14 hits from a team-21 body land.
- **Patch sites.** All the type-1 ("white phantom") checks below then read a non-1 type for the
  invader, which is what we want (section 6).

Estimate: **3 d** on top of section 2 (most of the join is shared), **+3 d [guess]** if team
21 does not stick (the game rewriting it on SetTeamTypeDefault) or hits do not land.

---

## 4. Co-op layers that must skip the hostile member (team side)

Add one helper in `bloodborne_party.cpp`, `std::vector<MemberVitals> TeammatesOnly(board)`. It
drops `hostile` lines; under design B it drops the invader's member instead. Then apply it here:

| # | Layer | Where (verified) | Change | d |
|---|---|---|---|---|
| 4.1 | Board intake | `TakeBoard` kVitalsBoard (`bloodborne_server_world.cpp:372-396`) | Carry `entry.hostile()` into `MemberVitals` (new `bool hostile`, `bloodborne_party.h:3303`). | 0.5 |
| 4.2 | Party panel, compass, map | `PostVitalsAndPanel` -> `BuildPartyPanel(tick.board, ...)` (`bloodborne_party_death.cpp:1413`, `bloodborne_party.cpp:6145`). The port reads it through `bbcoop_party_panel` (`bbport/gpu/bbcoop/coop.cpp:166`) and `bbcoop_map_view` (`:230`). | Build from `TeammatesOnly`. Optionally add a red "Adversary" marker later. | 0.5 |
| 4.3 | Armed / "someone else stands" | `TickPartyDeath`: `SomeoneElseStands`, `SomeoneElseStandsAnywhere` (`bloodborne_party_death.cpp:1474-1478`, `bloodborne_party.cpp:5774/5780`) | Use teammates only, or a lone member next to the invader goes down instead of dying. | 0.5 |
| 4.4 | Wipes | `WipeFor` (`bloodborne_party.cpp:5710`), `EndOfDown` (`bloodborne_party_death.cpp:1348`) | The server computes wipes (it excludes the invader). The game needs no change. Add a test only. | 0.25 |
| 4.5 | Revives | `ReviveTarget` (`bloodborne_party.cpp:6042`), `SwingTargetByBody` / `DownedBodies` (`bloodborne_party_death.cpp:716, 1229`), `DownedBodyPicker::Pick` (`bloodborne_party.cpp:5955`) | Pass `TeammatesOnly(board)`. **Remove `invader_handles` from the `positions` in `DownedBodies`**: the picker takes the nearest body to a downed member's spot, and that can be the invader standing over its kill. | 1 |
| 4.6 | DownedTeams | section 3 | `hostile` set. | (in 3) |
| 4.7 | Enemy-bar and real revive hits | `RecordReviveHit` / `real_hit_bodies` (`bloodborne_party_death.cpp:766-787`) | The invader's body is never in `real_hit_bodies` (follows from 4.5). | 0 |
| 4.8 | Presence | `ReconcileMemberPresence` (`bloodborne_re.cpp:697-830`), `ShownOtherPlayers` | Design A: no change, the invader's line keeps its body shown. Design B: never hide `invader_handles`. | 0.25 / 1 |
| 4.9 | Cutscene vote | `others_watching` from the board (`bloodborne_server_world.cpp` CutsceneVote frame) | Teammates only. | 0.25 |
| 4.10 | Boss runner | `BossRunnerPick::Tick(board)` (`bloodborne_party.h:3503`) | Teammates only. The invader must never run a boss. | 0.25 |
| 4.11 | Shared kill echoes | Team side: the server never sends the invader's `Echoes`. Invader side: `NoteAddSouls` (`bloodborne_party_rules.cpp:859`) pushes `echoes_to_post` when `SharesKillEchoes` (`bloodborne_party.cpp:2545`) | Invader side: no posting while `Invading()`. **[guess]** The native kill reward may still pay the invader for the host's kills on its map (the "full echoes" patches `GiveGuestsFullEchoes` make phantom rates 1.0). Phase 0 checks it. | 0.5 |
| 4.12 | Enemy owners | `TickEnemyOwner` (`bloodborne_kills_hp.cpp:1255`): role from server owners and `vitals_member` | Team: the server never names the invader. Invader: with the target's owners list from InvasionStart, `EnemyRoleOn` returns Follower. Check that `self` (`vitals_member`) is the invader's own board name. | 0.5 |
| 4.13 | "Shares one world" heuristic | `SharesOneWorld` (`bloodborne_kills_hp.cpp:1399`): any type-1 player present | Leave as is (the invader is not type 1 natively). In the fallback the invader's type is also not 1. | 0 |
| 4.14 | Session-name filtering | `SessionNames` (`bloodborne_party_death.cpp:985`) | Design A: no change. Design B: add the invader's online id to `online_ids` and drop its name from the result. | 0 / 0.5 |
| 4.15 | TeamJoin "alone in session" | `TeamJoin::Tick` `session_members <= 1` (`bloodborne_team.cpp:534`), `SessionMembers()` (`bloodborne_re.cpp:480`) | A guest left alone with only the invader would not `LeaveAlone`. Subtract the invader when `InvadedBy()`. | 0.25 |
| 4.16 | Nameplate role line | `WriteNameplateRoleLine` (`bloodborne_party_death.cpp:1575`) | Optional: "Adversary" in red for `invader_handles`. | 0.5 |

Subtotal: **~6 d** (design A).

### Item and flag sync (team side)

- **Flags and events from the invader's game.** Ch 0x0E/0x0F/0x31/0x2D/0x2E are dropped by the
  server's hostile table. As a game-side second line, `KeepLocalFlagsFromOthers`
  (`bloodborne_flags_store.cpp:1145`) has no sender. **[verified]** It cannot tell the invader
  apart, so rely on the server. 0 d.
- **Map items (0x2C)** are "Ours" already (`server/coop/packet_routes.h`). The invader gets no
  world log, so its pickups are its own. 0 d.

---

## 5. Kill attribution: an invader's lethal hit is a real death

Where: the hit hook `HitDamageStored` at 0x01A2BCA3:
- constants at `bloodborne_party.h:3570-3572`;
- attacker at `[rbp-0x318]`, victim at `[[r14]+8]`, damage in `eax`;
- installed in `InstallPartyDeath` (`bloodborne_party_death.cpp:2021-2028`) with
  `RecordReviveHit` (`:766`).

1. **Install unconditionally.** The hook today is installed only in the "real revive hits"
   branch (`:2021`, the `else if` after `SHADPS4_BLOODBORNE_REVIVE_HITS=presses`). Install it
   for invasions in both cases. Simplest: always install it and keep the presses switch only
   for the revive counting.
2. **Record the hit.** In `RecordReviveHit`, before the `attacker != GetLocalPlayer()` return:
   if `victim == GetLocalPlayer()` and the attacker's handle (`[attacker+8]`) is in
   `invader_handles`, store `invader_hit_ms = now` and `invader_hit_damage`. Use atomics, since
   the hook runs on the game thread.
3. **Let the death through.** In `KeepPartyMemberUp` (`bloodborne_party_death.cpp:913-962`),
   when `check == NoDeadCheck::Lethal` and `now - invader_hit_ms < 1000`, return before the
   `r14` overwrite (`:957`). The game then kills the player. `TickStanding` takes the
   `!tick.alive` branch, "died for real" (`:1259-1277`).
4. **Already down.** If the player is already down (`TickDown`), an invader hit can never be
   lethal, because `GoDown` sets no-damage flags (`DownedNoDamageFlags`, `:1083`). Rule: a
   downed member is safe from the invader. **[design decision]** The alternative is to end the
   down at once with `DieFromDown(run, ..., reset=false, "invader")` on the first invader hit.
   That is cleaner for "the invader wins" (Kaniel to decide).
5. **The death's map reset.** It goes through `DiesWithReset(in_session &&
   DeathResetsMap(false, armed))` (`:1263`). A member alone on its map then resets the map and
   tells the server (`TellServerMapWipe`, `bloodborne_server_world.cpp:711`). That is the same
   as dying alone to an enemy today, so keep it.
6. **Respawn.** It is party real death as today:
   - a host: `OverrideHostDeathRespawn` (`bloodborne_party_death.cpp:589`);
   - a guest: `ReviveGuestAfterGhostDeath` (`:536`);
   - then `MoveToRespawnPoint` (`:362`).
   - **[guess]** The bloodstain or echo loss follows `UpdateGuestBloodMark` as for any party
     real death. The research report wants "solo-like": check on the rig with `expect-stains`.
7. **Tell the server.** Set `killed_by_invader` on the next `PostVitals`
   (`bloodborne_boards.cpp:970`) after such a death, so the server can send
   `InvasionEnd{INVADER_WON}`.

Estimate: **2 d** (+1 d for the "end a down" variant).

---

## 6. Patch sites keyed on "phantom" (`emulator/src/core/bloodborne_patch_sites.h`)

**Byte patches** are written once at install, and the port's image is read-only after
`bbcoop_install` (`docs/BBPORT.md`). They **cannot be switched per character or per visit**.
Where the invader must take the native branch, the patch has to become a **hook** that decides
at run time, or our own hook must do the right thing after the patched code.

| Site (line in patch_sites.h) | Keyed on | Affects | Invader needs | Plan |
|---|---|---|---|---|
| `PartyGhostDeath SetEventFlag 4047` x2, `PartyGhostDeath goodbye`, `PartyGhostDeath flag reset`, `PartyGhostDeath_1 menu branch` (`:183-195`, "ghost death", required) | phantom death script | the invader's own game when it dies, **if** a black phantom's death runs PartyGhostDeath **[guess]**: `lua50dis.py` and phase 0 settle it | its own trip home | Keep the patches. In the existing hook `PartyGhostDeath1.Revive` 0x01382354 (`:197`, handler `ReviveGuestAfterGhostDeath`, `bloodborne_party_death.cpp:536`), when `Invading()` call `InvaderGoHome(died=true)` (section 7) instead of `ReviveDeadPartyGuest`. Today `ReviveDeadPartyGuest` returns false for non-white types (`bloodborne_party.cpp:360-366`, `is_white_ghost()`), which would leave a dead invader lying there with no menu. |
| `HostDead white phantom branch` (`:210`) | white phantom | the invader's game when **its host** dies natively | Ours: the server ends the invasion (`HOST_LEFT`/`INVADER_WON`) | None. Our `InvasionEnd` drives the trip home. |
| `character death member removal` (`:213`) | dying host | the host's world keeps its session members | fine: the team needs it. The invader is sent home by InvasionEnd | None |
| `SoloPlayDeath2.Respawn` hook (`:216`, `OverrideHostDeathRespawn` `bloodborne_party_death.cpp:589`) | live player in a session (`ShouldReviveHostInPlace`: matching 3 or 6) | the invader's game if its death runs SoloPlayDeath (fallback, or a type-0 invader) | its trip home | `if (Invading()) { InvaderGoHome(true); return true; }` |
| `BlockClear2_2 goodbye`, `BlockClear2_2 leave`, `BlockClear2_3 flags`, `BlockClear2_3 warp home`, `BlockClear2 load wait`, `SoloBlockClear leave` (`:219-236`, "boss clear") | white phantom at boss clear | the invader stays after a boss | The invader must be gone before a boss fight | Server: `InvasionEnd{BOSS}` when a fog entry or BossRunner starts. Game: the invader's game also refuses boss fog. Watch fog-entry flags (`IsBossFogEntryFlag`, `bloodborne_party.cpp:1151`) and call `InvaderGoHome` on one. |
| `leave limit` hook (`:348`, `StayWhenTheHostLeaves` `bloodborne_party_death.cpp:551`, `SkipsLeaveLimit` `bloodborne_party.cpp:4460`) | `type != 1` means always stay | the invader's game 5 s after losing its host | either the native trip home (a reload of its own save) or ours | When `Invading()`, return `false` (let the native OnLeave_Limit "reset and warp into own world" run) **only if** phase 0 shows that path works with our patches. Otherwise return true and call `InvaderGoHome`. **[guess]** |
| `CSMultiPlayMan StopRequest` hook (`:355`) and the "server owns leaves" patches (`:539-555`: session idle timeout, resend overflow, room timeout, matching check) | none (session) | an invader whose link dies is never dropped by the game | the server's `LINK_LOST`/`TIMEOUT` ends it | The invader's game also runs a local timeout: no board or server message for 30 s means `InvaderGoHome`. |
| `talk phantom check` (`:371`), `TalksAsHost` (`bloodborne_party.cpp:2796`) | type == 1 | white phantoms talk to NPCs as the host | the invader must not talk | None if the native type is not 1. In the fallback the type is not 1 either. Verify. |
| `HealingFountain.Availability.Host` / lamps (`ApplyHealingFountainHostAvailability`, `bloodborne_join.cpp:1365`) | `InPartySession()` | lamps unlocked for party members | the invader must not use lamps (retail **[believed]**) | `if (Invading()) leave blocked`. |
| `map item pickup check` x2, `shared loot` hooks (`:418-433`) | session state 6 (a guest) | guests pick up map items | the invader must not loot | In the pickup-check handlers, refuse while `Invading()`. **[guess]** about where the "can pick up" answer is; see `InstallSharedLoot` in `bloodborne_party_rules.cpp`. |
| `menu client condition` (`:415`) | session | guest's menus | | none |
| `guest hits NPCs` (`:381-388`, row 20 of the relation tables) | team 20 | guests hit friendly NPCs | the invader is team 21. Row 21 is untouched | none |
| `quiet summon banners` (`:242-247`) | arrival banners | "Discovered the chime of ..." | the team should see the invader arrive | Leave it. Our notice replaces it (section 10). |
| `no multiplayer banners` (`:307`) | in a party | | | none |
| `PartyGhostDeath1.Revive` / `ReviveInPlace` `set_chr_type(WhiteGhostCharacterType)` (`bloodborne_party.cpp:313-327`) | sets type 1 back after a revive | would turn a revived invader white | | Never revive an invader in place (it goes home). |
| `PhantomHome` (`bloodborne_server_world.cpp:1628-1640`, class at `bloodborne_team.h:340`) | type == 1 | a white phantom out of a session goes back to type 0 after 5 s | the invader at home must be type 0 again | Extend the check to `InvaderCharacterType`, or simply call `SetChrType(0)` in `InvaderGoHome`. |
| `GiveBackPhantomKeyItems` (`bloodborne_travel.cpp:1512`, `PhantomType = 1`) | type == 1 | the phantom's key item list | **[guess]** whether an invader also gets the swapped key item list (inventory +0x58) | Phase 0 checks it. If it does, apply the same logic for the invader type. |

Estimate: **3 d** (hooks and branches) **+2 d [guess]** for surprises in the death and leave
paths.

---

## 7. The invader goes home (death and every other end)

`InvaderGoHome(bool died)` in `bloodborne_invasion.cpp`:

1. If `died`, revive in place with the existing calls: `MakeReviveCalls()`
   (`bloodborne_party_death.cpp:500`) and `ReviveInPlace` (`bloodborne_party.cpp:313`) with
   `character_type = 0`, but **without** `warp_to_last_lamp` (we choose the lamp).
2. Leave the session: `MultiPlayStopRequest(manager)` (0x01ED07A0, as `TeamJoin::LeaveAlone`
   does at `bloodborne_server_world.cpp:1697-1702`).
3. Set the type back to live player: `SetChrType(0, 10000, 0)`.
4. Restore the home snapshot (section 8): `+0x1528`, then `StartTripToLamp(saved_point)`
   (`bloodborne_join.cpp:707`), so the invader wakes at **its own** lamp.
5. Tell the server: `InvasionEnd{reason}`. Set `Invading = None` when the trip has arrived.
6. Callers:
   - the death hooks (section 6);
   - the server's `InvasionEnd` (any reason);
   - the local timeout;
   - a boss fog;
   - the player choosing "Leave invasion" (menu, section 9).

Estimate: **2 d**.

---

## 8. Save hygiene for the invader

The invader plays in **its own** world memory, joined in place. Risks:
- the host's join snapshot (ch 0x10 "MNSE" flags into SprjEventFlagMan at matching 6,
  `docs/native-netcode/session.md` sequence);
- remote flag sets (0x0F) and events;
- the lamp trip's `+0x1528`;
- autosave during the visit;
- the bell flags 9180-9199 (`docs/FLAGS.md`, the Insight refund in `docs/HANDOFF.md` "Bell spam
  and Insight duping").

1. **Snapshot at InvasionStart, before the trip:**
   - GlobalState `+0x1528`;
   - echoes and Insight;
   - **all** event flag blocks. Reuse `CaptureWorldFlags` (`bloodborne_team.h` "Phase 5b") with
     a variant that keeps every flag, not only `IsWorldFlag`.
   Write it to a slot file `invasion_home_<slot>` (the same pattern as `PersonalWorldFile`,
   `bloodborne_server_world.cpp:1805`) **before** anything changes. A crash mid-visit is then
   restored at the next boot.
2. **During the visit:**
   - `KeepLocalFlagsFromOthers` and `KeepLocalEventsFromOthers`
     (`bloodborne_flags_store.cpp:1145, 1184`) keep the local value for **every** flag while
     `Invading()`, not only `KeepsLocalFlag`.
   - Never run `ApplyTeamWorldOnLoad` (`bloodborne_server_world.cpp:1858`). It already returns
     unless `team_world_due`, and the invader has no team. Add an explicit
     `if (Invading()) return;`.
   - No world log or `KillProposal` from the invader's game: skip the posting while
     `Invading()` (the server drops them anyway).
3. **At home:**
   - Put back the flags that differ: `WorldFlagChanges` (`bloodborne_team.h`), set with the
     `PartyGhostSetEventFlagOffset` setter as `ApplyTeamWorldOnLoad` does.
   - Put back `+0x1528`.
   - Delete the file.
   - Log the echo and Insight delta. Rewards later go through TradeDesk grants, so any other
     delta is a bug.
4. **Boot.** If `invasion_home_<slot>` exists when a save loads, restore it first (hook beside
   the personal-world restore in `ApplyTeamWorldOnLoad`'s load path,
   `bloodborne_flags_store.cpp:518`).
5. **Bells.** The invader never posts a native sign, because the bell triggers our request
   (section 9). Flags 9180-9199 come back from the snapshot anyway.
6. **Resume file.** `ResumeWhereTheGuestLeft` (`bloodborne_travel.cpp:1266`) runs for
   `IsPartyGuestRole()`. That is false for a solo invader (the search effect is not the Small
   Resonant Bell's, `bloodborne_flags_store.cpp:326`). **[verified]** No change, but test it.

Estimate: **3 d**.

---

## 9. The trigger: menu "Invade" first, bell optional

### 9a. Menu or overlay (recommended first)

- **Team screen in game.** In `bbport_overlay.cpp` `Team::Screen`, the bottom buttons
  (`:3409-3445`): when `lobby.menu && !lobby.in_team && lobby.playing`, the middle slot (now the
  disabled "Join the hunt") becomes **"Invade a hunt"**. It calls a new
  `bbcoop_invade_request()`. While invading, it becomes **"Leave the invasion"**.
- **C API.** `bbport/gpu/bbcoop.h` gets `void bbcoop_invade(int on)` and
  `int bbcoop_invasion_view(bbcoop_invasion*)`. Implement them in `bbport/gpu/bbcoop/coop.cpp`
  next to `bbcoop_team_join_hunt` (`:594`).
- **Leader's "Allow invaders" toggle.** Put it on the same screen (a `Button`, or a row under
  the world picker) when `lobby.in_team && lobby.leader`. It sends `bbcoop_lobby_request` with
  the server's new `TeamRequest` action (`:605`). Show the current state from the new `Team`
  field in `bbcoop_lobby_view` (`:391`).
- **The System menu's "Co-op Team" page** (`bbport_game_menu.cpp:566-602`, item added at `:742`,
  texts at `:852-858`) already opens this screen. No new System item is needed.

Estimate: **2 d** (UI, C API, the leader toggle, English and Russian texts).

### 9b. The Sinister Resonant Bell (optional, retail feel)

- **Strip.** `BellGoods{200, 205, 225, 4312}` (`bloodborne_team.h:265`) is removed every 3 s
  by `RemoveBellItems` (`bloodborne_trade.cpp:632`, called from `TickTrades` `:664`). Trades of
  bells are refused in `TakeTradeItems` (`:354`) and `bbport/gpu/bbcoop/trade.cpp:66`.
  `OwnKeyItemsForPhantom` and `PhantomKeyItemsToGiveBack` skip bells
  (`bloodborne_team.cpp:254, 279`).
- **Keep 225.** Remove it from the array used by `RemoveBellItems` only. Split it into
  `CoopBellGoods{200, 205, 4312}` and keep `IsBellGoods` for trades and phantoms, or allow the
  bell to be traded.
- **Detect a ring.** `LogLocalEffectChanges` (`bloodborne_trace_diag.cpp:94`) already walks the
  effect list. Use the same loop for effect **9025** (the Sinister bell's,
  `emulator/documents/bloodborne-seamless-re.md` goods table). On a ring:
  - send `InvadeRequest`;
  - end the effect, so the native sign never posts (`EndPlayerEffect`, as
    `bloodborne_re.cpp:1053` uses).
- **[guess]** The port has no `/summon_messenger` route that would answer the native sign.
  Ending the effect early avoids error popups. Test it.
- **Sold by the Insight messenger.** No change needed.

Estimate: **1.5 d** (+1 d if ending the effect still leaves a native popup).

---

## 10. "An invader approaches" notice (port overlay)

- **Data.** `bbcoop_invasion_view` returns: role (target or invader), phase (approaching with
  seconds left, active, ended), the other side's character name and the end reason. Implement
  it in `coop.cpp` from `Coop::InvadedBy()` / `Invading()`.
- **Draw.** Copy the `SkipVote` pattern in `bbport_overlay.cpp`:
  - namespace at `:4571-4598`: `Read()` from `Visible()` (`:6278-6305`), `Draw()` from
    `Render()` (`:6388-6440`);
  - the centred line uses `Party::Text(draw, sans_font, S(18..24), ...)`;
  - add `Invasion::shown` to `Visible()`'s return and to the `MenuArt::Load()` condition
    (`:6422`).
- **Texts:**
  - target: "An invader approaches" with a countdown, then "Invaded by `<name>`", then
    "Invader defeated" or "You were slain by the invader";
  - invader: "Invading `<host>`'s world", then "Returning home".
  Use the same red as party "down" text (`Party::Blood`, `:3832`).
- **Optional native banner.** The duels (`bloodborne_duel_game.cpp`) queue `SetTextEffect`
  (`SetTextEffectOffset`) with a text effect id (the ids are listed by it in `bloodborne_party.h`; 17 is NIGHTMARE
  SLAIN, not a free PvP banner). **[guess]** The game has native "invaded" / "adversary
  defeated" text effects. Phase 0 can record which id retail shows (section "Optional" in
  PHASE0).
- **Emulator.** shadPS4 has no overlay. There, use log lines and the native banner only.

Estimate: **1.5 d**.

---

## 11. Smaller items

- `TeamJoin` must not open or join anything while `Invading()`. `TickTeamJoin`'s `!in_team`
  return already does that; add an assert-style log. 0.25 d.
- The invader's party death: its board is empty, so `armed` is false and its deaths are real
  **[verified]** (`TickPartyDeath` `:1474-1478`). No change. 0 d.
- `ReconcileMemberPresence` on the invader's own screen shows everyone, because there is no
  fresh board (`bloodborne_re.cpp:745-757`). That is right. **[verified]** 0 d.
- Autoplay verbs for the rig: `invade [<team>]`, `wait-invasion <phase> [<timeout>]` and
  `expect-invader <0|1>` in `bloodborne_autoplay.cpp` (`AutoplayVerbs`, `:1936`), plus the
  matching `check_party_log.py` patterns. 1.5 d.
- Rig scenarios on the port: `invade-join`, `invade-hits`, `invade-kill-host`,
  `invade-invader-dies`, `invade-boss-end`, `invade-crash` in `tools/rig/scenarios.py`. They
  need a 3rd role as invader, playing alone (`team_prelude` gives `team-alone` only when there
  is one role, `tools/rig/run_scenario.py:840`). Add a per-scenario `"solo": ["guest2"]`. 2 d.

---

## 12. Emulator (shadPS4) differences

- **No server world.** Everything in sections 1, 2, 4, 9a and 10 that rides on
  `bbcoop::Envelope` is compiled out (`server_world_switch` false). shadPS4 players use bells,
  signs, P2P and the HTTP boards.
- **On shadPS4 an invasion is the retail one.** The Sinister bell posts a SummonType 2 sign and
  the broker matches it (`server/src/bloodborne_summon_broker.cpp:80-130`). Public signs match
  only without a party key. The same co-op-layer exclusions (section 4) and patch-site branches
  (section 6) still matter there, keyed on the **native** type and team instead of server
  messages.
- **Bells are not stripped** on shadPS4 (`RemoveBellItems` runs from `TickTrades`, which needs
  `TradeSwitchOn() = trade_switch && ServerWorldOn()`, `bloodborne_trade.cpp:261`).
- **Recommendation:** ship invasions **port-only** (Kaniel: port and shadPS4 players never share
  a party, `docs/BBPORT.md`). Keep shadPS4 as the RE bench (phase 0). For shadPS4 itself, only
  make sure the shared changes do not break its parties: the rig's emulator scenarios
  `join`, `host-death`, `guest-death`.

---

## 13. Estimate summary

| # | Item | Days |
|---|---|---|
| 1 | Invasion state, messages, ticks | 2 |
| 2 | Native join as invader (travel, join, place) | 3 (+2 guess) |
| 3 | Fallback SetChrType + team 21 everywhere | 3 (+3 guess), only if phase 0 says so |
| 4 | Co-op layers skip the hostile member | 6 |
| 5 | Kill attribution, real death | 2 (+1 variant) |
| 6 | Phantom-keyed patch sites | 3 (+2 guess) |
| 7 | Invader goes home | 2 |
| 8 | Save hygiene | 3 |
| 9a | Menu "Invade" and the leader toggle | 2 |
| 9b | Sinister bell (optional) | 1.5 (+1) |
| 10 | Notice UI | 1.5 |
| 11 | Small items, autoplay verbs, rig scenarios | 4 |

- **Native path (phase 0 good):** about **28.5 d** base; **~33 d** with the guesses landing
  badly. Without 9b: ~27 d.
- **Fallback path:** add **3-6 d**, about **32-39 d**.
- **In weeks:** about **6-8 weeks** of game-side work for one developer, phases 1 and 2. This
  fits the research report's 7-11 weeks total once the server work runs in parallel. Rewards,
  covenants and the Bell-Ringing Woman (the research report's phase 3) are not included.

## 14. Biggest risks (game side)

1. **The native kind/sign may not give a hostile phantom through `JoinTeamSession`** (phase 0).
   The fallback works but touches more patch sites.
2. **Hit acceptance:** 0x14 from team 21 to 19/20 on the victim's game. It is proven only for
   revive hits on downed bodies (`DownedTeams`).
3. **The ch 0x10 join snapshot writes host flags into the invader's world.** Section 8
   restores them. A crash between join and snapshot-write would lose the save's flags, so write
   the snapshot first.
4. **The invader's death path:** which native script runs (PartyGhostDeath vs SoloPlayDeath vs
   leave limit) with our patches on. If it is wrong, the invader is left lying dead or stuck in
   the host's world.
5. **Slot limit:** a full team of 4 plus an invader **[guess: impossible]**. Cap invadable
   teams at 3 until phase 0 tests it.

---

## 15. Server interface as built (invasions-server, branch `hs-invasions`, `docs/INVASIONS.md`)

This replaces the provisional names used above. The proto is append-only; PROTOCOL stays 7.

### Envelope fields

| Field | Message | Direction | Game use |
|---|---|---|---|
| 41 | `InvadeRequest{level=1, cancel=2}` | game to server | section 9 ("Invade" / "Leave the invasion") |
| 42 | `InvasionNotice{id, in_ms, invader, target}` | to both sides, 10 s before the join | the "An invader approaches" countdown (section 10) |
| 43 | `InvasionStart{id, session (TeamSession), target, map, at (Position), owners (EnemyOwners)}` | to the invader | sections 2 and 4.12. Later EnemyOwners changes for that world also reach the invader. |
| 44 | `InvaderJoined{id, member (board name), target, name}` | to the team | fills `InvadedBy()` |
| 45 | `InvasionEnd{id, reason, invader, text}` | to both sides | sections 7 and 10 |

### Other messages and fields

- **InvasionEnd reasons:**
  - `INVADER_DIED` 1, `TARGET_KILLED` 2, `PARTY_WIPED` 3, `BOSS` 4, `TIMEOUT` 5;
  - `INVADER_LEFT` 6, `HOST_LEFT` 7, `INVADERS_OFF` 8, `NOT_FOUND` 9, `REFUSED` 10.
- **Board:** `VitalsBoard.Member.hostile = 4` carries the invader's line. This is design A in
  section 0, so presence and session names work with no change. Wipes never count that line.
- **Sign:** config `InvasionSign`, default 4. If a member already holds that byte, the server
  takes the first free byte from 4 down to 1. It never uses 7/6/5.
- **New Vitals fields the game should fill:**
  - `killed_by_invader = 16` (section 5 step 7);
  - `level = 14`, the character level, for the level band;
  - `boss = 15`, true while a boss fight runs. The server uses it to skip matching and to end an
    invasion with `BOSS`.
  Fill all three in `PostVitals` and in the Vitals encoder in `bloodborne_server_world.cpp`.
- **Team opt-in:** `TeamRequest` action `INVADERS = 12` with `allow = 7` (leader only). The
  current setting shows as `Team.allow_invaders = 8` (section 9a).
- **Frames:** the hub drops every SHAD frame other than game packets between the invader and
  the team, in both directions. The game-side guards in sections 4.11 and 8 are backup only.
