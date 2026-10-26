// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common/logging/log.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/server_link.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_signaling/np_signaling_stubs.h"

namespace Libraries::Np::NpMatching2 {

namespace {

constexpr s32 kMatching2ConnInactive = 0;
constexpr s32 kMatching2ConnPending = 1;
constexpr s32 kMatching2ConnActive = 2;
// Phase 9: a room member is connected through the server (its virtual address), never by a
// handshake between the games; one whose address the server does not know yet is asked again,
// and declared dead after a while.
constexpr auto kMatching2ConnectRetry = std::chrono::milliseconds(350);
constexpr auto kMatching2ConnectTimeout = std::chrono::seconds(10);

std::atomic<bool> g_matching2_stop{false};
// The room's peers with a live signaling link, as the connect thread last saw them.
std::mutex g_active_peers_mutex;
std::vector<RoomPeerAddress> g_active_peers;
std::mutex g_matching2_thread_mutex;
std::thread g_matching2_thread;

bool ShouldConnectToPeer(const RoomCache& room, OrbisNpMatching2RoomMemberId self,
                         OrbisNpMatching2RoomMemberId peer) {
    if (peer == 0 || peer == self) {
        return false;
    }
    if (room.signaling_type == ORBIS_NP_MATCHING2_SIGNALING_TYPE_NONE) {
        return false;
    }
    if (room.signaling_type == ORBIS_NP_MATCHING2_SIGNALING_TYPE_STAR &&
        room.signaling_main_member != 0) {
        return self == room.signaling_main_member || peer == room.signaling_main_member;
    }
    return true;
}

bool ResolvePeerEndpoint(const MemberCache& member, PeerInfo& peer) {
    if (peer.addr != 0 && peer.port != 0) {
        return true;
    }
    if (member.addr != 0 && member.port != 0) {
        peer.addr = member.addr;
        peer.port = member.port;
        if (peer.online_id.data[0] == 0) {
            peer.online_id = member.np_id.handle;
        }
        return true;
    }

    const std::string online_id(member.np_id.handle.data);
    u32 resolved_addr = 0;
    u16 resolved_port = 0;
    if (!online_id.empty() && RequestSignalingInfos(online_id, &resolved_addr, &resolved_port)) {
        peer.addr = resolved_addr;
        peer.port = resolved_port;
    }
    if (peer.online_id.data[0] == 0) {
        peer.online_id = member.np_id.handle;
    }
    return peer.addr != 0 && peer.port != 0;
}

void MarkMatching2PeerActive(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                             OrbisNpMatching2RoomMemberId member_id, u32 addr, u16 port) {
    if (member_id == 0 || member_id == ctx.my_member_id) {
        return;
    }

    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    if (addr != 0) {
        peer.addr = addr;
    }
    if (port != 0) {
        peer.port = port;
    }
    const bool first_active = peer.status != kMatching2ConnActive;
    peer.status = kMatching2ConnActive;
    peer.handshake_started = true;

    if (first_active && !peer.sent_established) {
        peer.sent_established = true;
        QueueMatching2SignalingEvent(ctx, room_id, member_id,
                                     ORBIS_NP_MATCHING2_SIGNALING_EVENT_ESTABLISHED, ORBIS_OK);
        LOG_INFO(Lib_NpMatching2,
                 "Matching2 signaling established: ctx={} room={} member={} addr={:#x}:{}",
                 ctx.ctx_id, room_id, member_id, peer.addr, Libraries::Net::sceNetNtohs(peer.port));
    }
}

// Connected once the server knows the member's address (net_link.h ConnectsWithoutHandshake).
bool ConnectMatching2Peer(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                          OrbisNpMatching2RoomMemberId member_id) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return false;
    }
    auto member_it = room_it->second.members.find(member_id);
    if (member_it == room_it->second.members.end()) {
        return false;
    }
    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    if (!Net::ConnectsPeersWithoutHandshake() || !ResolvePeerEndpoint(member_it->second, peer)) {
        return false;
    }
    MarkMatching2PeerActive(ctx, room_id, member_id, peer.addr, peer.port);
    return true;
}

