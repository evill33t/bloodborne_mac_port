# Phase 0: capture a native Sinister Resonant Bell invasion on shadPS4

Goal: replace the research report's guesses with facts. Report:
`/mnt/project-files/research/hosted-server/research-invasions.md`, section "Main technical risks"
item 1. Plan that consumes the results: `GAME_SIDE_PLAN.md`.

To record, for a native invasion:

| # | What | Where it lives | Why |
|---|---|---|---|
| R1 | summon **kind** | GameMan (=GlobalState) `+0x16F0`, u32, on the invader | `InvasionStart.kind` |
| R2 | **sign / slot byte** | GameMan `+0x16F4`, u8, on the invader; sent on ch 0x42 | `InvasionStart.sign`, free-slot rule |
| R3 | invader **character type** | ChrIns `+0x78`, on the invader (local) and on the host (its copy) | `InvaderCharacterType`, patch-site keys |
| R4 | invader **team** | ChrIns `+0x88`, both sides; host is 19, guests 20 | confirms 21, hit acceptance |
| R5 | **Lua events** that run | the native event dispatch, see step 2c | which death and leave scripts run for a black phantom |
| R6 | **slot use** | host's session members (`SessionMembers()`), GameDataMan remote slots, ch 0x42 bytes | can a full team of 4 take an invader? |
| R7 | **what ends it**, and how the invader goes home | traces, logs, the invader's map and save after it | the `InvaderGoHome` design |
| R8 | does the broker match SummonType 2 as-is | `captures/bloodborne-summon/*` request and response bodies | server matching |
| R9 | effects on each side | 9000/9001/9003 (host bell), 9020 (invasion allowed), 9025 (Sinister) | gating |

shadPS4 is the right bench: it still has bells, signs, the HTTP summon broker and P2P. In
`emulator/src/core/bloodborne_server_world.cpp:48-53` the server world is compiled out on
shadPS4, so the native path is what runs.

---

## 1. Existing tooling (verified in the repo)

| Tool | Switch | Output |
|---|---|---|
| RE tracer: byte-verified observers on the SOS, summon, matching and warp sites (`emulator/src/core/bloodborne_re_sites.h`, e.g. `OnEvent.CallBlackSOS.Dispatch` 0x01389A6B, `OnEvent.CallDragonewtSOS.Dispatch` 0x01389A9B, `Call.BlackSos.Dispatch` 0x01389B22, `SosStatus.Update` 0x01872360; see `Sites`, and `CSMultiPlayMan.StopRequest`, `OnMatchingCheck`, `ForceSummonSuccess`, `CSMultiPlayerIns.*`) | `SHADPS4_BLOODBORNE_RE_TRACE=1` (`bloodborne_re_tracer.cpp:1848`) | JSONL in `<user>/captures/bloodborne-re/` (`bloodborne_re_tracer.cpp:1872`) |
| Summon HTTP capture: every `/summon_messenger/*` request and response | `SHADPS4_CAPTURE_BLOODBORNE_SUMMON=1` (`emulator/src/core/libraries/network/http.cpp:729`) | `<user>/captures/bloodborne-summon/<stem>-request.bin`, `-response.bin`, `.txt` |
| Native packet trace: every game packet in and out, with payloads (NATIVE_NETCODE.md "Phase 0 trace") | `SHADPS4_BLOODBORNE_PACKET_TRACE=1`, or `full` for every payload (`bloodborne_patches.cpp:445`) | `net-out`/`net-in` records in the co-op trace |
| Co-op trace | on unless `SHADPS4_BLOODBORNE_TRACE` turns it off (`bloodborne_party.cpp:3370`) | `<user>/log/coop-trace-*.txt` (`bloodborne_trace_diag.cpp:255-310`) |
| Packet summary | | `py -3.12 tools\re\packet_summary.py <coop-trace files> [--payloads CH] [--limit N]` |
| Change logs already in the game log | always | "Bloodborne other players: [type=.. hp=.. dead=..]" (`bloodborne_trace_diag.cpp:68`), local effects (`:94`), map changes, "Co-op lifecycle" lines |
| Lua (event script) disassembler | offline | `py -3.12 tools\re\lua50dis.py <eventcommon.luabnd.dcx> [function regex...]` |
| Autoplay verbs (`bloodborne_autoplay.cpp:1936`) | `SHADPS4_BLOODBORNE_AUTOPLAY=<script>`; the rig sets it | `add-effect <id>`, `peek <self\|handle> <hexoff>`, `dump <target>`, `kill self`, `travel <warp>`, `wait-session`, `wait-solo`, `expect-effect`, `snapshot`, `census`, `log` |
| Rig | `py -3.12 tools\rig\run_scenario.py <scenario>` (emulator is the default engine, `run_scenario.py:94`) | `rig/runs/<when>/<scenario>/` with each role's game log and the server logs |

