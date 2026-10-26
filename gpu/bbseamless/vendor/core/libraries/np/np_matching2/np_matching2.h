// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/np/np_types2.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpMatching2 {

int PS4_SYSV_ABI sceNpMatching2Initialize(OrbisNpMatching2InitializeParameter* param);
int PS4_SYSV_ABI sceNpMatching2Terminate();
int PS4_SYSV_ABI sceNpMatching2CreateContext(const OrbisNpMatching2CreateContextParameter* param,
                                             OrbisNpMatching2ContextId* ctxId);
int PS4_SYSV_ABI sceNpMatching2CreateContextA(const OrbisNpMatching2CreateContextParameterA* param,
                                              OrbisNpMatching2ContextId* ctxId);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
// Phase 7: whether this game owns the room it is in (the server passes a room to a guest when
// its owner leaves: ROOM_OWNER_CHANGED).
bool OwnsItsRoom();
// Whether that room holds another member besides this game (the owner who left not counted).
bool OwnedRoomHasOthers();
} // namespace Libraries::Np::NpMatching2
