// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// One target of fuzz_targets.h under libFuzzer (SHADNET_FUZZ=ON):
// SHADNET_FUZZ_TARGET names it, SHADNET_FUZZ_SEEDS its seeds. `<binary>
// -seeds=<folder>` writes the seeds there as a corpus. Run with
// ASAN_OPTIONS=detect_container_overflow=0: protobuf and Qt are not
// instrumented, and their inline container code trips false overflows.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "fuzz_targets.h"

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) {
  ShadNetFuzz::Quiet();
  for (int index = 1; index < *argc; ++index) {
    const char *arg = (*argv)[index];
    if (std::strncmp(arg, "-seeds=", 7) == 0) {
      int number = 0;
      for (const ShadNetFuzz::Bytes &seed : ShadNetFuzz::SHADNET_FUZZ_SEEDS()) {
        std::ofstream file(std::string(arg + 7) + "/seed" +
                               std::to_string(number++),
                           std::ios::binary);
        file.write(reinterpret_cast<const char *>(seed.data()),
                   static_cast<std::streamsize>(seed.size()));
      }
      std::exit(0);
    }
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  ShadNetFuzz::SHADNET_FUZZ_TARGET({data, size});
  return 0;
}