The two-client manual setup, if the rig is not used, is in
`emulator/documents/bloodborne-seamless-re.md` "Runtime capture" (`:742-770`):
- set `SHADPS4_BLOODBORNE_RE_TRACE=1`, `SHADPS4_CAPTURE_BLOODBORNE_SUMMON=1` and
  `SHADPS4_BLOODBORNE_SEAMLESS_COOP=1` in both processes;
- give each its own `--user-id`, `SHADPS4_P2P_PORT` and `--cache-dir`;
- run shadNet with `BloodborneSeamlessCoop=true`.

---

## 2. Small additions needed first (about 1 day of code, diagnostics only)

None of the existing logs record R1, R2, R4 or R5. Add these on a branch; they run only with the
switch on.

**2a. `LogInvasionProbe()`.** Put it in `bloodborne_trace_diag.cpp` and call it from
`TickPartyController` (`bloodborne_re.cpp:972`), next to `LogOtherPlayersChanges()` (outside the
`if (party_controller)` block, so it runs with no party). Gate it on
`SHADPS4_BLOODBORNE_INVASION_PROBE=1`. It logs **on change**, one line, prefix
`Bloodborne invasion probe:`, with:
- `GetGlobalState()+0x16F0` (u32) and `+0x16F4` (u8);
- matching `GetMatchingState()+0x124` and `+0x120`;
- `SessionMembers()`, `SessionPlayers()` (`bloodborne_re.cpp:480-495`);
- the local player's `+0x78`, `+0x88` and `+0x08` handle;
- `[type, team, handle, hp]` for every `OtherPlayers()` entry (`bloodborne_re.cpp:666`);
- `HasPlayerEffect(player, id)` for 9000, 9001, 9003, 9005, 9020, 9025;
- event flags 4000 and 4047 (`EventFlagOn`);
- `GetCurrentPackedMap()`.

Also write it to the co-op trace with `Trace("invasion-probe", {...})`, so it lines up with the
packet records.

**2b. Team in the existing line.** In `LogOtherPlayersChanges`
(`bloodborne_trace_diag.cpp:82`), add `team={}` (`ReadValue<s32>(player, 0x88)`) to the
`[type=..]` bracket. This is a one-line change.

**2c. Lua events (R5).** There is no Lua event tracer today. Add a trace site to `Sites`
(`bloodborne_re_sites.h`) on the generic game-event dispatch **0x01317110**. That is the path
`Call_WhiteSos` uses (`emulator/documents/bloodborne-seamless-re.md:672-683`). Check its
prologue bytes in the eboot first and copy them into the site (the tracer refuses a mismatch).
Log `rdi`, `rsi`, `edx` and the return address. **[guess]** Which register holds the event id
must be read from the disassembly. Fallback if that is too slow, three passive hooks:
- `PartyGhostCustomLuaCallStartOffset` 0x0132ED40 (`bloodborne_party.h:101`);
- `SoloPlayDeathRespawnOffset` 0x01381DE0;
- `GhostDeathReviveOffset` 0x01382354 (both in `bloodborne_re_sites.h`).
Those three show whether a black phantom's death runs PartyGhostDeath or SoloPlayDeath.

**2d. Offline, no game run.** On the PC:

