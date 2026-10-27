// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 9: the channel table and the enemy owner rules shadNet routes every
// game packet by (coop/packet_routes.h).
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "coop/packet_routes.h"

using namespace BloodborneCoop;

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

constexpr u32 CentralYharnam = 0x18010000;
constexpr u32 CathedralWard = 0x18000000;

std::vector<u8> Packet(u32 channel, std::vector<u8> payload = {1, 2, 3}) {
  std::vector<u8> packet;
  GameLinkDetail::Put(packet, channel);
  GameLinkDetail::Put(packet, static_cast<u32>(payload.size()));
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

bool Routes(u32 channel, u32 sender_map, u32 target_map,
            EnemyPart sender = EnemyPart::None,
            EnemyPart target = EnemyPart::None) {
  return RoutesNativePacket(channel, {sender_map, target_map, sender, target});
}

// docs/SERVER_COOP_PLAN.md "Traffic and who owns it", one row per area.
int EveryChannelHasThePlansPolicy() {
  // The join handshake and its snapshots, character data, announcements: the
  // game's own.
  for (const u32 channel :
       {0x07u, 0x19u, 0x42u, 0x10u, 0x11u, 0x3Fu, 0x08u, 0x0Au, 0x0Bu, 0x0Cu,
        0x0Du, 0x13u, 0x3Bu, 0x3Cu, 0x43u, 0x32u, 0x33u, 0x00u, 0x22u}) {
    CHECK(NativeChannelPolicy(channel) == ChannelPolicy::Pass);
  }
  // Player state, hits, throws, forced animations, bullets, enemies, events,
  // objects: only between members on one map.
  for (const u32 channel :
       {0x04u, 0x05u, 0x06u, 0x14u, 0x12u, 0x37u, 0x30u, 0x24u, 0x25u,
        0x3Eu, 0x40u, 0x15u, 0x16u, 0x17u, 0x29u, 0x36u, 0x3Du, 0x41u,
        0x34u, 0x0Eu, 0x0Fu, 0x31u, 0x2Au, 0x2Bu, 0x2Du, 0x2Eu}) {
    CHECK(NativeChannelPolicy(channel) == ChannelPolicy::SameMap);
  }
  CHECK(NativeChannelPolicy(0x2C) == ChannelPolicy::Ours);
  CHECK(NativeChannelPolicy(0x2F) == ChannelPolicy::Ours);
  CHECK(NativeChannelPolicy(0x27) == ChannelPolicy::Drop);
  CHECK(ChannelPolicyName(ChannelPolicy::Pass) == "pass");
  CHECK(ChannelPolicyName(ChannelPolicy::SameMap) == "same map");
  CHECK(ChannelPolicyName(ChannelPolicy::Ours) == "ours");
  CHECK(ChannelPolicyName(ChannelPolicy::Drop) == "drop");
  return 0;
}

int PassedChannelsAlwaysGoOursAndDroppedNever() {
  CHECK(Routes(0x10, CentralYharnam, CathedralWard));
  CHECK(Routes(0x0A, 0, 0));
  for (const u32 channel : {0x2Cu, 0x2Fu, 0x27u}) {
    CHECK(!Routes(channel, CentralYharnam, CentralYharnam));
    CHECK(!Routes(channel, 0, 0));
  }
  // Not a channel of the game's: nobody's.
  CHECK(!Routes(0x44, CentralYharnam, CentralYharnam));
  CHECK(!Routes(0xFFFF, 0, 0));
  return 0;
}

int SameMapChannelsGoOnlyBetweenMembersOnOneMap() {
  CHECK(Routes(0x06, CentralYharnam, CentralYharnam));
  CHECK(!Routes(0x06, CentralYharnam, CathedralWard));
  CHECK(!Routes(0x31, CathedralWard, CentralYharnam));
  // A map not known (a load, or not said yet): the game's own behavior.
  CHECK(Routes(0x06, CentralYharnam, 0));
  CHECK(Routes(0x06, 0, CathedralWard));
  return 0;
}

int TheEnemiesComeFromTheOwnerOnly() {
  const auto O = EnemyPart::Owner;
  const auto F = EnemyPart::Follower;
  const auto N = EnemyPart::None;
  const u32 Y = CentralYharnam;
  for (const u32 channel : {0x05u, 0x29u, 0x36u}) {
    CHECK(Routes(channel, Y, Y, O, F));
    CHECK(!Routes(channel, Y, Y, F, O));
    CHECK(!Routes(channel, Y, Y, F, F));
    CHECK(Routes(channel, Y, Y, N, N)); // no owner: the game's own way
    // Still one map only.
    CHECK(!Routes(channel, Y, CathedralWard, O, F));
  }
  // Positions come from whichever game runs the enemy (Kaniel 2026-10-15: a
  // guest runs the enemies only it is near, the game's own hand-out): the
  // owner and the followers, to everyone on the map.
  CHECK(Routes(0x04, Y, Y, O, F));
  CHECK(Routes(0x04, Y, Y, F, O));
  CHECK(Routes(0x04, Y, Y, F, F));
  CHECK(Routes(0x04, Y, Y, N, N));
  CHECK(!Routes(0x04, Y, CathedralWard, F, O));
  // A follower's hit and near list go to the owner, never the owner's to
  // anyone; the owner's answer (0x17, which enemies a follower runs) goes to
  // the followers only.
  CHECK(Routes(0x15, Y, Y, F, O));
  CHECK(!Routes(0x15, Y, Y, O, F));
  CHECK(!Routes(0x15, Y, Y, F, F));
  CHECK(Routes(0x15, Y, Y, N, N));
  CHECK(Routes(0x16, Y, Y, F, O));
  CHECK(!Routes(0x16, Y, Y, F, F));
  CHECK(!Routes(0x16, Y, Y, O, F));
  CHECK(!Routes(0x16, Y, Y, N, N));
  CHECK(Routes(0x17, Y, Y, O, F));
  CHECK(!Routes(0x17, Y, Y, F, O));
  CHECK(!Routes(0x17, Y, Y, F, F));
  CHECK(!Routes(0x17, Y, Y, N, N));
  CHECK(!Routes(0x17, Y, CathedralWard, O, F));
  // Everything else on the map goes between any two members there.
  for (const u32 channel : {0x06u, 0x14u, 0x3Du, 0x41u}) {
    CHECK(Routes(channel, Y, Y, F, F));
    CHECK(Routes(channel, Y, Y, O, F));
  }
  return 0;
}

// An unreliable packet refused leaves the frame; a reliable one keeps its
// sequence number, so the receiver's stream never waits for it.
int ARefusedReliablePacketKeepsItsPlace() {
  GameFrame frame{"Ada", 7, 9, 3, {}};
  frame.entries.push_back({PacketClass::Unreliable, 0, Packet(0x06)});
  frame.entries.push_back({PacketClass::Unreliable, 0, Packet(0x05)});
  frame.entries.push_back({PacketClass::Reliable, 11, Packet(0x31)});
  frame.entries.push_back({PacketClass::Reliable, 12, Packet(0x10)});
  frame.entries.push_back({PacketClass::Reliable, 13, Packet(0x2C)});
  const size_t refused = RouteGameFrame(
      frame, [](u32 channel) { return channel == 0x05 || channel == 0x10; });
  CHECK(refused == 3);
  CHECK(frame.sender == "Ada" && frame.ack == 3 && frame.instance == 7);
  CHECK(frame.entries.size() == 4);
  CHECK(frame.entries[0].packet == Packet(0x05));
  CHECK(frame.entries[1].seq == 11);
  CHECK(frame.entries[1].packet_class == PacketClass::Reliable);
  CHECK(frame.entries[1].packet == DroppedNativePacket());
  CHECK(NativePacketChannel(frame.entries[1].packet) == DroppedNativeChannel);
  CHECK(frame.entries[2].packet == Packet(0x10));
  CHECK(frame.entries[3].seq == 13);
  CHECK(frame.entries[3].packet == DroppedNativePacket());
  // A frame read back still decodes, and a dropped one stays dropped.
  const auto again = DecodeGameFrame(EncodeGameFrame(frame));
  CHECK(again && *again == frame);
  GameFrame resent = frame;
  CHECK(RouteGameFrame(resent, [](u32) { return true; }) == 0);
  CHECK(resent == frame);
  return 0;
}

// Invasions (docs/INVASIONS.md): a hostile visitor's packets, by the design's
// table. `from_visitor`: the invader sends; otherwise a team member sends to
// it.
bool Hostile(u32 channel, bool from_visitor, u32 sender_map, u32 target_map,
             EnemyPart sender = EnemyPart::None,
             EnemyPart target = EnemyPart::None) {
  return RoutesHostilePacket(channel, {sender_map, target_map, sender, target},
                             from_visitor);
}

int AHostileVisitorGetsOnlyTheDesignsChannels() {
  // The join handshake and character data: both ways, any map.
  for (const u32 channel : {0x07u, 0x19u, 0x10u, 0x11u, 0x3Fu, 0x42u, 0x08u,
                            0x0Au, 0x0Du, 0x13u, 0x3Bu, 0x3Cu, 0x43u}) {
    CHECK(HostileChannelPolicy(channel) == HostilePolicy::Pass);
    CHECK(Hostile(channel, true, CentralYharnam, CathedralWard));
    CHECK(Hostile(channel, false, CentralYharnam, CathedralWard));
  }
  // Players, hits on players, throws, forced animations, bullets: one map
  // only, and a map not known yet lets nothing through (strict).
  for (const u32 channel :
       {0x06u, 0x14u, 0x12u, 0x37u, 0x30u, 0x24u, 0x25u, 0x3Eu, 0x40u}) {
    CHECK(HostileChannelPolicy(channel) == HostilePolicy::SameMap);
    CHECK(Hostile(channel, true, CentralYharnam, CentralYharnam));
    CHECK(Hostile(channel, false, CentralYharnam, CentralYharnam));
    CHECK(!Hostile(channel, true, CentralYharnam, CathedralWard));
    CHECK(!Hostile(channel, false, CathedralWard, CentralYharnam));
    CHECK(!Hostile(channel, true, 0, CentralYharnam));
    CHECK(!Hostile(channel, false, CentralYharnam, 0));
  }
  // The enemies: to the invader from its map's owner only, never from it.
  for (const u32 channel : {0x04u, 0x05u, 0x29u, 0x36u, 0x3Du, 0x41u, 0x17u}) {
    CHECK(HostileChannelPolicy(channel) == HostilePolicy::OwnersEnemies);
    CHECK(Hostile(channel, false, CentralYharnam, CentralYharnam,
                  EnemyPart::Owner));
    CHECK(!Hostile(channel, false, CentralYharnam, CentralYharnam,
                   EnemyPart::Follower));
    CHECK(!Hostile(channel, false, CentralYharnam, CentralYharnam));
    CHECK(!Hostile(channel, false, CathedralWard, CentralYharnam,
                   EnemyPart::Owner));
    CHECK(!Hostile(channel, true, CentralYharnam, CentralYharnam,
                   EnemyPart::None, EnemyPart::Owner));
  }
  // Enemy hits and hand-over, events, objects, items, Insight: never.
  for (const u32 channel : {0x15u, 0x16u, 0x34u, 0x0Eu, 0x0Fu, 0x31u, 0x2Du,
                            0x2Eu, 0x2Au, 0x2Bu, 0x2Cu, 0x2Fu, 0x27u, 0x39u}) {
    CHECK(HostileChannelPolicy(channel) == HostilePolicy::Drop);
    CHECK(!Hostile(channel, true, CentralYharnam, CentralYharnam,
                   EnemyPart::None, EnemyPart::Owner));
    CHECK(!Hostile(channel, false, CentralYharnam, CentralYharnam,
                   EnemyPart::Owner));
  }
  CHECK(!Hostile(NativeChannelCount, false, CentralYharnam, CentralYharnam));
  return 0;
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, int (*)()>> tests{
      {"EveryChannelHasThePlansPolicy", EveryChannelHasThePlansPolicy},
      {"PassedChannelsAlwaysGoOursAndDroppedNever",
       PassedChannelsAlwaysGoOursAndDroppedNever},
      {"SameMapChannelsGoOnlyBetweenMembersOnOneMap",
       SameMapChannelsGoOnlyBetweenMembersOnOneMap},
      {"TheEnemiesComeFromTheOwnerOnly", TheEnemiesComeFromTheOwnerOnly},
      {"ARefusedReliablePacketKeepsItsPlace",
       ARefusedReliablePacketKeepsItsPlace},
      {"AHostileVisitorGetsOnlyTheDesignsChannels",
       AHostileVisitorGetsOnlyTheDesignsChannels},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    const int result = test();
    std::cout << (result == 0 ? "PASS " : "FAIL ") << name << '\n';
    failed += result != 0 ? 1 : 0;
  }
  return failed == 0 ? 0 : 1;
}
