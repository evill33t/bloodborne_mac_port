// SPDX-FileCopyrightText: Copyright 2026 Bloodborne Mac Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

// Negotiated link-v4 extension. Authenticate every fragment before calling Inbox::Take;
// only a complete Data datagram may enter the normal peer and channel routing checks.
namespace LinkFragments {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint8_t Kind = 12;
constexpr std::size_t ChunkBytes = 1120;
constexpr std::size_t MaxMessageBytes = 65507;
constexpr std::size_t MaxBufferedBytes = 256 * 1024;
constexpr std::size_t MaxMessages = 8;
constexpr std::int64_t LifetimeMs = 5000;
constexpr std::array<std::uint8_t, 9> Header{0xff, 0xff, 0xff, 0xff, 'S', 'H', 'L', 'K', Kind};
constexpr std::size_t MetadataBytes = 8 + 4 + 2;

inline bool IsData(std::span<const std::uint8_t> data) {
    return data.size() >= 15 && data[8] == 3 &&
           std::equal(Header.begin(), Header.begin() + 8, data.begin());
}

template <typename T> void Put(Bytes& bytes, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes.push_back(value >> (8 * i));
}
template <typename T> T Get(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= std::uint64_t{bytes[at + i]} << (8 * i);
    return static_cast<T>(value);
}

inline std::vector<Bytes> Split(std::span<const std::uint8_t> data, std::uint64_t id) {
    std::vector<Bytes> result;
    if (!IsData(data) || data.size() > MaxMessageBytes) return result;
    for (std::size_t at = 0; at < data.size(); at += ChunkBytes) {
        Bytes part{Header.begin(), Header.end()};
        Put(part, id);
        Put(part, static_cast<std::uint32_t>(data.size()));
        Put(part, static_cast<std::uint16_t>(at / ChunkBytes));
        const auto chunk = data.subspan(at, std::min(ChunkBytes, data.size() - at));
        part.insert(part.end(), chunk.begin(), chunk.end());
        result.push_back(std::move(part));
    }
    return result;
}

class Inbox {
public:
    void Clear() { messages.clear(); buffered = 0; }
    std::size_t BufferedBytes() const { return buffered; }
    void Expire(std::int64_t now) {
        for (auto it = messages.begin(); it != messages.end();) {
            if (now - it->second.started >= LifetimeMs) {
                buffered -= it->second.bytes.size();
                it = messages.erase(it);
            } else ++it;
        }
    }
    std::optional<Bytes> Take(std::span<const std::uint8_t> part, std::int64_t now) {
        Expire(now);
        constexpr auto prefix = Header.size() + MetadataBytes;
        if (part.size() <= prefix || !std::equal(Header.begin(), Header.end(), part.begin()))
            return std::nullopt;
        const auto id = Get<std::uint64_t>(part, Header.size());
        const auto size = Get<std::uint32_t>(part, Header.size() + 8);
        const auto index = Get<std::uint16_t>(part, Header.size() + 12);
        const std::size_t offset = std::size_t{index} * ChunkBytes;
        if (size < 15 || size > MaxMessageBytes || offset >= size ||
            part.size() - prefix != std::min(ChunkBytes, size - offset)) return std::nullopt;
        auto it = messages.find(id);
        if (it == messages.end()) {
            if (messages.size() >= MaxMessages || size > MaxBufferedBytes - buffered)
                return std::nullopt;
            it = messages.emplace(id, Message{Bytes(size),
                std::vector<bool>((size + ChunkBytes - 1) / ChunkBytes), 0, now}).first;
            buffered += size;
        }
        auto& message = it->second;
        const auto chunk = part.subspan(prefix);
        if (message.bytes.size() != size || (message.present[index] &&
            !std::equal(chunk.begin(), chunk.end(), message.bytes.begin() + offset))) {
            buffered -= message.bytes.size();
            messages.erase(it);
            return std::nullopt;
        }
        if (!message.present[index]) {
            std::copy(chunk.begin(), chunk.end(), message.bytes.begin() + offset);
            message.present[index] = true;
            message.received += chunk.size();
        }
        if (message.received != size) return std::nullopt;
        Bytes complete = std::move(message.bytes);
        buffered -= size;
        messages.erase(it);
        if (!IsData(complete)) return std::nullopt;
        return complete;
    }
private:
    struct Message {
        Bytes bytes;
        std::vector<bool> present;
        std::size_t received;
        std::int64_t started;
    };
    std::map<std::uint64_t, Message> messages;
    std::size_t buffered = 0;
};
} // namespace LinkFragments