```
py -3.12 tools\re\lua50dis.py "<game>\dvdroot_ps4\script\eventcommon.luabnd.dcx" "OnJoinMutiplay|BlackGhost|Dragonewt|GhostDeath|HostDead|OnLeave|BlockClear|Invade|Sinister|9240|9020"
```

Read which branches test the summon kind (`+0x16F0`), the character type and team. That gives
the expected values before the capture.

Build: `tools\build-emu.ps1`, then `tools\rig-setup.ps1 -Trace -Force` (or keep the rig and copy
the new `shadPS4.exe`).

---

## 3. Rig preparation

1. `tools\rig-setup.ps1 -Trace` (once; `C:\bb\rig`; shadNet with `BloodborneSeamlessCoop=true`).
2. `py -3.12 tools\rig\run_scenario.py --add-guest2` (a 3rd game, for the slot test).
3. `py -3.12 tools\rig\run_scenario.py setup` then `--save-baseline` (every role at the first
   Central Yharnam lamp, `tools/rig/scenarios.py:256`).
4. **Why the invasion runs with no party.** The rig gives every game
   `SHADPS4_BLOODBORNE_PARTY=rig:hunters` and a host/guest role (`run_scenario.py:885-893`).
   - A party controller auto-rings the Beckoning or Small Resonant bell
     (`bloodborne_re.cpp:358-378`).
   - The emulator's HTTP adds the party header from that variable (`http.cpp:837`).
   - The broker matches public signs only when **neither** side has a party key, so a keyed
     host never finds a public Sinister sign.
   - The scenario therefore overrides both with its `"settings"`, applied last
     (`run_scenario.py:896`): `SHADPS4_BLOODBORNE_PARTY=""` and
     `SHADPS4_BLOODBORNE_PARTY_ROLE="none"`. `InitPartyController` then logs "auto-rejoin
     disabled" and no controller exists, which is the retail behaviour. The seamless byte patches
     still go in (`SHADPS4_BLOODBORNE_SEAMLESS_COOP=1`). Run 3b covers them.

---

## 4. The scenario to write: `invade-native` (sketch for `tools/rig/scenarios.py`)

- Respect the budget rules (`run_scenario.py` docstring): timeout of at most 300 s, no fixed
  wait over 30 s.
- `add-effect` applies a SpEffect through the player's own AddSpEffect, the same call the party
  bell uses (`StartPlayerSummonSearch`, `bloodborne_join.cpp:730`).
- **[guess]** Effect 9025 alone makes the sign poster post a SummonType 2 sign. The poster reads
  SpecialState 188-190, `bloodborne-seamless-re.md:640-660`. If no `/summon_messenger/create`
  with `SummonType: 2` shows in the capture, use run 3c (by hand, with the real item).

```python
SINISTER_BELL = 9025      # Sinister Resonant Bell's SpEffect (SpecialState 190)
BECKONING_BELL = 9000     # Beckoning Bell's SpEffect (SpecialState 189)
SMALL_RESONANT_BELL = 9005
INVASION_ALLOWED = 9020   # common event 9240's effect
NO_PARTY = {"SHADPS4_BLOODBORNE_PARTY": "", "SHADPS4_BLOODBORNE_PARTY_ROLE": "none",
            "SHADPS4_CAPTURE_BLOODBORNE_SUMMON": "1",
            "SHADPS4_BLOODBORNE_PACKET_TRACE": "full",
            "SHADPS4_BLOODBORNE_INVASION_PROBE": "1"}

SCENARIOS["invade-native"] = {
    "about": "phase 0 (invasions): the guest rings the Sinister Resonant Bell (9025), the host "
             "the Beckoning Bell (9000); no party key on either side. Records the invader's "
             "GameMan +0x16F0/+0x16F4, its type (+0x78) and team (+0x88) on both games, then the "
             "invader dies and goes home",
    "settings": NO_PARTY,
    "roles": {
        "host": [START, "snapshot", "peek self 78", "peek self 88",
                 f"add-effect {BECKONING_BELL}",
                 "wait-session 2 240",           # the invader is in
                 "wait 10", "census 40",         # every character near: type, team, handle
                 "snapshot", "dump nearest",     # the invader's ChrIns as the host's copy
                 "sync invaded",
                 "wait 20", "snapshot",
                 "wait-solo 120",                # the session ends (the invader died)
                 "wait 10", "snapshot"],
        "guest": [START, "snapshot", "peek self 78", "peek self 88",
                  "wait 20",                     # the host's bell is up first
                  f"add-effect {SINISTER_BELL}",
                  "wait-session 2 240",
                  "wait 10", "snapshot", "peek self 78", "peek self 88", "dump self",
                  "sync invaded",
                  "wait 10", "kill self",        # end 1: the invader dies
                  "wait-solo 120", "wait 20",
                  "snapshot", "peek self 78", "peek self 88"],
    },
    "syncs": {"invaded": {"chapter": "invade-native", "roles": ["host", "guest"]}},
    "expect": {"host": [r"Bloodborne invasion probe:"],
               "guest": [r"Bloodborne invasion probe:",
                         r"Captured Bloodborne summon exchange"]},
    "timeout": 300,
}
```

