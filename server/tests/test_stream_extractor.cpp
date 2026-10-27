// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// A size read from a packet never reads past the packet, however large.
#include <iostream>

#include "stream_extractor.h"

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

QByteArray Blob(uint32_t size, const QByteArray &after) {
  QByteArray data(4, '\0');
  std::memcpy(data.data(), &size, 4); // little-endian, as the hosts it runs on
  return data + after;
}

} // namespace

int main() {
  {
    StreamExtractor in{Blob(3, "abc")};
    CHECK(in.getRawData() == "abc");
    CHECK(!in.error());
  }
  {
    StreamExtractor in{Blob(4, "abc")};
    CHECK(in.getRawData().isEmpty());
    CHECK(in.error());
  }
  for (const uint32_t size :
       {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu}) {
    StreamExtractor in{Blob(size, "abc")};
    CHECK(in.getRawData().isEmpty());
    CHECK(in.error());
  }
  {
    StreamExtractor in{QByteArray("abc")};
    CHECK(in.getBytes(-1).isEmpty());
    CHECK(in.error());
  }
  return 0;
}
