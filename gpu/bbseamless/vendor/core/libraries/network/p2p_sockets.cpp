// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <fcntl.h>
#endif
#ifdef __linux__
#include <sys/eventfd.h>
#endif

// clang-format off
#include "sockets.h"
// clang-format on
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/net_delay.h"
#include "core/libraries/network/net_firewall.h"
#include "core/libraries/network/net_link.h"
#include "core/libraries/network/net_silence.h"
#include "core/libraries/network/net_util.h"
#include "core/libraries/network/server_link.h"
#include "net.h"
#include "net_error.h"

namespace Libraries::Net {

namespace {

// Rig 2026-10-13: about 1 join in 4 stalls, the game giving up its per-peer handshake (state
// machine 0xcd8bb0) 5.4 s after signaling came up; the first game packets of the run are logged
// (size, first byte, time) to see which hello or ack never went or never came.
std::atomic<u32> join_packets_logged{0};
constexpr u32 JoinPacketsLogged = 3000;
// A stalled join's guest received the host's 271-byte message and never acted on it: the long
// ones have a budget of their own, and every line shows the first bytes.
constexpr size_t LongPacket = 200;

std::atomic<u32> long_packets_logged{0};

bool LogsPacket(size_t size) {
    if (size >= LongPacket) {
        return long_packets_logged.fetch_add(1) < JoinPacketsLogged;
    }
    return join_packets_logged.fetch_add(1) < JoinPacketsLogged;
}

std::string HeadHex(const u8* data, size_t size) {
    std::string text;
    for (size_t index = 0; index < std::min<size_t>(size, 48); ++index) {
        text += fmt::format("{:02x}", data[index]);
    }
    return text;
}

s64 P2PNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr u16 kSignalingVPortNbo = 0xffff;
// The server link's housekeeping (hello, ping, outage checks) runs this often on the receive
// thread; a refused hello is said again this often (a hello lost on the way is said every second).
constexpr s64 LinkTickMs = 100;
constexpr size_t kVPortHeaderSize = 4;
constexpr size_t kMaxUdpPayload = 65507;
constexpr size_t kMaxP2PPayload = kMaxUdpPayload - kVPortHeaderSize;

#ifdef _WIN32
constexpr net_socket kInvalidSocket = INVALID_SOCKET;
#else
constexpr net_socket kInvalidSocket = -1;
#endif

enum class InternalChannel {
    Signaling,
    Control,
    // Seamless co-op's own frames between the games ("SHAD" type 0x30: enemy damage).
    Coop,
    // The game's own packets in our frames ("SHAD" type 0x40, bloodborne_game_link.h).
    GamePackets,
    // Our messages with the server itself (the link's Coop kind: the party's world).
    ServerCoop,
    // Phase 11: the game's calls for shadNet's TCP port, tunnelled (the link's Rpc kind).
    ServerRpc,
};

struct QueuedPacket {
    u32 source_addr{};
    u16 source_port{};
    std::vector<u8> payload;
};

int FailWith(int error) {
    *Libraries::Kernel::__Error() = error;
    return -1;
}

bool IsValidSocket(net_socket socket) {
    return socket != kInvalidSocket;
}

int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool IsRetryableSocketError(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

void CloseSocket(net_socket& socket) {
    if (!IsValidSocket(socket)) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    socket = kInvalidSocket;
}

#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
// Windows reports an ICMP "port unreachable" for an earlier send (a peer whose game has gone) as
// WSAECONNRESET on the next receive of the same UDP socket: with a departed peer still in the
// room every read after a send to it failed. Off: a lost peer is the silence checks' business.
void DisableUdpConnectionReset(net_socket socket) {
    BOOL report = FALSE;
    DWORD returned = 0;
    if (::WSAIoctl(socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned,
                   nullptr, nullptr) != 0) {
        LOG_WARNING(Lib_Net, "P2P transport: SIO_UDP_CONNRESET off failed error={}",
                    LastSocketError());
    }
}
#endif

bool SetSocketNonBlocking(net_socket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    int enabled = 1;
    return ::ioctl(socket, FIONBIO, &enabled) == 0;
#endif
}

#ifdef _WIN32
bool CreateReadinessPair(net_socket* read_socket, net_socket* signal_socket) {
    *read_socket = kInvalidSocket;
    *signal_socket = kInvalidSocket;

    net_socket reader = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!IsValidSocket(reader)) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(reader, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        CloseSocket(reader);
        return false;
    }

    socklen_t address_len = sizeof(address);
    if (::getsockname(reader, reinterpret_cast<sockaddr*>(&address), &address_len) < 0) {
        CloseSocket(reader);
        return false;
    }

    net_socket signaler = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!IsValidSocket(signaler) ||
        ::connect(signaler, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        !SetSocketNonBlocking(reader) || !SetSocketNonBlocking(signaler)) {
        CloseSocket(signaler);
        CloseSocket(reader);
        return false;
    }

    *read_socket = reader;
    *signal_socket = signaler;
    return true;
}
#endif

class P2PTransport {
public:
    static P2PTransport& Instance() {
        static P2PTransport transport;
        return transport;
    }

    ~P2PTransport() {
        Stop();
    }

    bool Start() {
#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
        LOG_ERROR(Lib_Net, "P2P transport is currently supported on Linux and Windows");
        return false;
#else
        std::lock_guard lock(start_mutex);
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        host_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!IsValidSocket(host_socket)) {
            LOG_ERROR(Lib_Net, "P2P transport socket() failed error={}", LastSocketError());
            return false;
        }
#ifdef _WIN32
        DisableUdpConnectionReset(host_socket);
#endif

        int one = 1;
        (void)::setsockopt(host_socket, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&one), sizeof(one));
        if (!SetSocketNonBlocking(host_socket)) {
            LOG_ERROR(Lib_Net, "P2P transport failed to make socket nonblocking error={}",
                      LastSocketError());
            CloseSocket(host_socket);
            return false;
        }

        u16 requested_port = 0;
        if (const char* env = std::getenv("SHADPS4_P2P_PORT"); env != nullptr && *env != '\0') {
            char* end = nullptr;
            const long parsed = std::strtol(env, &end, 10);
            if (end == env || *end != '\0' || parsed < 1 || parsed > 65535) {
                LOG_ERROR(Lib_Net, "Invalid SHADPS4_P2P_PORT='{}'", env);
                CloseSocket(host_socket);
                return false;
            }
            requested_port = static_cast<u16>(parsed);
        }

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(requested_port);

        if (::bind(host_socket, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            LOG_ERROR(Lib_Net, "P2P transport bind(port={}) failed error={}", requested_port,
                      LastSocketError());
            CloseSocket(host_socket);
            return false;
        }

        sockaddr_in actual{};
        socklen_t actual_len = sizeof(actual);
        if (::getsockname(host_socket, reinterpret_cast<sockaddr*>(&actual), &actual_len) < 0) {
            LOG_ERROR(Lib_Net, "P2P transport getsockname() failed error={}", LastSocketError());
            CloseSocket(host_socket);
            return false;
        }

        physical_port = ntohs(actual.sin_port);
        running.store(true, std::memory_order_release);
        receive_thread = std::thread(&P2PTransport::ReceiveMain, this);
        if (const char* env = std::getenv("SHADPS4_P2P_DELAY_MS"); env != nullptr) {
            if (const auto range = ParseDelayRange(env)) {
                delay_line.emplace(*range, static_cast<u32>(physical_port));
                delay_thread = std::thread(&P2PTransport::DelayMain, this);
                LOG_INFO(Lib_Net, "P2P transport: every packet sent held {}-{} ms (test ping)",
                         range->min_ms, range->max_ms);
            }
        }

        LOG_INFO(Lib_Net,
                 "P2P transport ready: UDP physical_port={} (set SHADPS4_P2P_PORT to pin it)",
                 physical_port);
        return true;
#endif
    }

    void Stop() {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        std::lock_guard lock(start_mutex);
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        if (receive_thread.joinable()) {
            receive_thread.join();
        }
        ++work_generation;
        if (work_thread.joinable()) {
            work_thread.join();
        }
        delay_ready.notify_all();
        if (delay_thread.joinable()) {
            delay_thread.join();
        }
        delay_line.reset();
        delayed.clear();

        CloseSocket(host_socket);
        physical_port = 0;
#endif
    }

