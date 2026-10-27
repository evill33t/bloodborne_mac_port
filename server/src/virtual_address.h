// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>

// Phase 7: the virtual address 10.x.y.z (host byte order) a game is known by, x.y.z from a 24-bit
// FNV-1a of its online id. The games compute the same (emulator net_link.h VirtualAddressOf), so
// this never changes; two accounts can share one, which the accounts refuse for a new one
// (database.cpp).
inline quint32 VirtualAddressOf(const QByteArray& online_id) {
    quint32 hash = 2166136261u; // FNV-1a
    for (const char c : online_id) {
        hash = (hash ^ static_cast<quint8>(c)) * 16777619u;
    }
    quint32 host = hash & 0xFFFFFF;
    if ((host & 0xFF) == 0 || (host & 0xFF) == 0xFF) {
        host ^= 0x01; // no .0 or .255
    }
    return (10u << 24) | host;
}
