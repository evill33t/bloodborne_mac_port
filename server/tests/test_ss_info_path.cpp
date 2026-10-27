// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Every region's game gets ss.info: a US game asks /bb-us/ss.info, and a server
// that knew only /bb-eu/ss.info answered 404 (players 2026-10-22).
#include <iostream>

#include "ss_info_path.h"

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

using Bloodborne::Backend::IsSsInfoPath;

} // namespace

int main() {
  for (const char *path :
       {"/bb-eu/ss.info", "/bb-us/ss.info", "/bb-jp/ss.info", "/bb-asia/ss.info"}) {
    CHECK(IsSsInfoPath(path));
  }
  for (const char *path :
       {"", "/ss.info", "/bb-/ss.info", "/bb-us/ss.infox", "/bb-us/x/ss.info",
        "/bb-US1/ss.info", "/bb-eu/ss.info/", "/basic_utils/login",
        "/bb-toolongname/ss.info"}) {
    CHECK(!IsSsInfoPath(path));
  }
  return 0;
}