    bool IsReady() const {
        return running.load(std::memory_order_acquire);
    }

    u16 PhysicalPort() const {
        return physical_port;
    }

    bool RegisterSocket(P2PSocket* socket, u16 requested_vport_nbo, u16* actual_vport_nbo) {
        if (socket == nullptr || actual_vport_nbo == nullptr) {
            return false;
        }

        std::lock_guard lock(registry_mutex);
        u16 vport = requested_vport_nbo;

        // vport 0 acts as an ephemeral request in this PoC.
        if (vport == 0) {
            for (u32 attempt = 0; attempt < 35000; ++attempt) {
                if (next_ephemeral_vport >= 65535) {
                    next_ephemeral_vport = 30000;
                }
                const u16 candidate_host = next_ephemeral_vport++;
                const u16 candidate_nbo = htons(candidate_host);
                if (candidate_nbo == kSignalingVPortNbo || game_sockets.contains(candidate_nbo)) {
                    continue;
                }
                vport = candidate_nbo;
                break;
            }
        }

        if (vport == 0 || vport == kSignalingVPortNbo || game_sockets.contains(vport)) {
            return false;
        }

        game_sockets.emplace(vport, socket);
        *actual_vport_nbo = vport;
        LOG_INFO(Lib_Net, "P2P socket bound to vport {} t={}", ntohs(vport),
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count());
        return true;
    }

    void UnregisterSocket(P2PSocket* socket, u16 vport_nbo) {
        std::lock_guard lock(registry_mutex);
        const auto it = game_sockets.find(vport_nbo);
        if (it != game_sockets.end() && it->second == socket) {
            game_sockets.erase(it);
        }
    }

    int SendFrame(u16 source_vport_nbo, u16 dest_vport_nbo, const void* data, u32 len,
                  u32 dest_addr_nbo, u16 dest_port_nbo) {
#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
        return -1;
#else
        if (!Start() || !IsValidSocket(host_socket) || (data == nullptr && len != 0) ||
            len > kMaxP2PPayload || dest_addr_nbo == 0 || dest_port_nbo == 0) {
            return -1;
        }
        NoteSilence(std::nullopt);

        std::vector<u8> framed(kVPortHeaderSize + len);
        std::memcpy(framed.data(), &source_vport_nbo, sizeof(source_vport_nbo));
        std::memcpy(framed.data() + sizeof(source_vport_nbo), &dest_vport_nbo,
                    sizeof(dest_vport_nbo));
        if (len != 0) {
            std::memcpy(framed.data() + kVPortHeaderSize, data, len);
        }

        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = dest_addr_nbo;
        dest.sin_port = dest_port_nbo;

        // The server link: the frame goes to shadNet, which hands it to the peer.
        if (const auto server = LinkServer();
            Link::RoutesThroughServer(server, dest_addr_nbo, dest_port_nbo)) {
            auto sealed = SealLink(Link::EncodeData(dest_addr_nbo, dest_port_nbo, framed));
            if (!sealed) {
                return -1; // no link yet: the game sends its own again
            }
            framed = std::move(*sealed);
            dest.sin_addr.s_addr = server.addr_nbo;
            dest.sin_port = server.port_nbo;
        }
        return Transmit(dest, std::move(framed)) ? static_cast<int>(len) : -1;
#endif
    }

