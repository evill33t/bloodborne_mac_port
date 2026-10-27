// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The fuzz targets (tests/fuzz/fuzz_targets.h) without libFuzzer: each on its
// seeds, on random inputs and on mutated seeds, from a fixed seed, so every
// build catches a decoder that crashes, hangs or trips a sanitizer on bytes an
// attacker sends. SHADNET_FUZZ_ROUNDS sets the inputs per target. The inputs
// libFuzzer found crashing are in tests/fuzz/regressions (each run first).
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

#include "fuzz/fuzz_targets.h"

using namespace ShadNetFuzz;

namespace {

using Target = std::function<void(std::span<const std::uint8_t>)>;

Bytes Mutated(std::mt19937 &random, const std::vector<Bytes> &seeds) {
  Bytes input = seeds[random() % seeds.size()];
  const int changes = 1 + static_cast<int>(random() % 8);
  for (int change = 0; change < changes; ++change) {
    const size_t at = input.empty() ? 0 : random() % input.size();
    switch (random() % 7) {
    case 0: // a bit
      if (!input.empty())
        input[at] ^= static_cast<std::uint8_t>(1u << (random() % 8));
      break;
    case 1: // a byte
      if (!input.empty())
        input[at] = static_cast<std::uint8_t>(random());
      break;
    case 2: { // an edge value
      constexpr std::uint8_t Edges[] = {0, 1, 0x7F, 0x80, 0xFE, 0xFF};
      if (!input.empty())
        input[at] = Edges[random() % std::size(Edges)];
      break;
    }
    case 3: { // bytes in
      const size_t count = 1 + random() % 16;
      for (size_t index = 0; index < count; ++index)
        input.insert(input.begin() + static_cast<std::ptrdiff_t>(at),
                     static_cast<std::uint8_t>(random()));
      break;
    }
    case 4: { // bytes out
      const size_t count =
          std::min<size_t>(1 + random() % 32, input.size() - at);
      input.erase(input.begin() + static_cast<std::ptrdiff_t>(at),
                  input.begin() + static_cast<std::ptrdiff_t>(at + count));
      break;
    }
    case 5: { // a part again
      const size_t count =
          std::min<size_t>(1 + random() % 64, input.size() - at);
      const Bytes part(input.begin() + static_cast<std::ptrdiff_t>(at),
                       input.begin() + static_cast<std::ptrdiff_t>(at + count));
      input.insert(input.begin() + static_cast<std::ptrdiff_t>(
                                       random() % (input.size() + 1)),
                   part.begin(), part.end());
      break;
    }
    default: { // another seed's tail
      const Bytes &other = seeds[random() % seeds.size()];
      if (!other.empty()) {
        input.resize(at);
        const size_t from = random() % other.size();
        input.insert(input.end(),
                     other.begin() + static_cast<std::ptrdiff_t>(from),
                     other.end());
      }
      break;
    }
    }
  }
  return input;
}

Bytes Random(std::mt19937 &random) {
  Bytes input(random() % 600);
  for (std::uint8_t &byte : input)
    byte = static_cast<std::uint8_t>(random());
  return input;
}

int Run(const char *name, const Target &target, const std::vector<Bytes> &seeds,
        int rounds) {
  const auto start = std::chrono::steady_clock::now();
  const std::filesystem::path regressions =
      std::filesystem::path(SHADNET_FUZZ_REGRESSIONS) / name;
  int replayed = 0;
  if (std::filesystem::is_directory(regressions)) {
    for (const auto &file : std::filesystem::directory_iterator(regressions)) {
      std::ifstream in(file.path(), std::ios::binary);
      const Bytes input{std::istreambuf_iterator<char>(in), {}};
      target(input);
      ++replayed;
    }
  }
  for (const Bytes &seed : seeds)
    target(seed);
  std::mt19937 random(0x5EED0000u +
                      static_cast<unsigned>(std::string(name).size()));
  for (int round = 0; round < rounds; ++round)
    target(round % 4 == 0 ? Random(random) : Mutated(random, seeds));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  std::cout << name << ": " << replayed << " regressions, " << seeds.size()
            << " seeds, " << rounds << " inputs, " << ms << " ms\n";
  return 0;
}

} // namespace

int main() {
  Quiet();
  int rounds = 5000;
  if (const char *text = std::getenv("SHADNET_FUZZ_ROUNDS"))
    rounds = std::atoi(text);
  Run("link", LinkDatagrams, LinkSeeds(), rounds);
  Run("game_frame", GameFrames, GameFrameSeeds(), rounds);
  Run("world", WorldMessages, WorldSeeds(), rounds);
  Run("session", SessionPackets, SessionSeeds(), rounds);
  std::cout << "fuzz decoders: all ran\n";
  return 0;
}
