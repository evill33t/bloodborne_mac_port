// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// ss.info sends each game back to the address it used: the Host header's IPv4
// address, nothing else.
#include <iostream>

#include "request_address.h"

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

using Bloodborne::Backend::AddressFromHostHeader;

} // namespace

int main() {
  CHECK(AddressFromHostHeader("203.0.113.7:20443") == "203.0.113.7");
  CHECK(AddressFromHostHeader("26.1.2.3") == "26.1.2.3");
  CHECK(AddressFromHostHeader(" 192.168.4.25:20443 ") == "192.168.4.25");
  // A name (the game's own host), IPv6 or junk: the server's own address is
  // used instead.
  for (const char *value :
       {"", "ss4.scej-network.jp:20443", "999.1.2.3:20443", "[::1]:20443",
        "1.2.3.4:20443/evil", "1.2.3.4 :20443", "1.2.3.4@x", "1.2.3:20443",
        "1.2.3.4.5", "1..2.3", "1.2.3.4:", "1.2.3.4:123456", "1.2.3.4:2a"}) {
    CHECK(AddressFromHostHeader(value).empty());
  }
  return 0;
}
