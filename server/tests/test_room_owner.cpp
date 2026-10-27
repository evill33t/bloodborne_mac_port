// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 7, host migration: a room whose owner leaves passes ownership to one
// member the same way every time (its successor list first, else the member
// who joined first: the lowest member id), and the leaving game's session host
// role goes with it (ROOM_OWNER_CHANGED to the others).
#include <iostream>

#include "matching_types.h"

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

void Add(Room &room, uint16_t id) {
  RoomMember member;
  member.memberId = id;
  room.members.insert(id, member);
}

int OwnershipPassesToTheFirstJoiner() {
  Room room;
  for (const uint16_t id : {0x31, 0x13, 0x22}) {
    Add(room, id);
  }
  room.ownerMemberId = 0x13;
  room.members.remove(0x13);
  CHECK(room.PassOwnership() == 0x22);
  CHECK(room.ownerMemberId == 0x22);
  CHECK(room.findById(0x22)->flagAttr &
        Matching2::ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER);
  // A successor named by the owner comes first.
  room.ownerSuccession = {0x40, 0x31};
  room.members.remove(0x22);
  CHECK(room.PassOwnership() == 0x31);
  // Nobody left: no owner.
  room.members.clear();
  CHECK(room.PassOwnership() == 0);
  return 0;
}

} // namespace

int main() {
  if (OwnershipPassesToTheFirstJoiner() != 0) {
    return 1;
  }
  std::cout << "room owner: ok\n";
  return 0;
}
