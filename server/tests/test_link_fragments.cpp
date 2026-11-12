// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include "coop/game_link.h"
#include "coop/link_fragments.h"

#define CHECK(expr) do { if (!(expr)) { std::cerr << "check failed at " << __LINE__ << ": " << #expr << "\n"; return 1; } } while (false)

using namespace LinkFragments;
using namespace BloodborneCoop;

Bytes Data(Bytes payload) {
    Bytes data(Header.begin(), Header.end());
    data[8] = 3;
    data.resize(15); // peer endpoint; its value is immaterial to this unit
    data.insert(data.end(), payload.begin(), payload.end());
    return data;
}

int main() {
    // A single lost large AI message blocks subsequent reliable equipment updates.
    // The path drops UDP datagrams over 1200 bytes, as observed on the deployed host.
    Bytes ai;
    GameLinkDetail::Put(ai, std::uint32_t{0x3d});
    GameLinkDetail::Put(ai, std::uint32_t{1600});
    ai.resize(1608, 0x31);
    GameLinkPeer host{"host", 1}, guest{"guest", 2};
    host.Send(PacketClass::Reliable, ai, 0);
    auto large = host.Flush(0).at(0);
    CHECK(EncodeGameFrame(large).size() > 1200 && host.Unacked() == 1);
    Bytes gear(136, 0);
    gear[0] = 0x0c; gear[4] = 128;
    host.Send(PacketClass::Reliable, gear, 1);
    for (const auto& frame : host.Flush(1)) CHECK(guest.Receive(frame, 1).empty());
    auto plain = Data(EncodeGameFrame(large));
    Inbox inbox;
    auto parts = Split(plain, 7);
    CHECK(parts.size() == 2);
    CHECK(!inbox.Take(parts[1], 2)); // reordered and duplicated
    CHECK(!inbox.Take(parts[1], 3));
    auto joined = inbox.Take(parts[0], 4);
    CHECK(joined && *joined == plain && inbox.BufferedBytes() == 0);
    for (const auto& part : parts) CHECK(part.size() + 24 + 28 <= 1200);
    auto frame = DecodeGameFrame(std::span{*joined}.subspan(15));
    CHECK(frame);
    const auto delivered = guest.Receive(*frame, 4);
    CHECK(delivered.size() == 2 && delivered[0] == ai && delivered[1] == gear);
    for (const auto& ack : guest.Flush(5)) host.Receive(ack, 5);
    CHECK(host.Unacked() == 0);

    // Bounds, malformed indices, truncated chunks, conflicting duplicates and timeout.
    auto bad = parts[0]; bad.pop_back();
    CHECK(!inbox.Take(bad, 10) && inbox.BufferedBytes() == 0);
    bad = parts[0]; bad[21] = 0xff; bad[22] = 0xff;
    CHECK(!inbox.Take(bad, 10) && inbox.BufferedBytes() == 0);
    CHECK(!inbox.Take(parts[0], 10));
    bad = parts[0]; bad.back() ^= 1;
    CHECK(!inbox.Take(bad, 11) && inbox.BufferedBytes() == 0);
    CHECK(!inbox.Take(parts[0], 12));
    inbox.Expire(12 + LifetimeMs);
    CHECK(inbox.BufferedBytes() == 0);
    CHECK(!inbox.Take(parts[1], 13 + LifetimeMs));
    inbox.Clear();
    CHECK(!inbox.BufferedBytes());

    const auto huge = Data(Bytes(MaxMessageBytes - 15, 0x40));
    for (std::uint64_t id = 1; id <= 10; ++id) {
        const auto chunks = Split(huge, id);
        CHECK(chunks.size() < 64);
        CHECK(!inbox.Take(chunks[0], 0));
        CHECK(inbox.BufferedBytes() <= MaxBufferedBytes);
    }
    inbox.Expire(LifetimeMs);
    CHECK(!inbox.BufferedBytes());
    CHECK(Split(Data(Bytes(MaxMessageBytes, 0)), 1).empty());
    bad = plain; bad[8] = 7; // fragments cannot carry a hello, RPC or co-op control message
    CHECK(Split(bad, 1).empty());
    std::cout << "PASS: MTU loss, reliable AI/equipment ordering, duplicates, bounds and expiry\n";
}
