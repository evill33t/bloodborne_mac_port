# Invasion reward server extension — 2026-10-27

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
level-10/30 fixtures; it is not a retail-economy certification. Covenant-specific rewards and
the Bell-Ringing Woman are outside this extension.

See [the gameplay validation record](SEAMLESS_COOP_VALIDATION.md) for evidence from the deployed
server and native Mac clients. As with the reference trade path, inventory application and
the server acknowledgement are separate operations; these tests do not establish an atomic
exactly-once transaction across a crash between applying and acknowledging a grant.
