// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// What the fuzz tests feed attacker bytes to: the server link's datagrams, the
// game frames in them, our messages (DecodeEnvelope -> Service::Take) and the
// TCP sessions' packets. libfuzzer_main.cpp runs one under libFuzzer
// (SHADNET_FUZZ=ON, clang); tests/test_fuzz_decoders.cpp runs them all on
// seeded random and mutated inputs in every build.
//
// An input is a list of chunks: a selector byte, a u16 little-endian size, that
// many bytes (fewer at the end). What a selector means is each target's.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ShadNetFuzz {

using Bytes = std::vector<std::uint8_t>;

class Chunks {
public:
  explicit Chunks(std::span<const std::uint8_t> input) : input{input} {}

  bool Next(std::uint8_t &selector, std::span<const std::uint8_t> &bytes) {
    if (at >= input.size()) {
      return false;
    }
    selector = input[at++];
    std::size_t size = 0;
    if (input.size() - at >= 2) {
      size = input[at] | (std::size_t{input[at + 1]} << 8);
      at += 2;
    } else {
      at = input.size();
    }
    size = (std::min)(size, input.size() - at);
    bytes = input.subspan(at, size);
    at += size;
    return true;
  }

private:
  std::span<const std::uint8_t> input;
  std::size_t at = 0;
};

inline void PutChunk(Bytes &input, std::uint8_t selector,
                     std::span<const std::uint8_t> bytes) {
  const std::size_t size = (std::min<std::size_t>)(bytes.size(), 0xFFFF);
  input.push_back(selector);
  input.push_back(static_cast<std::uint8_t>(size));
  input.push_back(static_cast<std::uint8_t>(size >> 8));
  input.insert(input.end(), bytes.begin(),
               bytes.begin() + static_cast<std::ptrdiff_t>(size));
}

// Server link datagrams into a Hub (server_link.h), every decoder on each, the
// Rpc stream's bodies into a tunnel each (coop/rpc_tunnel.h).
void LinkDatagrams(std::span<const std::uint8_t> input);
// DecodeGameFrame (coop/game_link.h), its round trip, and frames into a
// CoopStream.
void GameFrames(std::span<const std::uint8_t> input);
// Coop datagram bodies and our messages into a BloodborneWorld::Service.
void WorldMessages(std::span<const std::uint8_t> input);
// Bytes from a TCP client into a ClientSession, signed in or not.
void SessionPackets(std::span<const std::uint8_t> input);

// Well-formed inputs of each, for the mutations (and libFuzzer's seed corpus).
std::vector<Bytes> LinkSeeds();
std::vector<Bytes> GameFrameSeeds();
std::vector<Bytes> WorldSeeds();
std::vector<Bytes> SessionSeeds();

// Qt's log lines off (the targets log every refusal).
void Quiet();

} // namespace ShadNetFuzz