    // Sends a datagram as it is, after the test ping's delay when there is one.
    bool Transmit(const sockaddr_in& dest, std::vector<u8> datagram) {
#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
        return false;
#else
        if (LinkCut()) {
            return true; // lost on the way, as on a dropped connection
        }
        if (!FirewallAllows(dest.sin_addr.s_addr, dest.sin_port, "a datagram to")) {
            return false;
        }
        if (delay_line) {
            {
                std::lock_guard lock(delay_mutex);
                delayed.push_back(
                    {delay_line->DueAt(DelayLine::Clock::now()), dest, std::move(datagram)});
            }
            delay_ready.notify_one();
            return true;
        }

        const auto rc = ::sendto(host_socket, reinterpret_cast<const char*>(datagram.data()),
                                 static_cast<int>(datagram.size()), 0,
                                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (rc < 0) {
            LOG_DEBUG(Lib_Net, "P2P sendto failed error={} dst={:#x}:{}", LastSocketError(),
                      dest.sin_addr.s_addr, ntohs(dest.sin_port));
            return false;
        }
        if (static_cast<size_t>(rc) != datagram.size()) {
            LOG_WARNING(Lib_Net, "P2P short UDP send: {} of {} bytes", rc, datagram.size());
            return false;
        }
        return true;
#endif
    }

    void ConfigureLink(u32 server_addr_nbo, u16 server_port_nbo, std::string online_id,
                       std::string secret, std::string_view server_key) {
        const auto key = LinkAuth::KeyFromHex(server_key);
        {
            std::lock_guard lock(link_mutex);
            link_server = {server_addr_nbo, server_port_nbo};
            link_online_id = std::move(online_id);
            link_secret = std::move(secret);
            link_welcomed = false;
            link_hello_ms.reset();
            link_auth = {};
            if (key) {
                link_auth.Pin(*key);
            }
            link_ticket.clear();
            link_other_keys = 0;
            ++work_generation; // a work under way was for the other id
        }
        LOG_INFO(Lib_Net, "Server link: every frame for a peer goes through the server at {:#x}:{}",
                 ntohl(server_addr_nbo), ntohs(server_port_nbo));
        if (key) {
            LOG_INFO(Lib_Net, "Server link: the server's key, from the launcher: {}", server_key);
        } else {
            if (!server_key.empty()) {
                LOG_WARNING(Lib_Net, "Server link: '{}' is not a server key (64 hex digits)",
                            server_key);
            }
            LOG_INFO(Lib_Net, "Server link: no key for this server: its first cookie's is taken");
        }
    }

    void SetLinkBuild(std::string build) {
        std::lock_guard lock(link_mutex);
        if (Link::SaysHelloAgainForBuild(link_build, build)) {
            link_welcomed = false;
            link_hello_ms.reset();
        }
        link_build = std::move(build);
    }

    bool SendToServer(std::span<const u8> body) {
        const Link::ServerEndpoint server = LinkServer();
        if (server.addr_nbo == 0 || server.port_nbo == 0 || !IsReady()) {
            return false;
        }
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = server.addr_nbo;
        dest.sin_port = server.port_nbo;
        auto sealed = SealLink(Link::EncodeCoop(body));
        return sealed && Transmit(dest, std::move(*sealed));
    }

    // Phase 11: a frame of the Rpc tunnel's stream for the server (before its welcome too: the
    // stream resends until it is taken).
    bool SendRpcToServer(std::span<const u8> body) {
        const Link::ServerEndpoint server = LinkServer();
        if (server.addr_nbo == 0 || server.port_nbo == 0 || !IsReady()) {
            return false;
        }
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = server.addr_nbo;
        dest.sin_port = server.port_nbo;
        auto sealed = SealLink(Link::EncodeRpc(body));
        return sealed && Transmit(dest, std::move(*sealed));
    }

    // Link version 4 (net_link.h): a datagram for the server encrypted, with its link's counter
    // and tag; nothing before the server's welcome (the server would drop it).
    std::optional<std::vector<u8>> SealLink(std::vector<u8> datagram) {
        std::lock_guard lock(link_mutex);
        return Link::Sealed(link_auth, std::move(datagram));
    }

    // A new ephemeral key for a hello's handshake (the system's random source).
    static LinkAuth::Key FreshKey() {
        std::random_device device;
        LinkAuth::Key key;
        for (u8& byte : key) {
            byte = static_cast<u8>(device());
        }
        return key;
    }

    bool LinkWelcomed() {
        std::lock_guard lock(link_mutex);
        return link_welcomed;
    }

    void CutLink(u32 ms) {
        link_cut_until_ms.store(P2PNowMs() + ms);
        LOG_WARNING(Lib_Net, "P2P: this game's connection is cut for {} ms (the rig)", ms);
    }

    bool LinkCut() const {
        return P2PNowMs() < link_cut_until_ms.load(std::memory_order_relaxed);
    }

    std::string LinkRefusal() {
        std::lock_guard lock(link_mutex);
        return link_refusal;
    }

    std::string LinkWarning() {
        std::lock_guard lock(link_mutex);
        return link_warning;
    }

    void SetLinkObserver(ServerLinkObserver observer) {
        link_observer.store(observer);
    }

    int SendInternal(const void* data, u32 len, u32 dest_addr_nbo, u16 dest_port_nbo) {
        // Phase 7: our signaling, matching and co-op frames go the game sockets' way, never
        // straight to a peer (before the server is known, nothing).
        if (!Link::SendsToPeer(Server())) {
            return -1;
        }
        return SendFrame(kSignalingVPortNbo, kSignalingVPortNbo, data, len, dest_addr_nbo,
                         dest_port_nbo);
    }

    int ReceiveInternal(InternalChannel channel, void* buf, u32 len, u32* from_addr,
                        u16* from_port) {
        std::lock_guard lock(internal_mutex);
        auto& queue = QueueFor(channel);
        if (queue.empty()) {
            return -1;
        }

        QueuedPacket packet = std::move(queue.front());
        queue.pop_front();

        const u32 copy_len = static_cast<u32>(std::min<size_t>(len, packet.payload.size()));
        if (copy_len != 0 && buf != nullptr) {
            std::memcpy(buf, packet.payload.data(), copy_len);
        }
        if (from_addr != nullptr) {
            *from_addr = packet.source_addr;
        }
        if (from_port != nullptr) {
            *from_port = packet.source_port;
        }
        return static_cast<int>(copy_len);
    }

    // The server link's endpoint (none until it is configured).
    Link::ServerEndpoint Server() {
        return LinkServer();
    }

private:
    P2PTransport() = default;

    // Logs a peer that stops sending and is heard again (see PeerSilence): on every packet
    // received (`heard`) and every send, so a peer going quiet is noticed while the game talks.
    void NoteSilence(std::optional<u64> heard) {
        const s64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        std::vector<SilenceChange> changes;
        {
            std::lock_guard lock(silence_mutex);
            if (heard) {
                if (const auto back = silence.Heard(*heard, now_ms)) {
                    changes.push_back(*back);
                }
            }
            if (now_ms - silence_checked_ms >= 1000) {
                silence_checked_ms = now_ms;
                const auto quiet = silence.Check(now_ms);
                changes.insert(changes.end(), quiet.begin(), quiet.end());
            }
        }
        for (const SilenceChange& change : changes) {
            const u32 addr = static_cast<u32>(change.peer >> 16);
            const u16 port = ntohs(static_cast<u16>(change.peer & 0xFFFF));
            if (change.silent) {
                LOG_WARNING(Lib_Net, "P2P: nothing from {:#x}:{} for {} ms ({} packets before)",
                            addr, port, change.gap_ms, change.packets);
            } else {
                LOG_INFO(Lib_Net, "P2P: {:#x}:{} heard again after {} ms", addr, port,
                         change.gap_ms);
            }
        }
    }

    std::deque<QueuedPacket>& QueueFor(InternalChannel channel) {
        switch (channel) {
        case InternalChannel::Signaling:
            return signaling_queue;
        case InternalChannel::Control:
            return control_queue;
        case InternalChannel::Coop:
            return coop_queue;
        case InternalChannel::GamePackets:
            return game_packets_queue;
        case InternalChannel::ServerCoop:
            return server_coop_queue;
        case InternalChannel::ServerRpc:
            return server_rpc_queue;
        }
        return signaling_queue;
    }

    static InternalChannel ClassifyInternal(const u8* payload, size_t size) {
        if (size >= 5 && std::memcmp(payload, "SHAD", 4) == 0) {
            if (payload[4] == 0x10) {
                return InternalChannel::Control;
            }
            if ((payload[4] & 0xF0) == 0x30) { // co-op: 0x30 damage, 0x31 vitals, 0x32 kills
                return InternalChannel::Coop;
            }
            if (payload[4] == 0x40) {
                return InternalChannel::GamePackets;
            }
        }
        // Signaling 0x05/0x06/0x07 land here.
        return InternalChannel::Signaling;
    }

    void PushInternal(InternalChannel channel, u32 from_addr, u16 from_port, const u8* payload,
                      size_t size) {
        QueuedPacket packet{};
        packet.source_addr = from_addr;
        packet.source_port = from_port;
        packet.payload.assign(payload, payload + size);

        std::lock_guard lock(internal_mutex);
        auto& queue = QueueFor(channel);
        // Nobody may read the co-op queue (no party): keep only the newest frames.
        if (channel == InternalChannel::Coop && queue.size() >= 256) {
            queue.pop_front();
        }
        // Read every few milliseconds while a party plays; a run without one keeps the newest.
        if ((channel == InternalChannel::GamePackets || channel == InternalChannel::ServerCoop ||
             channel == InternalChannel::ServerRpc) &&
            queue.size() >= 4096) {
            queue.pop_front();
        }
        queue.push_back(std::move(packet));
    }

    void ReceiveMain() {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        std::vector<u8> buffer(65536);
        while (running.load(std::memory_order_acquire)) {
            LinkHousekeeping();
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
#ifdef _WIN32
            constexpr int receive_flags = 0;
#else
            constexpr int receive_flags = MSG_DONTWAIT;
#endif
            const auto rc = ::recvfrom(host_socket, reinterpret_cast<char*>(buffer.data()),
                                       static_cast<int>(buffer.size()), receive_flags,
                                       reinterpret_cast<sockaddr*>(&from), &from_len);

            if (rc < 0) {
                const int error = LastSocketError();
                if (IsRetryableSocketError(error)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                // The address the error came back from, when the system names it.
                if (from.sin_family == AF_INET) {
                    LOG_WARNING(Lib_Net, "P2P recvfrom failed error={} from {:#x}:{}", error,
                                ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
                } else {
                    LOG_WARNING(Lib_Net, "P2P recvfrom failed error={}", error);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (rc < static_cast<decltype(rc)>(kVPortHeaderSize)) {
                LOG_DEBUG(Lib_Net, "P2P dropping too-short UDP frame size={}", rc);
                continue;
            }
            if (LinkCut()) {
                continue; // never arrived (a dropped connection, the rig's cut-link)
            }
            if (!FirewallAllows(from.sin_addr.s_addr, from.sin_port, "a datagram from")) {
                continue;
            }
            const std::span<const u8> datagram{buffer.data(), static_cast<size_t>(rc)};
            if (const auto server = LinkServer();
                server.addr_nbo == from.sin_addr.s_addr && server.port_nbo == from.sin_port) {
                if (const auto kind = Link::KindOf(datagram)) {
                    OnLinkDatagram(*kind, datagram);
                    continue;
                }
            }
            if (!Link::TakesDatagramFrom(LinkServer(), from.sin_addr.s_addr, from.sin_port)) {
                continue; // no datagram straight from a peer (phase 6), nor before the link
            }
            Deliver(from.sin_addr.s_addr, from.sin_port, datagram);
        }
#endif
    }

    // A frame from addr:port (straight from the peer, or relayed by the server): signaling to
    // its queues, a game packet to the socket bound to its vport.
    void Deliver(u32 from_addr_nbo, u16 from_port_nbo, std::span<const u8> frame) {
        if (frame.size() < kVPortHeaderSize) {
            return;
        }
        NoteSilence((u64{from_addr_nbo} << 16) | from_port_nbo);
        u16 source_vport_nbo = 0;
        u16 dest_vport_nbo = 0;
        std::memcpy(&source_vport_nbo, frame.data(), sizeof(source_vport_nbo));
        std::memcpy(&dest_vport_nbo, frame.data() + sizeof(source_vport_nbo),
                    sizeof(dest_vport_nbo));

        const u8* payload = frame.data() + kVPortHeaderSize;
        const size_t payload_size = frame.size() - kVPortHeaderSize;

        if (dest_vport_nbo == kSignalingVPortNbo) {
            PushInternal(ClassifyInternal(payload, payload_size), from_addr_nbo, from_port_nbo,
                         payload, payload_size);
            return;
        }

        std::lock_guard lock(registry_mutex);
        const auto it = game_sockets.find(dest_vport_nbo);
        if (it == game_sockets.end() || it->second == nullptr) {
            // Rig 2026-10-13: about 1 join in 4 stalls, the guest's game giving up its
            // connection 5.4 s after signaling came up with no game traffic in between; a
            // one-shot hello dropped here, before the game bound its port, would do that. The
            // first drops are logged to tell.
            static std::atomic<u32> dropped{0};
            if (dropped.fetch_add(1) < 200) {
                LOG_INFO(Lib_Net,
                         "P2P RX dropped, no socket for dst_vport={} (src_vport={}, {} bytes, "
                         "first byte {:#x}) t={}",
                         ntohs(dest_vport_nbo), ntohs(source_vport_nbo), payload_size,
                         payload_size > 0 ? static_cast<u8>(payload[0]) : 0,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
            }
            return;
        }

        P2PDatagram packet{};
        packet.source_addr = from_addr_nbo;
        packet.source_port = from_port_nbo;
        packet.source_vport = source_vport_nbo;
        packet.payload.assign(payload, payload + payload_size);
        it->second->EnqueuePacket(std::move(packet));
    }

    Link::ServerEndpoint LinkServer() {
        std::lock_guard lock(link_mutex);
        return link_server;
    }

    void TellLinkObserver(std::string_view event, const std::string& detail) {
        if (const auto observer = link_observer.load()) {
            observer(event, detail);
        }
    }

    // A datagram from the server's link: a relayed frame, the hello's answer, a pong, news of
    // another member's link.
    void OnLinkDatagram(Link::Kind kind, std::span<const u8> sealed) {
        const s64 now_ms = P2PNowMs();
        std::vector<u8> opened;
        std::optional<LinkAuth::Key> learned;
        bool other_key = false;
        {
            std::lock_guard lock(link_mutex);
            if (kind == Link::Kind::Cookie) {
                // The hello's answer without a cookie of ours: echoed in the next hello at once.
                // Never encrypted, so taken only while there is a hello to answer, and only with
                // the key pinned for this server (when one is).
                LinkAuth::Key key{};
                if (const auto cookie = Link::DecodeCookie(sealed, nullptr, &key);
                    cookie && !link_welcomed) {
                    if (link_auth.TakeCookie(*cookie, key)) {
                        link_hello_ms.reset();
                    } else if (link_other_keys++ == 0) {
                        // Someone else answering for the server, or the server made a new key
                        // (then its operator says the new one, and the launcher's pin goes).
                        LOG_ERROR(Lib_Net,
                                  "Server link: the server says another key than the one pinned "
                                  "for it ({}, pinned {}): no link",
                                  LinkAuth::HexOf(key), LinkAuth::HexOf(*link_auth.ServerKey()));
                        other_key = true;
                    }
                }
            } else if (auto plain = Link::Opened(link_auth, sealed);
                       plain && Link::KindOf(*plain) == kind) {
                opened = std::move(*plain);
                learned = link_auth.TakeLearned();
            } else if (link_untagged++ < 20) {
                // Not from the server (a spoofed source, another server's key), or a replay.
                LOG_INFO(Lib_Net, "Server link: a datagram of kind {} that does not open: dropped",
                         static_cast<int>(kind));
            }
        }
        if (other_key) {
            TellLinkObserver("refused", "the server's key is not the one pinned for it");
        }
        if (opened.empty()) {
            return;
        }
        if (learned) {
            // The launcher keeps it for this server (bbport_coop.py LEARNED_KEY_LINE).
            LOG_INFO(Lib_Net, "Server link: the server's key, trusted on first use: {}",
                     LinkAuth::HexOf(*learned));
        }
        const std::span<const u8> datagram{opened};
        switch (kind) {
        case Link::Kind::Data:
            if (const auto data = Link::DecodeData(datagram)) {
                Deliver(data->addr_nbo, data->port_nbo, data->frame);
            }
            return;
        case Link::Kind::Welcome:
            if (const auto welcome = Link::DecodeWelcome(datagram)) {
                OnWelcome(*welcome);
            }
            return;
        case Link::Kind::Pong:
            if (const auto pong = Link::DecodePing(Link::Kind::Pong, datagram)) {
                std::optional<Link::LinkChange> back;
                {
                    std::lock_guard lock(link_mutex);
                    back = ping_meter.Pong(*pong, now_ms);
                }
                if (back) {
                    LOG_INFO(Lib_Net, "Server link: the server answers again after {} ms",
                             back->gap_ms);
                    TellLinkObserver("back", fmt::format("gap_ms={}", back->gap_ms));
                }
            }
            return;
        case Link::Kind::Member:
            if (const auto member = Link::DecodeMember(datagram)) {
                OnMember(*member);
            }
            return;
        case Link::Kind::Coop:
            if (const auto body = Link::DecodeCoop(datagram)) {
                const Link::ServerEndpoint server = LinkServer();
                PushInternal(InternalChannel::ServerCoop, server.addr_nbo, server.port_nbo,
                             body->data(), body->size());
            }
            return;
        case Link::Kind::Rpc:
            if (const auto body = Link::DecodeRpc(datagram)) {
                const Link::ServerEndpoint server = LinkServer();
                PushInternal(InternalChannel::ServerRpc, server.addr_nbo, server.port_nbo,
                             body->data(), body->size());
            }
            return;
        case Link::Kind::Work:
            if (const auto bits = Link::DecodeWork(datagram)) {
                StartWork(*bits);
            }
            return;
        case Link::Kind::Redirect:
            if (const auto redirect = Link::DecodeRedirect(datagram)) {
                OnRedirect(*redirect);
            }
            return;
        default:
            return;
        }
    }

    // A region of shards (docs/SCALING.md): the server sends this game on to another shard. The
    // link moves there as after a restart of the server: a new cookie and handshake there with
    // the same id and secret, the key it says pinned (it came sealed by a server this game
    // trusts), its ticket in the hellos. The Coop and Rpc streams see a new server and start
    // over by themselves; the HTTP calls follow the link (http.cpp FollowLinkHost).
    void OnRedirect(const Link::Redirect& redirect) {
        if (redirect.addr_nbo == 0 || redirect.port_nbo == 0) {
            return;
        }
        {
            std::lock_guard lock(link_mutex);
            link_server = {redirect.addr_nbo, redirect.port_nbo};
            link_welcomed = false;
            link_hello_ms.reset();
            link_refusal.clear();
            link_warning.clear();
            link_auth = {};
            link_auth.Pin(redirect.key);
            link_ticket = redirect.ticket;
            link_other_keys = 0;
            ++work_generation; // a work under way was for the other shard's cookie
        }
        const u32 addr = ntohl(redirect.addr_nbo);
        const std::string host = fmt::format("{}.{}.{}.{}", addr >> 24, (addr >> 16) & 0xFF,
                                             (addr >> 8) & 0xFF, addr & 0xFF);
        Libraries::Http::FollowLinkHost(host);
        LOG_INFO(Lib_Net, "Server link: sent on to the shard at {}:{} ({}), key {}", host,
                 ntohs(redirect.port_nbo), redirect.text, LinkAuth::HexOf(redirect.key));
        TellLinkObserver("redirect", fmt::format("shard={}:{}", host, ntohs(redirect.port_nbo)));
    }

    // Hosted: this id has no account yet, and the server asks its first hello for work
    // (net_link.h). Done on a thread of its own, about a second's CPU once per account, so the
    // receive thread goes on; the hellos wait for it, then one carries it at once.
    void StartWork(int bits) {
        LinkAuth::Cookie cookie;
        std::string online_id;
        {
            std::lock_guard lock(link_mutex);
            if (link_welcomed || !link_auth.WorkAsked(bits)) {
                return;
            }
            cookie = link_auth.CookieToEcho();
            online_id = link_online_id;
        }
        LOG_INFO(Lib_Net, "Server link: a new account: the server asks for {} bits of work", bits);
        const u64 generation = ++work_generation;
        if (work_thread.joinable()) {
            work_thread.join(); // it gives up within a slice: its generation is over
        }
        work_thread = std::thread([this, cookie, online_id, bits, generation] {
            const auto started = std::chrono::steady_clock::now();
            constexpr u64 Slice = u64{1} << 16;
            std::optional<u64> found;
            for (u64 from = 0; !found && running.load(std::memory_order_acquire) &&
                               work_generation.load() == generation;
                 from += Slice) {
                found = LinkAuth::SolveWork(cookie, online_id, bits, from, Slice);
            }
            {
                std::lock_guard lock(link_mutex);
                if (work_generation.load() != generation) {
                    return;
                }
                link_auth.TakeWork(cookie, bits, found);
                link_hello_ms.reset();
            }
            LOG_INFO(Lib_Net, "Server link: the new account's work took {} ms",
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count());
        });
    }

    void OnWelcome(const Link::Welcome& welcome) {
        bool first = false;
        bool repeated = false;
        {
            std::lock_guard lock(link_mutex);
            if (welcome.accepted) {
                first = !link_welcomed;
                link_welcomed = true;
                link_refusal.clear();
                // Another installation than the others' (Kaniel 2026-10-25): linked, and told.
                repeated = link_warning == welcome.text;
                link_warning = welcome.text;
            } else {
                repeated = link_refusal == welcome.text;
                link_refusal = welcome.text;
            }
        }
        if (welcome.accepted) {
            // The address the game's own NAT queries see: its virtual one (no STUN).
            std::string online_id;
            {
                std::lock_guard lock(link_mutex);
                online_id = link_online_id;
            }
            Common::Singleton<NetUtil::NetUtilInternal>::Instance()->SetExternalIp(
                htonl(Link::VirtualAddressOf(online_id).ipv4));
            if (first) {
                LOG_INFO(Lib_Net, "Server link: the server took this game's hello");
                TellLinkObserver("welcome", welcome.text);
            }
            if (!welcome.text.empty() && !repeated) {
                // The team screen's line, then every field that differs.
                std::string text = welcome.text;
                for (size_t at = 0; (at = text.find('\n', at)) != std::string::npos;) {
                    text.replace(at, 1, " | ");
                }
                LOG_WARNING(Lib_Net, "Server link: this game's installation differs: {}", text);
                TellLinkObserver("differs", welcome.text);
            }
        } else if (!repeated) {
            // One line: the build check's refusal is the screen's line, then the whole reason.
            std::string text = welcome.text;
            for (size_t at = 0; (at = text.find('\n', at)) != std::string::npos;) {
                text.replace(at, 1, " | ");
            }
            LOG_ERROR(Lib_Net, "Server link: the server refused this game: {}", text);
            TellLinkObserver("refused", welcome.text);
        }
    }

    void OnMember(const Link::Member& member) {
        const u32 addr = ntohl(member.addr_nbo);
        const u16 port = ntohs(member.port_nbo);
        const char* event = "member-quiet";
        switch (member.state) {
        case Link::MemberState::Quiet:
            LOG_WARNING(Lib_Net, "Server link: nothing from {} ({:#x}:{}) for {} ms",
                        member.online_id, addr, port, member.gap_ms);
            break;
        case Link::MemberState::Back:
            event = "member-back";
            LOG_INFO(Lib_Net, "Server link: {} ({:#x}:{}) heard again after {} ms",
                     member.online_id, addr, port, member.gap_ms);
            break;
        case Link::MemberState::Gone:
            event = "member-gone";
            LOG_WARNING(Lib_Net, "Server link: {} ({:#x}:{}) gone, nothing for {} ms",
                        member.online_id, addr, port, member.gap_ms);
            break;
        }
        TellLinkObserver(event, fmt::format("member={} peer={:#x}:{} gap_ms={}", member.online_id,
                                            addr, port, member.gap_ms));
    }

    // The link's hello (until the server takes it), its ping, and the outage and round trip
    // lines. On the receive thread, every LinkTickMs.
    void LinkHousekeeping() {
        if (!ServerLinkSwitchOn()) {
            return;
        }
        const s64 now_ms = P2PNowMs();
        if (now_ms - link_tick_ms < LinkTickMs) {
            return;
        }
        link_tick_ms = now_ms;
        std::vector<std::vector<u8>> datagrams;
        Link::ServerEndpoint server;
        std::optional<Link::LinkChange> quiet;
        std::optional<Link::PingSummary> summary;
        {
            std::lock_guard lock(link_mutex);
            server = link_server;
            if (server.addr_nbo == 0 || server.port_nbo == 0) {
                return;
            }
            if (!link_auth.Working() &&
                Link::HelloDue(link_welcomed, link_hello_ms, !link_refusal.empty(), now_ms)) {
                link_hello_ms = now_ms;
                datagrams.push_back(Link::HelloDatagram(link_auth, link_online_id, link_build,
                                                        link_secret, FreshKey(), link_ticket));
            }
            if (const auto ping = ping_meter.Due(now_ms)) {
                if (auto sealed = Link::Sealed(link_auth, Link::Encode(Link::Kind::Ping, *ping))) {
                    datagrams.push_back(std::move(*sealed));
                }
            }
            quiet = ping_meter.Check(now_ms);
            if (quiet) {
                // The server may have restarted and forgotten this game: say hello again, on a
                // new session (the old one stays in use until the server welcomes it).
                link_welcomed = false;
                link_auth.Restart();
            }
            summary = ping_meter.TakeSummary(now_ms);
        }
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = server.addr_nbo;
        dest.sin_port = server.port_nbo;
        for (auto& datagram : datagrams) {
            Transmit(dest, std::move(datagram));
        }
        if (quiet) {
            LOG_WARNING(Lib_Net, "Server link: no answer from the server for {} ms", quiet->gap_ms);
            TellLinkObserver("quiet", fmt::format("gap_ms={}", quiet->gap_ms));
        }
        if (summary) {
            LOG_INFO(Lib_Net,
                     "Server link: round trip {}/{}/{} ms (min/avg/max), {} of {} pings "
                     "answered",
                     summary->min_ms, summary->avg_ms, summary->max_ms, summary->pongs,
                     summary->pings);
            TellLinkObserver("rtt", fmt::format("min_ms={} avg_ms={} max_ms={} pongs={} pings={}",
                                                summary->min_ms, summary->avg_ms, summary->max_ms,
                                                summary->pongs, summary->pings));
        }
    }

    // The test ping (SHADPS4_P2P_DELAY_MS): packets wait their turn here, sent by DelayMain.
    void DelayMain() {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
        std::unique_lock lock(delay_mutex);
        while (running.load(std::memory_order_acquire)) {
            if (delayed.empty()) {
                delay_ready.wait_for(lock, std::chrono::milliseconds(50));
                continue;
            }
            if (DelayLine::Clock::now() < delayed.front().due) {
                delay_ready.wait_until(lock, delayed.front().due);
                continue;
            }
            DelayedFrame frame = std::move(delayed.front());
            delayed.pop_front();
            lock.unlock();
            (void)::sendto(host_socket, reinterpret_cast<const char*>(frame.framed.data()),
                           static_cast<int>(frame.framed.size()), 0,
                           reinterpret_cast<const sockaddr*>(&frame.dest), sizeof(frame.dest));
            lock.lock();
        }
#endif
    }

    struct DelayedFrame {
        DelayLine::Clock::time_point due;
        sockaddr_in dest;
        std::vector<u8> framed;
    };

    mutable std::mutex start_mutex;
    std::atomic<bool> running{false};
    std::thread receive_thread;

    std::optional<DelayLine> delay_line;
    std::atomic<s64> link_cut_until_ms{0}; // CutLink
    std::thread delay_thread;
    std::mutex delay_mutex;
    std::condition_variable delay_ready;
    std::deque<DelayedFrame> delayed;

    net_socket host_socket{kInvalidSocket};
    u16 physical_port{0};

    std::mutex registry_mutex;
    std::unordered_map<u16, P2PSocket*> game_sockets;
    u16 next_ephemeral_vport{30000};

    // The server link (net_link.h); link_tick_ms only on the receive thread.
    std::mutex link_mutex;
    Link::ServerEndpoint link_server;
    std::string link_online_id;
    std::string link_secret;
    std::string link_build;
    bool link_welcomed{false};
    std::string link_refusal;
    std::string link_warning; // the welcome's: another installation than the others'
    std::optional<s64> link_hello_ms;
    LinkAuth::Client link_auth;  // link version 4: the key, the cookie, the handshake, the session
    std::vector<u8> link_ticket; // a region's: from the Redirect that sent the game to this shard
    u32 link_other_keys{0};      // cookies that said another key than the pinned one
    std::thread work_thread;     // hosted: a new account's work (StartWork)
    std::atomic<u64> work_generation{0};
    u32 link_untagged{0};
    Link::PingMeter ping_meter;
    s64 link_tick_ms{0};
    std::atomic<ServerLinkObserver> link_observer{nullptr};

    std::mutex silence_mutex;
    PeerSilence silence{3000};
    s64 silence_checked_ms{0};

    std::mutex internal_mutex;
    std::deque<QueuedPacket> signaling_queue;
    std::deque<QueuedPacket> control_queue;
    std::deque<QueuedPacket> coop_queue;
    std::deque<QueuedPacket> game_packets_queue;
    std::deque<QueuedPacket> server_coop_queue;
    std::deque<QueuedPacket> server_rpc_queue;
};

int ReadIntegerOption(const void* optval, u32 optlen, int* out) {
    if (optval == nullptr || out == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (optlen < sizeof(int)) {
        return FailWith(ORBIS_NET_EINVAL);
    }
    std::memcpy(out, optval, sizeof(int));
    return 0;
}

int WriteIntegerOption(void* optval, u32* optlen, int value) {
    if (optval == nullptr || optlen == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (*optlen < sizeof(int)) {
        return FailWith(ORBIS_NET_EINVAL);
    }
    std::memcpy(optval, &value, sizeof(int));
    *optlen = sizeof(int);
    return 0;
}

} // namespace

P2PSocket::P2PSocket(int domain, int type, int protocol) : Socket(domain, type, protocol) {
#ifdef _WIN32
    if (!CreateReadinessPair(&readiness_fd, &readiness_signal_fd)) {
        LOG_ERROR(Lib_Net, "P2P readiness socket pair failed error={}", LastSocketError());
        valid = false;
    }
#elif defined(__APPLE__)
    int pair[2];
    // A datagram socket pair reports writable as well as readable readiness, like eventfd.
    if (::socketpair(AF_UNIX,SOCK_DGRAM,0,pair) != 0) { valid=false; return; }
    for (int fd : pair) {
        if (::fcntl(fd,F_SETFL,O_NONBLOCK)<0 || ::fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
            ::close(pair[0]);::close(pair[1]);valid=false;return;
        }
    }
    readiness_fd=pair[0];readiness_signal_fd=pair[1];
#elif defined(__linux__)
    readiness_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (readiness_fd < 0) {
        LOG_ERROR(Lib_Net, "P2P eventfd() failed errno={}", errno);
        valid = false;
    }
#endif
}

P2PSocket::~P2PSocket() {
    Close();
}

int P2PSocket::Close() {
    std::lock_guard lock(m_mutex);
    if (!valid) {
        return 0;
    }

    if (bound) {
        P2PTransport::Instance().UnregisterSocket(this, local_addr.sin_vport);
        bound = false;
    }

    {
        std::lock_guard receive_lock(receive_mutex);
        valid = false;
    }
    connected = false;
    receive_cv.notify_all();

#ifdef _WIN32
    CloseSocket(readiness_signal_fd);
    CloseSocket(readiness_fd);
#elif defined(__linux__) || defined(__APPLE__)
#ifdef __APPLE__
    if (readiness_signal_fd>=0) { ::close(readiness_signal_fd);readiness_signal_fd=-1; }
#endif
    if (readiness_fd >= 0) {
        ::close(readiness_fd);
        readiness_fd = -1;
    }
#endif
    return 0;
}

int P2PSocket::SetSocketOptions(int level, int optname, const void* optval, u32 optlen) {
    if (level != ORBIS_NET_SOL_SOCKET) {
        LOG_WARNING(Lib_Net, "P2P setsockopt ignoring non-SOL_SOCKET level={} opt={}", level,
                    optname);
        return 0;
    }

    int value = 0;
    switch (optname) {
    case ORBIS_NET_SO_NBIO:
    case ORBIS_NET_SO_USECRYPTO:
    case ORBIS_NET_SO_USESIGNATURE:
    case ORBIS_NET_SO_REUSEADDR:
    case ORBIS_NET_SO_REUSEPORT:
    case ORBIS_NET_SO_KEEPALIVE:
    case ORBIS_NET_SO_BROADCAST:
    case ORBIS_NET_SO_ONESBCAST:
    case ORBIS_NET_SO_SNDBUF:
    case ORBIS_NET_SO_RCVBUF:
    case ORBIS_NET_SO_PRIORITY:
        if (ReadIntegerOption(optval, optlen, &value) < 0) {
            return -1;
        }
        integer_socket_options[optname] = value;
        if (optname == ORBIS_NET_SO_NBIO) {
            std::lock_guard receive_lock(receive_mutex);
            sockopt_so_nbio = value;
        } else if (optname == ORBIS_NET_SO_USECRYPTO) {
            sockopt_so_usecrypto = value;
        } else if (optname == ORBIS_NET_SO_USESIGNATURE) {
            sockopt_so_usesignature = value;
        }
        return 0;
    default:
        // For the first emulator-to-emulator PoC, remember that an option was requested
        // but do not fail the title merely because the Sony-specific behavior is absent.
        LOG_WARNING(Lib_Net, "P2P setsockopt opt={:#x} accepted as no-op", optname);
        return 0;
    }
}

int P2PSocket::GetSocketOptions(int level, int optname, void* optval, u32* optlen) {
    if (level != ORBIS_NET_SOL_SOCKET) {
        return FailWith(ORBIS_NET_ENOPROTOOPT);
    }

    switch (optname) {
    case ORBIS_NET_SO_NBIO:
        return WriteIntegerOption(optval, optlen, sockopt_so_nbio);
    case ORBIS_NET_SO_USECRYPTO:
        return WriteIntegerOption(optval, optlen, sockopt_so_usecrypto);
    case ORBIS_NET_SO_USESIGNATURE:
        return WriteIntegerOption(optval, optlen, sockopt_so_usesignature);
    case ORBIS_NET_SO_TYPE:
        return WriteIntegerOption(optval, optlen, socket_type);
    case ORBIS_NET_SO_ERROR:
    case ORBIS_NET_SO_ERROR_EX:
        return WriteIntegerOption(optval, optlen, 0);
    default:
        if (const auto it = integer_socket_options.find(optname);
            it != integer_socket_options.end()) {
            return WriteIntegerOption(optval, optlen, it->second);
        }
        return FailWith(ORBIS_NET_ENOPROTOOPT);
    }
}

int P2PSocket::Bind(const OrbisNetSockaddr* addr, u32 addrlen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return FailWith(ORBIS_NET_EOPNOTSUPP);
    }
    if (addr == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (addrlen < sizeof(OrbisNetSockaddrIn)) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    const auto* in = reinterpret_cast<const OrbisNetSockaddrIn*>(addr);
    if (in->sin_family != ORBIS_NET_AF_INET) {
        return FailWith(ORBIS_NET_EAFNOSUPPORT);
    }
    if (!EnsureP2PTransport()) {
        return FailWith(ORBIS_NET_ENETDOWN);
    }

    std::lock_guard lock(m_mutex);
    if (!valid) {
        return FailWith(ORBIS_NET_EBADF);
    }
    if (bound) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    u16 actual_vport = 0;
    if (!P2PTransport::Instance().RegisterSocket(this, in->sin_vport, &actual_vport)) {
        return FailWith(ORBIS_NET_EADDRINUSE);
    }

    local_addr = *in;
    local_addr.sin_len = sizeof(OrbisNetSockaddrIn);
    local_addr.sin_family = ORBIS_NET_AF_INET;
    local_addr.sin_port = htons(P2PTransport::Instance().PhysicalPort());
    local_addr.sin_vport = actual_vport;
    bound = true;

    LOG_INFO(Lib_Net, "P2P bind: physical_port={} virtual_port={}",
             P2PTransport::Instance().PhysicalPort(), ntohs(local_addr.sin_vport));
    return 0;
}

int P2PSocket::Listen(int backlog) {
    LOG_WARNING(Lib_Net, "P2P STREAM listen not implemented (backlog={})", backlog);
    return FailWith(ORBIS_NET_EOPNOTSUPP);
}

int P2PSocket::SendMessage(const OrbisNetMsghdr* msg, int flags) {
    if (msg == nullptr || msg->msg_iovlen < 0) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    size_t total = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        total += static_cast<size_t>(msg->msg_iov[i].iov_len);
        if (total > kMaxP2PPayload || total > std::numeric_limits<u32>::max()) {
            return FailWith(ORBIS_NET_EMSGSIZE);
        }
    }

    std::vector<u8> payload(total);
    size_t offset = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        const auto len = static_cast<size_t>(msg->msg_iov[i].iov_len);
        if (len != 0) {
            std::memcpy(payload.data() + offset, msg->msg_iov[i].iov_base, len);
        }
        offset += len;
    }

    return SendPacket(payload.data(), static_cast<u32>(payload.size()), flags,
                      reinterpret_cast<const OrbisNetSockaddr*>(msg->msg_name), msg->msg_namelen);
}

int P2PSocket::SendPacket(const void* msg, u32 len, int flags, const OrbisNetSockaddr* to,
                          u32 tolen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return FailWith(ORBIS_NET_EOPNOTSUPP);
    }
    if (msg == nullptr && len != 0) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (len > kMaxP2PPayload) {
        return FailWith(ORBIS_NET_EMSGSIZE);
    }
    if (!EnsureP2PTransport()) {
        return FailWith(ORBIS_NET_ENETDOWN);
    }

    std::lock_guard lock(m_mutex);
    if (!valid) {
        return FailWith(ORBIS_NET_EBADF);
    }

    // UDP permits implicit local binding. Give an unbound guest socket an ephemeral vport.
    if (!bound) {
        u16 actual_vport = 0;
        if (!P2PTransport::Instance().RegisterSocket(this, 0, &actual_vport)) {
            return FailWith(ORBIS_NET_EADDRINUSE);
        }
        local_addr = {};
        local_addr.sin_len = sizeof(local_addr);
        local_addr.sin_family = ORBIS_NET_AF_INET;
        local_addr.sin_port = htons(P2PTransport::Instance().PhysicalPort());
        local_addr.sin_vport = actual_vport;
        bound = true;
    }

    OrbisNetSockaddrIn target{};
    if (to != nullptr) {
        if (tolen < sizeof(OrbisNetSockaddrIn)) {
            return FailWith(ORBIS_NET_EINVAL);
        }
        target = *reinterpret_cast<const OrbisNetSockaddrIn*>(to);
        if (target.sin_family != ORBIS_NET_AF_INET) {
            return FailWith(ORBIS_NET_EAFNOSUPPORT);
        }
    } else {
        if (!connected) {
            return FailWith(ORBIS_NET_ENOTCONN);
        }
        target = peer_addr;
    }

    // The port sends a peer nothing straight (phase 6): before the server link is up, nothing.
    if (!Link::SendsToPeer(P2PTransport::Instance().Server())) {
        return FailWith(ORBIS_NET_EHOSTUNREACH);
    }
    const int rc = P2PTransport::Instance().SendFrame(local_addr.sin_vport, target.sin_vport, msg,
                                                      len, target.sin_addr, target.sin_port);
    if (rc < 0) {
        return FailWith(ORBIS_NET_EHOSTUNREACH);
    }

    if (LogsPacket(len)) {
        LOG_INFO(Lib_Net, "P2P TX {} bytes vport {} -> {} dst={:#x}:{} t={} {}", len,
                 ntohs(local_addr.sin_vport), ntohs(target.sin_vport), target.sin_addr,
                 ntohs(target.sin_port), P2PNowMs(), HeadHex(static_cast<const u8*>(msg), len));
    }
    LOG_TRACE(Lib_Net, "P2P TX {} bytes vport {} -> {} dst={:#x}:{}", len,
              ntohs(local_addr.sin_vport), ntohs(target.sin_vport), target.sin_addr,
              ntohs(target.sin_port));
    return rc;
}

int P2PSocket::ReceiveMessage(OrbisNetMsghdr* msg, int flags) {
    if (msg == nullptr || msg->msg_iovlen < 0) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    size_t capacity = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        capacity += static_cast<size_t>(msg->msg_iov[i].iov_len);
        if (capacity > std::numeric_limits<u32>::max()) {
            return FailWith(ORBIS_NET_EMSGSIZE);
        }
    }

    std::vector<u8> payload(capacity);
    u32 name_len = msg->msg_namelen;
    const int rc = ReceivePacket(payload.data(), static_cast<u32>(payload.size()), flags,
                                 reinterpret_cast<OrbisNetSockaddr*>(msg->msg_name),
                                 msg->msg_name != nullptr ? &name_len : nullptr);
    if (rc < 0) {
        return rc;
    }
    msg->msg_namelen = name_len;
    msg->msg_flags = 0;

    size_t remaining = static_cast<size_t>(rc);
    size_t offset = 0;
    for (int i = 0; i < msg->msg_iovlen && remaining != 0; ++i) {
        const size_t amount =
            std::min<size_t>(remaining, static_cast<size_t>(msg->msg_iov[i].iov_len));
        if (amount != 0) {
            std::memcpy(msg->msg_iov[i].iov_base, payload.data() + offset, amount);
        }
        offset += amount;
        remaining -= amount;
    }
    return rc;
}

int P2PSocket::ReceivePacket(void* buf, u32 len, int flags, OrbisNetSockaddr* from, u32* fromlen) {
    if (buf == nullptr && len != 0) {
        return FailWith(ORBIS_NET_EFAULT);
    }

    std::unique_lock lock(receive_mutex);
    while (receive_queue.empty() && valid) {
        if (sockopt_so_nbio != 0 || (flags & ORBIS_NET_MSG_DONTWAIT) != 0) {
            return FailWith(ORBIS_NET_EWOULDBLOCK);
        }
        receive_cv.wait(lock);
    }

    if (!valid) {
        return FailWith(ORBIS_NET_EBADF);
    }

    const P2PDatagram& packet = receive_queue.front();
    const u32 amount = static_cast<u32>(std::min<size_t>(len, packet.payload.size()));
    if (amount != 0) {
        std::memcpy(buf, packet.payload.data(), amount);
    }

    if (from != nullptr && fromlen != nullptr) {
        if (*fromlen < sizeof(OrbisNetSockaddrIn)) {
            return FailWith(ORBIS_NET_EINVAL);
        }
        auto* out = reinterpret_cast<OrbisNetSockaddrIn*>(from);
        *out = {};
        out->sin_len = sizeof(OrbisNetSockaddrIn);
        out->sin_family = ORBIS_NET_AF_INET;
        out->sin_addr = packet.source_addr;
        out->sin_port = packet.source_port;
        out->sin_vport = packet.source_vport;
        *fromlen = sizeof(OrbisNetSockaddrIn);
    }

    const bool peek = (flags & ORBIS_NET_MSG_PEEK) != 0;
    const bool peek_len =
        (flags & ORBIS_NET_MSG_PEEKLEN) == static_cast<int>(ORBIS_NET_MSG_PEEKLEN);
    const int result =
        peek_len ? static_cast<int>(packet.payload.size()) : static_cast<int>(amount);

    if (!peek) {
        receive_queue.pop_front();
#ifdef _WIN32
        if (receive_queue.empty() && IsValidSocket(readiness_fd)) {
            char signal = 0;
            (void)::recv(readiness_fd, &signal, sizeof(signal), 0);
        }
#elif defined(__linux__) || defined(__APPLE__)
        if (receive_queue.empty() && readiness_fd >= 0) {
            u64 event_count = 0;
            (void)::read(readiness_fd, &event_count, sizeof(event_count));
        }
#endif
    }

    return result;
}

SocketPtr P2PSocket::Accept(OrbisNetSockaddr* addr, u32* addrlen) {
    LOG_WARNING(Lib_Net, "P2P STREAM accept not implemented");
    *Libraries::Kernel::__Error() = ORBIS_NET_EOPNOTSUPP;
    return nullptr;
}

int P2PSocket::Connect(const OrbisNetSockaddr* addr, u32 namelen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return FailWith(ORBIS_NET_EOPNOTSUPP);
    }
    if (addr == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (namelen < sizeof(OrbisNetSockaddrIn)) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    const auto* in = reinterpret_cast<const OrbisNetSockaddrIn*>(addr);
    if (in->sin_family != ORBIS_NET_AF_INET) {
        return FailWith(ORBIS_NET_EAFNOSUPPORT);
    }
    if (!EnsureP2PTransport()) {
        return FailWith(ORBIS_NET_ENETDOWN);
    }

    std::lock_guard lock(m_mutex);
    peer_addr = *in;
    connected = true;
    return 0;
}

int P2PSocket::GetSocketAddress(OrbisNetSockaddr* name, u32* namelen) {
    if (name == nullptr || namelen == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (*namelen < sizeof(OrbisNetSockaddrIn)) {
        return FailWith(ORBIS_NET_EINVAL);
    }

    auto out = local_addr;
    out.sin_len = sizeof(out);
    out.sin_family = ORBIS_NET_AF_INET;
    if (!bound && EnsureP2PTransport()) {
        out.sin_port = htons(GetP2PConfiguredPort());
    }
    std::memcpy(name, &out, sizeof(out));
    *namelen = sizeof(out);
    return 0;
}

int P2PSocket::GetPeerName(OrbisNetSockaddr* addr, u32* namelen) {
    if (!connected) {
        return FailWith(ORBIS_NET_ENOTCONN);
    }
    if (addr == nullptr || namelen == nullptr) {
        return FailWith(ORBIS_NET_EFAULT);
    }
    if (*namelen < sizeof(OrbisNetSockaddrIn)) {
        return FailWith(ORBIS_NET_EINVAL);
    }
    std::memcpy(addr, &peer_addr, sizeof(peer_addr));
    *namelen = sizeof(peer_addr);
    return 0;
}

int P2PSocket::fstat(Libraries::Kernel::OrbisKernelStat* stat) {
    LOG_DEBUG(Lib_Net, "P2P fstat called");
    return 0;
}

void P2PSocket::EnqueuePacket(P2PDatagram packet) {
    std::lock_guard lock(receive_mutex);
    if (!valid) {
        return;
    }

    const bool was_empty = receive_queue.empty();
    if (LogsPacket(packet.payload.size())) {
        LOG_INFO(Lib_Net, "P2P RX {} bytes vport {} <- {} src={:#x}:{} t={} {}",
                 packet.payload.size(), ntohs(local_addr.sin_vport), ntohs(packet.source_vport),
                 packet.source_addr, ntohs(packet.source_port), P2PNowMs(),
                 HeadHex(packet.payload.data(), packet.payload.size()));
    }
    LOG_TRACE(Lib_Net, "P2P RX {} bytes vport {} <- {} src={:#x}:{}", packet.payload.size(),
              ntohs(local_addr.sin_vport), ntohs(packet.source_vport), packet.source_addr,
              ntohs(packet.source_port));
    receive_queue.push_back(std::move(packet));

#ifdef _WIN32
    if (was_empty && IsValidSocket(readiness_signal_fd)) {
        constexpr char signal = 1;
        if (::send(readiness_signal_fd, &signal, sizeof(signal), 0) < 0) {
            LOG_WARNING(Lib_Net, "P2P readiness signal failed error={}", LastSocketError());
        }
    }
#elif defined(__APPLE__)
    if (was_empty && readiness_signal_fd>=0) {
        const u64 one=1;(void)::write(readiness_signal_fd,&one,sizeof(one));
    }
#elif defined(__linux__)
    if (was_empty && readiness_fd >= 0) {
        const u64 one = 1;
        (void)::write(readiness_fd, &one, sizeof(one));
    }
#endif
    receive_cv.notify_all();
}

bool ServerLinkSwitchOn() {
#ifdef BLOODBORNE_NATIVE_PORT
    // Always in the port (phase 6: no switch): the team's world lives on the server.
    return true;
#else
    return false; // the port only (docs/SERVER_COOP_PLAN.md)
#endif
}

void ConfigureServerLink(u32 server_addr_nbo, u16 server_port_nbo, std::string online_id,
                         std::string secret, std::string_view server_key) {
    // Every build (phase 9): a game reaches its peers through the server only.
    if (server_addr_nbo == 0 || server_port_nbo == 0) {
        return;
    }
    P2PTransport::Instance().ConfigureLink(server_addr_nbo, server_port_nbo, std::move(online_id),
                                           std::move(secret), server_key);
}

void SetServerLinkBuild(std::string build) {
    P2PTransport::Instance().SetLinkBuild(std::move(build));
}

void SetServerLinkObserver(ServerLinkObserver observer) {
    P2PTransport::Instance().SetLinkObserver(observer);
}

bool ServerCoopSend(const void* body, u32 len) {
    return ServerLinkSwitchOn() &&
           P2PTransport::Instance().SendToServer({static_cast<const u8*>(body), len});
}

int ServerCoopReceive(void* body, u32 len) {
    u32 addr = 0;
    u16 port = 0;
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::ServerCoop, body, len, &addr,
                                                    &port);
}

bool ServerRpcSend(const void* body, u32 len) {
    return P2PTransport::Instance().SendRpcToServer({static_cast<const u8*>(body), len});
}

int ServerRpcReceive(void* body, u32 len) {
    u32 addr = 0;
    u16 port = 0;
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::ServerRpc, body, len, &addr,
                                                    &port);
}

bool ServerLinkWelcomed() {
    return ServerLinkSwitchOn() && P2PTransport::Instance().LinkWelcomed();
}

bool ConnectsPeersWithoutHandshake() {
    return Link::ConnectsWithoutHandshake(P2PTransport::Instance().Server());
}

void CutServerLinkForTest(u32 ms) {
    P2PTransport::Instance().CutLink(ms);
}

std::string ServerLinkRefusal() {
    return ServerLinkSwitchOn() ? P2PTransport::Instance().LinkRefusal() : std::string{};
}

std::string ServerLinkWarning() {
    return ServerLinkSwitchOn() ? P2PTransport::Instance().LinkWarning() : std::string{};
}

u16 GetP2PConfiguredPort() {
    return P2PTransport::Instance().PhysicalPort();
}

u32 GetP2PAdvertisedAddr() {
    auto* netinfo = Common::Singleton<NetUtil::NetUtilInternal>::Instance();
    return netinfo->GetExternalIp();
}

bool EnsureP2PTransport() {
    return P2PTransport::Instance().Start();
}

bool P2PTransportIsReady() {
    return P2PTransport::Instance().IsReady();
}

int P2PSignalingSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return P2PTransport::Instance().SendInternal(data, len, dest_addr, dest_port);
}

int P2PSignalingRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::Signaling, buf, len, from_addr,
                                                    from_port);
}

