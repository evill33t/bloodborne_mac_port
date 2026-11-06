// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: a setting from the environment as UTF-8, the server's encoding. On Windows getenv
// gives the PC's code page, so a name with other letters did not match (from Amanises 2026-10-26).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

namespace BbEnv {

// UTF-16 on Windows; half a surrogate pair becomes U+FFFD.
inline std::string WideToUtf8(const wchar_t* wide) {
    std::string out;
    for (const wchar_t* at = wide; *at; ++at) {
        std::uint32_t code = static_cast<std::uint32_t>(*at);
        if (sizeof(wchar_t) == 2 && code >= 0xD800 && code < 0xE000) {
            const std::uint32_t next = static_cast<std::uint32_t>(at[1]);
            if (code < 0xDC00 && next >= 0xDC00 && next < 0xE000) {
                code = 0x10000 + ((code - 0xD800) << 10) + (next - 0xDC00);
                ++at;
            } else {
                code = 0xFFFD;
            }
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

// `name`'s value, `fallback` when it is not set or empty.
inline std::string Text(const char* name, const char* fallback) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const wchar_t* wide = _wgetenv(wide_name.c_str());
    return wide && wide[0] ? WideToUtf8(wide) : std::string{fallback};
#else
    const char* value = std::getenv(name);
    return std::string{value && value[0] ? value : fallback};
#endif
}

} // namespace BbEnv