void Matching2ConnectThreadMain() {
    while (!g_matching2_stop.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<RoomPeerAddress> active;
        for (u32 id = 1; id <= ContextManager::kMaxContexts; ++id) {
            ContextObject* ctx =
                ContextManager::Instance().Get(static_cast<OrbisNpMatching2ContextId>(id));
            if (!ctx || ctx->room_id == 0 ||
                ctx->room_cache.find(ctx->room_id) == ctx->room_cache.end()) {
                continue;
            }
            for (const auto& [member_id, peer] : ctx->peers) {
                if (peer.status == kMatching2ConnActive && peer.addr != 0 && peer.port != 0) {
                    active.push_back({peer.addr, peer.port});
                }
            }
            for (auto& [member_id, peer] : ctx->peers) {
                if (peer.status != kMatching2ConnPending || !peer.handshake_started) {
                    continue;
                }
                if (now - peer.first_try > kMatching2ConnectTimeout) {
                    peer.status = kMatching2ConnInactive;
                    QueueMatching2SignalingEvent(*ctx, ctx->room_id, member_id,
                                                 ORBIS_NP_MATCHING2_SIGNALING_EVENT_DEAD,
                                                 ORBIS_NP_MATCHING2_SIGNALING_ERROR_TIMEOUT);
                    continue;
                }
                if (now - peer.last_send >= kMatching2ConnectRetry) {
                    peer.last_send = now;
                    ConnectMatching2Peer(*ctx, ctx->room_id, member_id);
                }
            }
        }
        {
            std::scoped_lock lock{g_active_peers_mutex};
            g_active_peers = std::move(active);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

std::vector<RoomPeerAddress> GetActiveRoomPeers() {
    std::scoped_lock lock{g_active_peers_mutex};
    return g_active_peers;
}

void QueueMatching2SignalingEvent(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                  OrbisNpMatching2RoomMemberId member_id,
                                  OrbisNpMatching2Event event, s32 error_code) {
    PendingEvent ev{};
    ev.type = PendingEvent::SIGNALING_CB;
    ev.ctx_id = ctx.ctx_id;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.room_id = room_id;
    ev.member_id = member_id;
    ev.sig_event = event;
    ev.error_code = error_code;
    ScheduleEvent(std::move(ev));
}

void StartMatching2PeerConnect(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                               OrbisNpMatching2RoomMemberId member_id) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end() ||
        !ShouldConnectToPeer(room_it->second, ctx.my_member_id, member_id) ||
        room_it->second.members.find(member_id) == room_it->second.members.end()) {
        return;
    }

    Net::EnsureP2PTransport();

    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    if (peer.status == kMatching2ConnActive) {
        return;
    }
    if (ConnectMatching2Peer(ctx, room_id, member_id)) {
        return;
    }
    // Not yet: the connect thread asks again.
    const auto now = std::chrono::steady_clock::now();
    peer.status = kMatching2ConnPending;
    peer.handshake_started = true;
    peer.first_try = now;
    peer.last_send = now;
}

void StartMatching2SignalingForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id) {
    const auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return;
    }
    for (const auto& [member_id, member] : room_it->second.members) {
        StartMatching2PeerConnect(ctx, room_id, member_id);
    }
}

void QueueMatching2DeadForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                    s32 error_code) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return;
    }

    for (const auto& [member_id, member] : room_it->second.members) {
        if (member_id == 0 || member_id == ctx.my_member_id) {
            continue;
        }

        auto peer_it = ctx.peers.find(member_id);
        if (peer_it != ctx.peers.end()) {
            peer_it->second.status = kMatching2ConnInactive;
            peer_it->second.handshake_started = false;
        }

        QueueMatching2SignalingEvent(ctx, room_id, member_id,
                                     ORBIS_NP_MATCHING2_SIGNALING_EVENT_DEAD, error_code);
        LOG_INFO(Lib_NpMatching2, "Matching2 signaling dead: ctx={} room={} member={} reason={:#x}",
                 ctx.ctx_id, room_id, member_id, static_cast<u32>(error_code));
    }
}

void StartMatching2ConnectThread() {
    std::lock_guard lock(g_matching2_thread_mutex);
    if (g_matching2_thread.joinable()) {
        return;
    }
    g_matching2_stop.store(false, std::memory_order_relaxed);
    g_matching2_thread = std::thread(Matching2ConnectThreadMain);
}

void StopMatching2ConnectThread() {
    {
        std::lock_guard lock(g_matching2_thread_mutex);
        g_matching2_stop.store(true, std::memory_order_relaxed);
    }
    if (g_matching2_thread.joinable()) {
        g_matching2_thread.join();
    }
}

