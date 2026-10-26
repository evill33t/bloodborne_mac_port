// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Libraries::Np::NpSignaling {

// A connection this game deactivated is only kept for its 60 s keepalive linger before DEAD.
// A new activation in that window must start a new connection: reusing the lingering one sent
// the peer an activation on a connection its game had already deactivated, and that game never
// heard the invite (the host's second summon of the guest went nowhere). Whether the peer closed
// it too does not matter: a peer whose game died (a crash, the rig's guest-crash test) never
// does, and its restarted game only answers a new handshake.
inline bool LingeringConnectionIsFinished(bool established, bool locally_deactivated,
                                          [[maybe_unused]] bool peer_deactivated, bool dead_fired) {
    return established && locally_deactivated && !dead_fired;
}

} // namespace Libraries::Np::NpSignaling
