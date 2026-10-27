// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <string>
#include <type_traits>

namespace BbSeamless {
// libc++'s floating from_chars requires macOS 26. Keep the wire format independent
// of the user's locale on the port's macOS 15 minimum as well.
template<class T> std::from_chars_result FloatChars(const char* first, const char* last, T& value) {
    static_assert(std::is_floating_point_v<T>);
    if (first == last || *first == '+' || *first == ' ' || (*first >= '\t' && *first <= '\r'))
        return {first, std::errc::invalid_argument};
    const std::string text(first, last);
    static locale_t locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    char* end = nullptr;
    const int saved_errno = errno;
    errno = 0;
    const T parsed = [&] {
        if constexpr (std::is_same_v<T, float>) return strtof_l(text.c_str(), &end, locale);
        else return strtod_l(text.c_str(), &end, locale);
    }();
    const int error = errno;
    errno = saved_errno;
    if (end == text.c_str()) return {first, std::errc::invalid_argument};
    if (error == ERANGE) return {first + (end - text.c_str()), std::errc::result_out_of_range};
    value = parsed;
    return {first + (end - text.c_str()), {}};
}
}
