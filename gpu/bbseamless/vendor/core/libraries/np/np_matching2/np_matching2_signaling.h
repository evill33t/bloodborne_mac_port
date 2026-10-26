// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"

namespace Libraries::Np::NpMatching2 {

struct ContextObject;

// Phase 9: room members connect through the server only (no handshake, no STUN).
void StartMatching2ConnectThread();

// The room's peers with a live signaling link (network byte order), for seamless co-op's own
// frames between the games.
struct RoomPeerAddress {
    u32 addr = 0;
    u16 port = 0;
};
std::vector<RoomPeerAddress> GetActiveRoomPeers();
void StopMatching2ConnectThread();

void StartMatching2PeerConnect(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                               OrbisNpMatching2RoomMemberId member_id);
void StartMatching2SignalingForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id);
void QueueMatching2DeadForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                    s32 error_code);
void QueueMatching2SignalingEvent(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                  OrbisNpMatching2RoomMemberId member_id,
                                  OrbisNpMatching2Event event, s32 error_code);

u32 GetRoomPingUs(const ContextObject& ctx, OrbisNpMatching2RoomId roomId);
void* BuildSignalingGetPingInfoPayload(ContextObject& ctx, OrbisNpMatching2RoomId roomId);
s32 FillMatching2ConnectionInfo(const ContextObject& ctx, OrbisNpMatching2RoomId roomId,
                                OrbisNpMatching2RoomMemberId memberId, u32 infoType, void* connInfo,
                                bool a_variant);

} // namespace Libraries::Np::NpMatching2