u32 GetRoomPingUs(const ContextObject& ctx, OrbisNpMatching2RoomId roomId) {
    const auto room_it = ctx.room_cache.find(roomId);
    if (room_it == ctx.room_cache.end()) {
        return 0;
    }

    u64 total_ping = 0;
    u32 ping_count = 0;
    for (const auto& [member_id, member] : room_it->second.members) {
        if (member_id == ctx.my_member_id) {
            continue;
        }
        const auto peer_it = ctx.peers.find(member_id);
        if (peer_it == ctx.peers.end() || peer_it->second.ping_us == 0) {
            continue;
        }
        total_ping += peer_it->second.ping_us;
        ++ping_count;
    }

    return ping_count == 0 ? 0 : static_cast<u32>(total_ping / ping_count);
}

void* BuildSignalingGetPingInfoPayload(ContextObject& ctx, OrbisNpMatching2RoomId roomId) {
    CallbackPayload& p =
        ctx.request_payload_override ? *ctx.request_payload_override : ctx.request_payload;
    p.Reset();

    const auto room_it = ctx.room_cache.find(roomId);
    p.ping_info_response = std::make_unique<OrbisNpMatching2SignalingGetPingInfoResponse>();
    auto& out = *p.ping_info_response;
    out = {};
    if (room_it != ctx.room_cache.end()) {
        out.serverId = room_it->second.server_id;
        out.worldId = room_it->second.world_id;
        out.roomId = room_it->second.room_id;
    } else {
        out.serverId = ctx.server_id;
        out.worldId = ctx.world_id;
        out.roomId = roomId;
    }
    out.rtt = GetRoomPingUs(ctx, roomId);

    p.request_data = p.ping_info_response.get();
    return p.request_data;
}

s32 FillMatching2ConnectionInfo(const ContextObject& ctx, OrbisNpMatching2RoomId roomId,
                                OrbisNpMatching2RoomMemberId memberId, u32 infoType, void* connInfo,
                                bool a_variant) {
    if (!connInfo) {
        LOG_ERROR(Lib_NpMatching2, "connInfo null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    const auto room_it = ctx.room_cache.find(roomId);
    if (room_it == ctx.room_cache.end()) {
        LOG_INFO(Lib_NpMatching2, "room={} not cached for connection info", roomId);
        if (a_variant) {
            *static_cast<OrbisNpMatching2SignalingConnectionInfoA*>(connInfo) = {};
        } else {
            *static_cast<OrbisNpMatching2SignalingConnectionInfo*>(connInfo) = {};
        }
        return ORBIS_OK;
    }

    const auto member_it = room_it->second.members.find(memberId);
    const MemberCache* member =
        member_it != room_it->second.members.end() ? &member_it->second : nullptr;
    const auto peer_it = ctx.peers.find(memberId);
    const PeerInfo* peer = peer_it != ctx.peers.end() ? &peer_it->second : nullptr;

    if (a_variant) {
        auto* out = static_cast<OrbisNpMatching2SignalingConnectionInfoA*>(connInfo);
        *out = {};
        switch (infoType) {
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_RTT:
            out->rtt = peer ? peer->ping_us : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_BANDWIDTH:
            out->bandwidth = 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDR:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_MAPPED_ADDR:
            if (peer) {
                out->address.addr = peer->addr;
                out->address.port = peer->port;
            } else if (member) {
                out->address.addr = member->addr;
                out->address.port = member->port;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PACKET_LOSS:
            out->packetLoss = 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDRESS_A:
            if (member) {
                out->peerAddrA.accountId = member->account_id;
                out->peerAddrA.platform = member->platform;
            }
            break;
        default:
            LOG_WARNING(Lib_NpMatching2, "unsupported connection info A type={}", infoType);
            return ORBIS_NP_MATCHING2_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
    } else {
        auto* out = static_cast<OrbisNpMatching2SignalingConnectionInfo*>(connInfo);
        *out = {};
        switch (infoType) {
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_RTT:
            out->rtt = peer ? peer->ping_us : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_BANDWIDTH:
            out->bandwidth = 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_NP_ID:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_NPID:
            if (member) {
                out->npId = member->np_id;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDR:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_MAPPED_ADDR:
            if (peer) {
                out->address.addr = peer->addr;
                out->address.port = peer->port;
            } else if (member) {
                out->address.addr = member->addr;
                out->address.port = member->port;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PACKET_LOSS:
            out->packetLoss = 0;
            break;
        default:
            LOG_WARNING(Lib_NpMatching2, "unsupported connection info type={}", infoType);
            return ORBIS_NP_MATCHING2_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
    }

    LOG_INFO(Lib_NpMatching2, "connection info{}: ctx={} room={} member={} type={} rtt={}",
             a_variant ? "A" : "", ctx.ctx_id, roomId, memberId, infoType,
             peer ? peer->ping_us : 0);
    return ORBIS_OK;
}

} // namespace Libraries::Np::NpMatching2
