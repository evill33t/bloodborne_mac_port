// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_coop_ui.h"
#include "bbport_split.h"
#include <cstdio>
#include <deque>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
namespace {
std::mutex command_mutex;
std::deque<BbCoopCommand> commands;
std::mutex model_mutex;
uint64_t model_sequence=0;
uint32_t model_used=0;
std::vector<uint8_t> assembling,completed;
}
namespace BbCoopOverlay {
void Receive(const BbGpuRecord* record) {
    if (record->bytes != sizeof(CommandRecord)) return;
    auto command = reinterpret_cast<const CommandRecord*>(record)->command;
    if (command.version != BB_COOP_UI_VERSION || command.action < BB_COOP_LOBBY ||
        command.action > BB_COOP_PREFERENCES) return;
    command.text[sizeof(command.text)-1] = 0;
    std::scoped_lock lock{command_mutex};
    if (commands.size() < 64) commands.push_back(command);
    else std::fprintf(stderr, "Co-op: command queue full\n");
}
void ReceiveModel(const BbGpuRecord* record) {
    if(record->bytes!=sizeof(ModelRecord)) return;
    const auto& part=*reinterpret_cast<const ModelRecord*>(record);
    if(part.count==0 || part.count>sizeof(part.pixels) || part.offset>BB_COOP_MODEL_BYTES ||
       part.count>BB_COOP_MODEL_BYTES-part.offset) return;
    std::scoped_lock lock{model_mutex};
    if(part.offset==0) {model_sequence=part.sequence;model_used=0;assembling.resize(BB_COOP_MODEL_BYTES);}
    if(part.sequence!=model_sequence || part.offset!=model_used) return;
    std::memcpy(assembling.data()+part.offset,part.pixels,part.count);model_used+=part.count;
    if(model_used==BB_COOP_MODEL_BYTES) {completed.swap(assembling);model_used=0;}
}
void PublishModel(const uint8_t* rgba,uint32_t bytes) {
    if(!rgba || bytes!=BB_COOP_MODEL_BYTES) return;
    static std::atomic<uint64_t> next_sequence{};
    auto record=std::make_unique<ModelRecord>();
    record->r={BBGPU_EV_COOP_MODEL,sizeof(ModelRecord)};
    record->sequence=++next_sequence;
    for(uint32_t offset=0;offset<bytes;offset+=record->count) {
        record->offset=offset;record->count=std::min<uint32_t>(sizeof(record->pixels),bytes-offset);
        std::memcpy(record->pixels,rgba+offset,record->count);
        if(BbSplit::Host()) BbSplit::Post(record.get());else ReceiveModel(&record->r);
    }
}

}
extern "C" void bbgpu_coop_publish(const BbCoopUi* view) {
    if (!view || view->version != BB_COOP_UI_VERSION) return;
    // Guest pad threads may have small stacks; the portrait-bearing snapshot lives on the heap.
    auto record=std::make_unique<BbCoopOverlay::SnapshotRecord>();
    record->r={BBGPU_REC_COOP_UI,sizeof(*record)};record->view=*view;
    if (BbSplit::Remote()) BbSplit::Push(record.get());
    else BbCoopOverlay::Dispatch(&record->r);
}
extern "C" int bbgpu_coop_next_command(BbCoopCommand* command) {
    if (!command) return 0;
    std::scoped_lock lock{command_mutex};
    if (commands.empty()) return 0;
    *command=commands.front(); commands.pop_front(); return 1;
}

extern "C" int bbgpu_coop_take_portrait(uint8_t* rgba,uint32_t capacity) {
    if(!rgba || capacity<BB_COOP_MODEL_BYTES) return 0;
    std::scoped_lock lock{model_mutex};
    if(completed.empty()) return 0;
    std::memcpy(rgba,completed.data(),completed.size());completed.clear();return 1;
}
