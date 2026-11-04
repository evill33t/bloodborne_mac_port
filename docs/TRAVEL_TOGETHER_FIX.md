# Travel Together fixes (2026-11-04)

The shared client can deadlock when receiving travel news overlaps answering or
finishing a vote. `bloodborne_server_world.cpp` receives under `stream_mutex` and
calls `TakeTeamTravelNews`, which previously took `travel_mutex`. Conversely,
`AnswerTeamTravel` and `TickTeamTravel` hold `travel_mutex` while `SendTravel`
queues an outgoing message under `stream_mutex`.

The bounded 32-message travel inbox now has its own mutex. The game thread drains
and releases that inbox before taking the travel state lock or sending messages.
The vote rules, timeout, message ordering and wire protocol are unchanged.
The same travel-only patch and regression are prepared for lKaniel's source
repository, based on `3c6efa7669e4ee13369283823ba9b201cc46f3a5`.
Published review branch: https://github.com/evill33t/bloodborne-server/tree/codex/fix-team-travel-deadlock.

Our Mac prompt also lacked the original explicit control handling and veto
notices. It now offers **With the team / Alone** for the initiating player and
**Yes / No** for a teammate's vote. Arrows/D-pad select, Enter/keypad Enter/Space
or Cross/A confirm, and Escape or Circle/B choose Alone/No. It ignores the
opening frame's press and sends one answer until the game updates its snapshot.
Waiting and veto notices do not capture game input. Lamp names come from the
player's localized game text. The co-op UI snapshot version is now **6**;
rebuild the game module and both GPU libraries/hosts together.

Validation:

- The compiled production travel-state-machine regression fails against the
  unmodified lKaniel source and passes with the inbox lock. It checks receive
  completion with the state lock held, the 32-message bound/order, a veto and an
  agreed trip. Game memory, clock and transport boundaries are adapters; this
  fixture does not reproduce a naturally occurring production deadlock.
- `python3 tests/test_team_travel_locking.py -v` passes against our imported
  source. Original imported hashes and the adapted source hash remain recorded.
- The real Metal/SDL travel-control fixture fails on the previous Escape path
  and passes after the fix: keyboard/controller confirmation, cancellation,
  selection, an already-held opening press, stale snapshots and input capture.
  The fixture and existing co-op presentation fixture pass with Metal validation.
- Release/LTO x86-64 game/client/GPU and arm64 GPU/hosts build successfully.
- Isolated two-client gameplay checks verified a veto kept both in Yharnam, then
  scripted keyboard/gamepad input sent both to Hunter's Dream. Both clients
  rendered the destination and retained their two-player session. Original save
  seed and all 56 monitored source game files remained unchanged. Logs and
  screenshots are retained in
  this worktree's `out/travel-fixed` and `out/travel-render`. They use copied
  saves and fresh accounts. No Windows or mixed-platform gameplay is claimed.

The user's original report did not identify exact reproduction steps. These are
confirmed defects found during investigation, not proof that either explains
every possible Travel Together failure.
