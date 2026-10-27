# Invasion reward server extension — 2026-10-27

The complete server source and all four local server changes are now included under
[`server/`](../server/LOCAL_FORK.md), with the original patches, licenses, source manifest
and ARM64 Docker build. The deployment records below identify the running source snapshot.

Backend authority: [Kaniel / lKaniel's bloodborne-server](https://github.com/lKaniel/bloodborne-server)
at `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`. Original implementation credits include
Vitalii Shevchuk and shadNet contributors. The server changes live in a separate source
checkout, not in the native Mac client. Both server commits cite original authors and pinned
source URLs.

- Server branch: `codex/invasion-victory-rewards`, head
  `fceb97debb950b94df220c9963ccced4ad31fa2a` (preceded by `4201408d919f9787af43a075068947a44e95a231`).
- Reviewed source on `opc`: `/docker/bloodborne-server/invasion-rewards-reviewed`.
- Durable source bundle and patch on `opc`:
  `/docker/bloodborne-server/invasion-rewards-fceb97de.bundle` and `.patch`.
- Deployed image: `bloodborne-server:invasion-rewards-fceb97d-arm64`, digest
  `sha256:8125227ca4b43b16e6ae7866c85b0beae13b834bced47e05c8c269029bf9c162`.
- Deployment/rollback evidence:
  `/docker/bloodborne-server/rollbacks/invasion-rewards-fceb97de/deployment-result.json`.
- Rollback: `python3 /docker/bloodborne-server/invasion-rewards-deploy.py --rollback`.

The active compose file is `/docker/docker-compose.yml`. Only the Bloodborne service's image
changed; the normalized configuration comparison and the unchanged IDs of all other containers
are recorded in the deployment evidence. The original image and exact compose backup are
retained. Existing data, credentials, settings, mounts and ports were preserved.

The server tests passed 10/10, including the service/world-writer and trade tests. Meaningful
reward cases cover both victories, no reward during notice or non-victory ends, duplicate end,
solo delivery without a party Hello, mailbox persistence/counters across restart, capability
negotiation and legacy reconnect, authenticated mailbox isolation, acknowledgements/resume,
and rejection of forged reward items even through refund paths.

The optional capability is `LobbyHello.invasion_rewards` (field 5). Existing protocol numbers
remain unchanged. Reward grants have a high-bit ID and reason 2; their only new item kind is
Insight (4), restricted to one point in a validated reward. The server stores them under a
private mailbox derived from the authenticated online identity. The native client applies
Insight using the game's effect 4680, and Echoes through its original AddSouls function,
then acknowledges through the lobby stream.

The explicit server economy is one Insight and `floor(cost * percent / 100)` Echoes, where
`cost = floor((2*L^3 + 306*L^2 + 10560*L - 89500) / 100)`, `L` is the defeated player's level,
and percent is 10 for the invading winner or 30 for the defending host. Levels outside 10–544
receive Insight only. The level is frozen for the invasion. This policy matches the retained
level-10/30 fixtures; it is not a retail-economy certification. The Bell-Ringing Woman is
outside this extension. Covenant rewards and durable payout delivery were added below.

See [the gameplay validation record](SEAMLESS_COOP_VALIDATION.md) for evidence from the deployed
server and native Mac clients. The initial fceb97de delivery used separate inventory/ACK
operations; the crash window identified there is addressed by the subsequent implementation.

## Covenants and durable delivery

This follow-up's server commits are `ebbef91358246c860bee80f03393fa56de2810a6` (covenants)
and `9e60cec70e589a199cf86771de4844fe556562d4` (durable mailbox publication). The deployed image is
`bloodborne-server:atomic-rewards-9e60cec7-arm64`, digest
`sha256:b3bac7c7fbbf83a8da36b44ea5daf32d8444cacf5d3d619d504891f48f392848`.
Source bundle, patch, reviewed checkout and test log are retained on `opc` under
`/docker/bloodborne-server/atomic-rewards-9e60cec7.bundle`, `.patch`,
`atomic-rewards-reviewed-9e60cec7` and `atomic-rewards-9e60cec7-build-test.log`.
Deployment evidence is `/docker/bloodborne-server/rollbacks/atomic-rewards-9e60cec7/deployment-result.json`.
The rollback helper is `/docker/bloodborne-server/atomic-rewards-deploy-9e60cec7.py --rollback`;
it requires zero players and checks old-loader mailbox compatibility before reverting.
Only the Bloodborne image changed. Configuration/link-key hashes and all 52 unrelated
container IDs matched before and after deployment.

Server grants and acknowledgements are staged in a copied ledger. A synchronous completion
task passes through the same FIFO world-writer queue as prior writes; the file and ancestor
directories are durably flushed before the new ledger becomes observable or its grant is sent.
Failed persistence retains the prior ledger/counter and retries the eligible victory without
publishing a payout. Mailbox ledgers restore independently of optional world metadata; corrupt
present ledgers fail closed rather than resetting their counters. Tests inspect the durable
file inside the delivery callback, cover failed writes/ACKs and older queued snapshots, and
restart from a trades-only mailbox. All ten focused ARM64 server test targets passed.
Active invasion records remain memory-only: a server crash before the reward ledger commits
can end an unpaid match. The payout guarantee starts with that durable ledger commit.
Client-reported oath, level and death data are authenticated/frozen/whitelisted, not an
anti-cheat authority; physical power-cut testing and Windows directory-flush behavior were
not exercised.

The original 1.09 ItemLotParam rows 5500 and 5510 (Vileblood-oath PvP host/guest reward) each
contain exactly one guaranteed goods 704, Blood Dreg. The native award branches at 0x013830C8
and 0x01382B08 select those lots for effect 6100. Radiance rows 5600/5610, Hunter 5700/5710
and blood-addled Hunter 5800/5810 contain no enabled item. GemeffectParam 2108001 supplies
Corruption marker 6100; 2107001/2110001 supply 6110/6120. The server therefore adds only one
Blood Dreg for a Corruption winner, with the winner's oath frozen for the visit. No item is
invented for an empty native reward lot.

Sources are the player's own unmodified FromSoftware 1.09 parameters/events/executable,
[Paramdex definitions](https://github.com/soulsmods/Paramdex/tree/292ac5c10ace71116466cf8cd99c7dc034ef7602/BB/Defs)
and the retained Kaniel/lKaniel co-op implementation. Covenant metadata is appended to
Vitals (19) and InvadeRequest (3); LobbyHello.covenant_rewards (6) opts in to the exact
three-item reward shape. Legacy reward clients retain the prior shape and cannot consume
a pending covenant grant partially. Ordinary offers/refunds cannot mint a reward.

The native repository is the inventory pointer at PlayerGameData +0x5B0, using the same
16-byte inventory entry layout as held goods at +0x328. Native overflow and the original
Goods704 definitions establish 99 held / 99 stored. The client checks the actual total after
awarding; a fully occupied total of 198 defers the entire grant until capacity is available.

The verified native idle predicate at 0x0156AA10 checks GameMan +0x1554. Fresh character save
0x0156D570(-1, false, false), also called by the native wrapper at 0x01C2A9F0, synchronously
serializes current data through 0x0156AA50 before queuing a 0x140000-byte character image.
The client verifies native entry bytes and the current slot before using this path. Native
SaveRequest research by [vladtepes22/NightwatchCoop](https://github.com/vladtepes22/NightwatchCoop/blob/db7c9fb2f07a007a9bb53d3056a425c887c9d4b3/engine/gpu/shim/party/game_state.cpp)
guided the independently checked sites; no Nightwatch implementation is copied.

Before applying a payout, the runtime pins the old canonical save. The client applies once
and observes each native Echo/Insight/Dreg update before arming the fresh save. Native write
mounts use a cloned online save directory. Only a fully written, closed, correctly sized/versioned
character file can publish its matching receipt; profile-only or partial writes cannot.
The same postimage replaces the native backup for that character. All files/receipts and
directories are flushed, then macOS RENAME_SWAP publishes the directory in one step. Only
durable receipt lookup releases the server ACK. Subsequent online native saves retain those
receipts and use the same directory transaction, preserving the payout through later crashes.

The directory transaction extends [Kaniel's atomic temporary-close save foundation](https://github.com/lKaniel/bloodborne-server/blob/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/src/runtime_file.c)
and Mark Hall's native Mac runtime. It retains per-server save isolation, uses a profile lock,
rejects linked write paths, validates the complete bounded grant payload and records the
credited slot. Receipts are bounded to 4096 entries per profile; reaching this limit defers
further payouts without acknowledging or losing them. Duplicate IDs with different payloads
fail closed. Abandoned stages are removed
only after acquiring that profile's lock. Receipt removal or external save rollback/editing,
filesystem/storage that cannot honor atomic exchange/durable flush, and malicious local
modification are outside the guarantee. Offline save paths retain their existing behavior.