int P2PControlSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return P2PTransport::Instance().SendInternal(data, len, dest_addr, dest_port);
}

int P2PControlRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::Control, buf, len, from_addr,
                                                    from_port);
}

int P2PCoopSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    if (!P2PTransport::Instance().IsReady()) {
        return -1; // no session has started the transport
    }
    return P2PTransport::Instance().SendInternal(data, len, dest_addr, dest_port);
}

int P2PGamePacketsRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::GamePackets, buf, len,
                                                    from_addr, from_port);
}

int P2PCoopRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return P2PTransport::Instance().ReceiveInternal(InternalChannel::Coop, buf, len, from_addr,
                                                    from_port);
}

bool FirewallAllows(u32 ipv4_nbo, u16 port_nbo, std::string_view what) {
    static const std::optional<NetFirewall> firewall = [] {
        const char* setting = std::getenv("SHADPS4_NET_FIREWALL");
        auto parsed = NetFirewall::Parse(setting != nullptr ? setting : "");
        if (parsed) {
            LOG_INFO(Lib_Net, "Net firewall: on, the game reaches only {}", setting);
        }
        return parsed;
    }();
    const u32 ipv4 = ntohl(ipv4_nbo);
    const u16 port = ntohs(port_nbo);
    if (!firewall || firewall->Allows(ipv4, port)) {
        return true;
    }
    static std::atomic<u32> blocked{0};
    const u32 count = blocked.fetch_add(1) + 1;
    if (count <= 20 || count % 1000 == 0) {
        LOG_WARNING(Lib_Net, "Net firewall: blocked {} {}.{}.{}.{}:{} ({} blocked so far)", what,
                    ipv4 >> 24, (ipv4 >> 16) & 0xFF, (ipv4 >> 8) & 0xFF, ipv4 & 0xFF, port, count);
    }
    return false;
}

} // namespace Libraries::Net