**Variants.** Copy the scenario and change only the ending. Each one is a separate run, because
every run starts from the baseline.

| Variant | Change | Answers |
|---|---|---|
| `invade-native-host-dies` | the host does `kill self` after `sync invaded`; the guest waits with `wait-solo 120` | R7: does the invader go home when its host dies (HostDead branch, `bloodborne_patch_sites.h:210` is **patched** in this build) |
| `invade-native-host-travels` | the host does `travel 2402950` (Cathedral Ward lamp) after the sync | R7: host leaves the map |
| `invade-native-blank` | the host does `add-effect 7` (Silencing Blank's SpEffect, `bloodborne-seamless-re.md` goods table) | R7: Silencing Blank **[guess]** that the effect alone is the blank's action |
| `invade-native-gate` | before the bell: the host does `expect-effect 9020 1` | R9: needs "invasion allowed". If it fails, add `add-effect 9020` to the host and record that the gate was forced |
| `invade-native-slots` | add `guest2` as a **co-op** guest: `add-effect 9005` first, then the host's `wait-session 2`. The invader rings after; the host waits with `wait-session 3` | R6: host + co-op guest + invader. Read the 0x42 slot bytes. A team of 4 plus an invader needs a 4th account and role (none in `engines.ACCOUNTS`, `tools/rig/engines.py:24`); mark it untested |
| `invade-native-timeout` | no kill; both `wait 30` repeated to 280 s | R7: does retail time it out (expect no end within the budget; repeat by hand for 10 min) |

Run: `py -3.12 tools\rig\run_scenario.py invade-native invade-native-host-dies invade-native-gate invade-native-slots`

---

## 5. Runs and what to read

- **3a.** Scripted, as above. Read every role's game log, co-op trace and captures from
  `C:\bb\rig\runs\<when>\invade-native\` (plus each role's `user\captures\`).
- **3b.** Patches compared. The rig always sets `SHADPS4_BLOODBORNE_SEAMLESS_COOP=1`, so 3a runs
  with the ghost-death, host-death and boss-clear patches on. For the retail end (R7), repeat
  `invade-native` with `"SHADPS4_BLOODBORNE_SEAMLESS_COOP": "0"` in its settings. **[guess]**
  The summon HTTP still reaches shadNet with it off, since `http.cpp` routes `/summon_messenger`
  by itself. If the bell is refused in that area without the seamless area patches, ring in an
  area retail allows (Central Yharnam before its boss, which the rig saves have beaten). Use
  the manual run 3c on fresh saves.
- **3c.** By hand. Two instances per `bloodborne-seamless-re.md:742-770`, real items:
  - the host uses the Beckoning Bell and waits for the Bell-Ringing Woman;
  - the guest uses the Sinister Resonant Bell (bought for 1 Insight);
  - same env as `NO_PARTY` plus `SHADPS4_BLOODBORNE_RE_TRACE=1`.
  Do every ending by hand: invader killed by the host, host killed by the invader, Silencing
  Blank, Bell-Ringing Woman killed before arrival, boss fog entered, host to the Dream.

### Reading the results

| # | How |
|---|---|
| R1/R2 | The guest's log: `findstr /C:"invasion probe" shad_log.txt`. Look for the first line after the join with `16f0=` and `16f4=`. Cross-check R2 with `packet_summary.py <guest trace> --payloads 0x42` (the byte the invader sends) and the host's trace (the bytes it receives). |
| R3/R4 | The probe line (local and others) plus `peek self 78/88` lines ("Bloodborne autoplay peek: ...") on both sides; `census` gives the host's view of every character. Compare with the co-op guest in `invade-native-slots` (expect 1/20). |
| R5 | The new event-dispatch trace records in `captures/bloodborne-re/*.jsonl` (2c), around the join (`OnJoinMutiplay`) and the death; check them against the 2d disassembly. Note especially whether `GhostDeathRevive` 0x01382354 or `SoloPlayDeathRespawn` 0x01381DE0 fires on the invader's death. |
| R6 | `SessionMembers`/`SessionPlayers` in the probe; the 0x42 bytes from each member; any `OnSummonResult.*`/`ForceSummonFail` trace on a refused join. |
| R7 | The invader's log after death: does `CSMultiPlayMan.StopRequest` fire (trace), does the map change (probe `map=`), does its type go back to 0, is there a reload ("Co-op lifecycle")? Then on the invader's **save**: compare `snapshot` before and after (map, echoes), and `expect-flag` a few host-world flags the invader's save did not have (e.g. `GASCOIGNE_DEFEATED`, the opening `12410000`) to see whether the 0x10 snapshot leaked. |
| R8 | `captures/bloodborne-summon/*.txt` metadata, plus the bodies: the guest's `/create` (`SummonType: 2`), the host's `/get` `SummonTypeList` (does it list 2?), the host's `/request` (claim), the guest's next `/create` response (the claim delivery with the blob). Also the shadNet log's broker lines. |
| R9 | Effect sets in the probe and in "local effects" lines on both sides through the run. |
| Key items | `dump self` on the invader before and after the join, inventory `+0x58` (the key item list, `GiveBackPhantomKeyItems` `bloodborne_travel.cpp:1512`): does it change as for a white phantom? |
| Echoes | The invader's "Bloodborne echoes: +N" lines (`NoteAddSouls`, `bloodborne_party_rules.cpp:859`) while the host kills an enemy: is the native kill reward paid to an invader? Add `attack nearest` on the host after the sync in one variant. |
| Banner | Optional: a temporary passive hook on `SetTextEffect` (`SetTextEffectOffset`, used by the duels' `bloodborne_duel_game.cpp`) logging the text effect id on both sides gives the native "invaded" or "defeated" banner ids for the notice UI. |

## 6. Results template (fill in, attach to the PR or thread)

```
kind (+0x16F0)        invader: ____   co-op guest for reference: 0xFFFFFFFF
sign (+0x16F4 / 0x42) invader: ____   host: 0  co-op guests: 7/6/5
type (+0x78)          invader local: __  invader on host: __  (white phantom = 1)
team (+0x88)          invader local: __  invader on host: __  host: 19  co-op: 20
effects               host before ring: ____  invader: ____  9020 needed: y/n
broker                /get SummonTypeList: ____  matched as-is: y/n
death path            invader: GhostDeath / SoloPlayDeath / LeaveLimit / other: ____
end, invader dies     goes home by: ____  reload: y/n  save changed: ____
end, host dies        ____
end, host leaves map  ____
end, blank            ____
slots                 host+guest+invader: ok/refused   4+1: untested
key item list swap    y/n      echoes from host kills: y/n
```

Go/no-go:
- **Native** if a `JoinTeamSession` with the captured kind/sign gives type and team equal to
  the retail capture. Try that next on the port rig with a debug `InvasionStart`.
- **Otherwise the fallback**, `GAME_SIDE_PLAN.md` section 3.
