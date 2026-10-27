// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gpu/bbseamless/ui.h"
#include "host/bbgpu_shared.h"
union SDL_Event;
namespace BbCoopOverlay {
struct SnapshotRecord { BbGpuRecord r; BbCoopUi view; };
struct CommandRecord { BbGpuRecord r; BbCoopCommand command; };
struct ModelRecord {
    BbGpuRecord r;
    uint64_t sequence;
    uint32_t offset, count;
    uint8_t pixels[65536];
};
bool Dispatch(const BbGpuRecord* record);
void Receive(const BbGpuRecord* record);
void ReceiveModel(const BbGpuRecord* record);
bool WantsPortrait();
void PublishModel(const uint8_t* rgba, uint32_t bytes);
bool Visible();
bool CapturesInput();
bool Shortcut(const SDL_Event& event);
bool MenuButtons();
void Render();
}
