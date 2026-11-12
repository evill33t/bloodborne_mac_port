# Co-op reliable-packet delivery after a reload

The November 12 Mac 1.5.0 report described Central Yharnam enemies moving for the
host but standing still for the guest after both players died and reloaded. Player
movement and damage continued. The host also reported a missing remote weapon.

Private two-client reproductions on bloodborne.phobos.services isolated a reliable
stream blocked at sequence 419 by a 1,608-byte native enemy AI packet (channel 0x3D).
The guest held later reliable packets from sequence 420 onward while smaller position
packets continued arriving. Repeated resends did not repair the gap; one capture
exceeded 57,000 resends. Mac IP fragmentation counters increased. This establishes
an oversized-datagram delivery failure on the tested path; the precise intervening
router or cloud rule was not identified. No firewall rule was changed.

The client and server now negotiate an optional authenticated link-v4 extension.
Large Data datagrams are split into 1,120-byte pieces before encryption, keeping
each complete IP/UDP packet below 1,200 bytes. Each piece has its own authentication
and replay counter. Reassembly is per authenticated link, bounded to eight messages
and 256 KiB, and expires incomplete messages after five seconds. A new handshake
clears incomplete state. Only complete Data datagrams reach the existing team,
channel and enemy-authority checks. Native game packets and co-op protocol 9 stay
unchanged. Legacy clients receive the original Data format; older servers retain
the original transport when they do not advertise the extension.

Validation on November 12:

- The real GameLinkPeer regression reproduces reliable AI blocking later equipment
  updates when large UDP packets are lost, then checks recovery, ordering, acks,
  duplicate/reordered fragments, malformed sizes/indices, conflicts, quotas and expiry.
- Both shared-header copies pass the regression with AddressSanitizer and UBSan.
  The Mac hello/welcome extension and legacy fallback encoding pass separately.
- The Linux ARM64 server image passes all 21 configured tests, including negotiated
  encrypted reassembly, rejection of unauthenticated fragments, channel/team
  enforcement, legacy destinations and the existing handshake contracts.
- Isolated real UDP simulated clients pass with both dev and 1.5.0 release labels.
- Two Mac game processes using disposable base-game saves join, die together,
  reload Central Yharnam, retain both visible player records and resume enemy updates.
  The guest's actual unarmed hit changes enemy 0x10014066 from 225 to 222 HP in both
  copies. Enemy movement is tracked in both copies and the reliable backlog drains.
  This is a controlled same-Mac regression, not a long-session two-machine test.
- The optimized production module passes the runtime ABI fixture with default and
  overridden server settings. It disables the developer autoplay rig.

The tested server image is `bloodborne-server:v1.5.0-guest-sync-fix`, digest
`sha256:45f537686efa0a295df7d95ed3038fa1daf025fa926121662433cc72fb5b0e11`.
Deployment retained all 45 existing worlds, the pinned key and static configuration,
and unrelated containers. A closed-database backup and rollback configuration are
under `/docker/bloodborne-server/rollbacks/guest-sync-fix-20261112` on opc.
The original upstream baseline remains v1.5.0, 0bb87c1b80ca2dfc01d75931cc3cb55873c057e2.

The missing-weapon symptom is compatible with reliable equipment messages being
blocked behind such a gap. Its visual gameplay retest is recorded separately;
the packet-ordering fixture alone does not prove that every missing-weapon cause
is resolved.
