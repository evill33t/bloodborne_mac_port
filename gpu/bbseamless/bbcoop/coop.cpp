// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the emulator's seamless co-op code (../emulator/src/core/bloodborne_*.cpp) in the
// native port, through the C interface in gpu/bbcoop.h, and the shadPS4 services it uses that
// this port has no emulator for (the eboot's place, param.sfo, shared trophies).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "bbcoop.h"
#include "bbcoop/bbcoop_internal.h"
#include "bbcoop_revision.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "coop/team_code.h"
#include "core/bloodborne_compass.h"
#include "core/bloodborne_crash_dump.h"
#include "core/bloodborne_map.h"
#include "core/bloodborne_party.h"
#include "core/bloodborne_pings.h"
#include "core/bloodborne_portrait.h"
#include "core/bloodborne_re.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_team.h"
#include "core/cpu_patches.h"
#include "core/file_format/psf.h"
#include "core/libraries/network/server_link.h"
#include "core/libraries/np/np_trophy.h"

extern "C" int32_t runtime_trophy_unlock_from_party(int32_t trophy_id);

namespace MemoryPatcher {
EXPORT uintptr_t g_eboot_address = 0;
uint64_t g_eboot_image_size = 0;
std::string g_game_serial;
} // namespace MemoryPatcher

namespace Libraries::Np::NpTrophy {
namespace {
std::atomic<TrophyUnlockObserver> unlock_observer{};
} // namespace

void SetTrophyUnlockObserver(TrophyUnlockObserver observer) {
    unlock_observer.store(observer);
}

int UnlockTrophyFromParty(OrbisNpTrophyId trophy_id) {
    return runtime_trophy_unlock_from_party(trophy_id);
}

void NotifyTrophyUnlocked(OrbisNpTrophyId trophy_id) {
    if (const auto observer = unlock_observer.load(); observer != nullptr) {
        observer(trophy_id);
    }
}
} // namespace Libraries::Np::NpTrophy

extern "C" void bbcoop_note_segment(uint64_t address, uint64_t size, unsigned flags) {
    Core::NoteImageSegment(static_cast<uintptr_t>(address), size, flags);
}

namespace {
std::string game_folder; // app0, as bbcoop_install got it
} // namespace

extern "C" int bbcoop_game_file(const char* relative, char* out, size_t size) {
    if (game_folder.empty()) {
        return 0;
    }
    const auto path = (std::filesystem::path{game_folder} / relative).string();
    if (path.size() + 1 > size) {
        return 0;
    }
    std::memcpy(out, path.c_str(), path.size() + 1);
    return 1;
}

extern "C" void bbcoop_install(unsigned char* image, uint64_t eboot_size, void* code,
                               uint64_t code_size, const char* app0) {
    game_folder = app0 != nullptr ? app0 : "";
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    const auto sfo = std::filesystem::path{app0 != nullptr ? app0 : "."} / "sce_sys" / "param.sfo";
    if (!param_sfo->Open(sfo)) {
        LOG_WARNING(Loader, "Co-op: no param.sfo at {}", sfo.string());
    }
    MemoryPatcher::g_game_serial = std::string{param_sfo->GetString("TITLE_ID").value_or("")};
    MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(image);
    MemoryPatcher::g_eboot_image_size = eboot_size;
    // The build, as the emulator logs it: the rig checks that every game runs the same one.
    LOG_INFO(Loader, "Run: Revision {}", BB_SCM_REV);
    // What a log needs to compare two players' games, in one line: the build, the launcher's frame
    // rate preset and extra patches (patches.py's names; the patches themselves are compiled
    // into patches.bin), and the map event files the game reads (the kit's co-op ones over the
    // game's). The build and the event files also go to the room's peers (PeerHello).
    const auto app0_folder = std::filesystem::path{game_folder.empty() ? "." : game_folder};
    const u64 event_files = Core::Bloodborne::EventFilesHash(app0_folder / "dvdroot_ps4" / "event");
    // Build check on join: the game files and patches.bin (run.py names it in BB_PATCHES_FILE)
    // go into the build fingerprint with the build and the settings.
    const char* patches_file = std::getenv("BB_PATCHES_FILE");
    Core::Bloodborne::SetBuildIdentity(
        BB_SCM_REV, event_files, Core::Bloodborne::FileHash(app0_folder / "eboot.bin"),
        patches_file != nullptr ? Core::Bloodborne::FileHash(patches_file) : 0);
    const auto environment = [](const char* name, const char* absent) {
        const char* value = std::getenv(name);
        return std::string{value != nullptr && *value != '\0' ? value : absent};
    };
    char started[32] = "?";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", utc);
    }
    LOG_INFO(
        Loader, "Co-op: started {} UTC, build {}, FPS preset {}, extra patches {}, event files {}",
        started, BB_SCM_REV, environment("BB_FPS_PRESET", "?"), environment("BB_PATCHES", "none"),
        event_files != 0 ? fmt::format("{:016x}", event_files) : std::string{"unread"});
    if (code != nullptr) {
        Core::RegisterPatchModule(image, eboot_size, code, code_size);
    }
#ifdef _WIN32
    // The loader's own last-chance handler stays: it writes the co-op crash report itself
    // (bbcoop_crash_report) before it ends the process.
    const auto loader_filter = SetUnhandledExceptionFilter(nullptr);
    Core::Bloodborne::InstallCrashCatcher(reinterpret_cast<u64>(image), eboot_size);
    SetUnhandledExceptionFilter(loader_filter);
#else
    Core::Bloodborne::InstallCrashCatcher(reinterpret_cast<u64>(image), eboot_size);
#endif
    Core::Bloodborne::InstallSeamlessCoopPatches();
    Core::Bloodborne::InstallReverseEngineeringTrace();
}

extern "C" void bbcoop_crash_report(void* exception_pointers) {
    Core::Bloodborne::WriteCrashReport(exception_pointers);
}

extern "C" void bbcoop_window_closing(void) {
    Core::Bloodborne::ReportFreezeOnClose();
}

extern "C" int bbcoop_autoplay_pad(uint32_t* buttons, uint8_t sticks[4]) {
    std::uint32_t held = 0;
    std::array<std::uint8_t, 4> moved{128, 128, 128, 128};
    if (!Core::Bloodborne::AutoplayPad(held, moved)) {
        return 0;
    }
    *buttons = held;
    std::memcpy(sticks, moved.data(), moved.size());
    return 1;
}

extern "C" void bbcoop_party_death_pad(uint32_t* buttons, uint8_t sticks[4], uint8_t* l2,
                                       uint8_t* r2) {
    std::array<std::uint8_t, 4> moved{};
    std::memcpy(moved.data(), sticks, moved.size());
    Core::Bloodborne::FilterPartyDeathPad(*buttons, moved, *l2, *r2);
    std::memcpy(sticks, moved.data(), moved.size());
}

static_assert(BBCOOP_VITAL_OFF == static_cast<int>(Core::Bloodborne::Vital::Off) &&
              BBCOOP_VITAL_UP == static_cast<int>(Core::Bloodborne::Vital::Up) &&
              BBCOOP_VITAL_DOWN == static_cast<int>(Core::Bloodborne::Vital::Down) &&
              BBCOOP_VITAL_DEAD == static_cast<int>(Core::Bloodborne::Vital::Dead) &&
              BBCOOP_VITAL_SPECTATE == static_cast<int>(Core::Bloodborne::Vital::Spectate));

extern "C" void bbcoop_set_free_camera(int allowed) {
    Core::Bloodborne::SetFreeCameraAllowed(allowed != 0);
}

extern "C" void bbcoop_free_camera_toggle(void) {
    Core::Bloodborne::ToggleFreeCamera();
}

extern "C" int bbcoop_free_camera_on(void) {
    return Core::Bloodborne::Coop::FreeCameraOn() ? 1 : 0;
}

extern "C" void bbcoop_free_camera_pad(uint32_t* buttons, uint8_t sticks[4], uint8_t* l2,
                                       uint8_t* r2) {
    std::array<std::uint8_t, 4> moved{};
    std::memcpy(moved.data(), sticks, moved.size());
    Core::Bloodborne::FreeCameraPad(*buttons, moved, *l2, *r2);
    std::memcpy(sticks, moved.data(), moved.size());
}

extern "C" int bbcoop_camera_mouse(float dx, float dy) {
    return Core::Bloodborne::CoopCameraMouse(dx, dy) ? 1 : 0;
}

extern "C" int bbcoop_party_panel(bbcoop_panel* panel) {
    *panel = bbcoop_panel{.revive_target = -1};
    Core::Bloodborne::PartyPanel party;
    if (!Core::Bloodborne::ReadPartyPanel(party)) {
        return 0;
    }
    const auto count = std::min<std::size_t>(party.members.size(), BBCOOP_PANEL_MEMBERS);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& member = party.members[index];
        auto& out = panel->members[index];
        // Phase 11: the character's name when the server knows it, else "" (the overlay says
        // Hunter); never the online id.
        std::snprintf(out.name, sizeof(out.name), "%s", member.shown.c_str());
        out.vital = static_cast<int>(member.vital);
        out.here = member.here ? 1 : 0;
        out.hp = member.hp;
        out.hp_max = member.max;
        out.landed = member.landed;
        out.needed = member.needed;
        out.vials = member.vials;
        out.bullets = member.bullets;
    }
    panel->member_count = static_cast<int>(count);
    panel->revive_target = party.revive_target < static_cast<s32>(count) ? party.revive_target : -1;
    panel->vital = static_cast<int>(party.vital);
    panel->landed = party.landed;
    panel->needed = party.needed;
    panel->give_up = party.give_up;
    panel->native_list = Core::Bloodborne::NativePartyListShown() ? 1 : 0;
    panel->native_prompt = Core::Bloodborne::NativePartyPromptShown() ? 1 : 0;
    panel->native_supplies = Core::Bloodborne::NativePartySuppliesShown() ? 1 : 0;
    if (party.watching) {
        panel->watching = 1;
        std::snprintf(panel->watching_name, sizeof(panel->watching_name), "%s",
                      party.watching->c_str());
    }
    return 1;
}

extern "C" int bbcoop_compass_view(bbcoop_compass* compass) {
    *compass = {};
    Core::Bloodborne::CompassView view;
    if (!Core::Bloodborne::ReadCompass(view)) {
        return 0;
    }
    compass->heading = view.heading;
    for (size_t i = 0; i < view.markers.size() && i < BBCOOP_COMPASS_MARKERS; ++i) {
        const auto& marker = view.markers[i];
        auto& out = compass->markers[compass->count++];
        std::snprintf(out.name, sizeof(out.name), "%s", view.names[i].c_str());
        out.member = marker.member;
        out.bearing = marker.bearing;
        out.distance = marker.distance;
        out.x = marker.place.x;
        out.pinned = marker.place.pinned ? 1 : 0;
        std::snprintf(out.board, sizeof(out.board), "%s", view.members[i].c_str());
        out.vital = view.vitals[i];
        out.revive = view.revives[i];
    }
    if (view.stain) {
        compass->has_stain = 1;
        compass->stain.bearing = view.stain->bearing;
        compass->stain.distance = view.stain->distance;
        compass->stain.x = view.stain->place.x;
        compass->stain.pinned = view.stain->place.pinned ? 1 : 0;
    }
    return compass->count > 0 || compass->has_stain ? 1 : 0;
}

extern "C" int bbcoop_map_view(bbcoop_map* map, int open) {
    using namespace Core::Bloodborne;
    *map = {};
    MapScreenView view;
    if (!ReadMapScreen(view, open != 0)) {
        return 0;
    }
    map->in_world = view.in_world ? 1 : 0;
    map->map = view.map;
    map->area_name_id = AreaNameId(view.map);
    if (view.me) {
        map->has_me = 1;
        map->x = (*view.me)[0];
        map->y = (*view.me)[1];
        map->z = (*view.me)[2];
    }
    if (view.heading) {
        map->has_heading = 1;
        map->heading = *view.heading;
    }
    const MapTravelBlock block = MapTravelCheck({.lit = true,
                                                 .in_world = view.in_world,
                                                 .down = view.down,
                                                 .boss_fight = view.boss_fight,
                                                 .travelling = view.travelling});
    map->block = static_cast<int>(block); // the enums' orders agree (static_asserts below)
    PartyPanel panel;
    ReadPartyPanel(panel);
    for (const PanelMember& member : panel.members) {
        if (map->member_count >= BBCOOP_MAP_MEMBERS) {
            break;
        }
        bbcoop_map_member& out = map->members[map->member_count++];
        std::snprintf(out.name, sizeof(out.name), "%s", member.shown.c_str()); // "": Hunter
        std::snprintf(out.board, sizeof(out.board), "%s", member.member.c_str());
        out.map = member.map;
        out.vital = static_cast<int>(member.vital);
        if (member.at) {
            out.has_at = 1;
            out.x = (*member.at)[0];
            out.y = (*member.at)[1];
            out.z = (*member.at)[2];
        }
    }
    // Kaniel 2026-10-20: this player's own bloodstain, on its own map (only its own: Kaniel after
    // v1.0.2).
    if (const Bloodstain& stain = panel.own_stain; stain.map != 0) {
        map->has_stain = 1;
        map->stain = {.map = stain.map,
                      .x = stain.at[0],
                      .y = stain.at[1],
                      .z = stain.at[2],
                      .echoes = stain.echoes};
    }
    for (const u32 area : view.cleared) {
        if (map->cleared_count < 24) {
            map->cleared[map->cleared_count++] = area;
        }
    }
    for (const MapLamp& lamp : MapLamps()) {
        if (map->lamp_count >= BBCOOP_MAP_LAMPS) {
            break;
        }
        bbcoop_map_lamp& out = map->lamps[map->lamp_count++];
        out.warp_point = lamp.warp_point;
        out.map = lamp.packed_map;
        out.x = lamp.x;
        out.y = lamp.y;
        out.z = lamp.z;
        out.lit = std::ranges::find(view.lit, lamp.warp_point) != view.lit.end() ? 1 : 0;
        out.name_id = LampNameId(lamp.warp_point);
    }
    return 1;
}
static_assert(BBCOOP_MAP_GO == int(Core::Bloodborne::MapTravelBlock::None) &&
              BBCOOP_MAP_NOT_LIT == int(Core::Bloodborne::MapTravelBlock::NotLit) &&
              BBCOOP_MAP_NOT_IN_WORLD == int(Core::Bloodborne::MapTravelBlock::NotInWorld) &&
              BBCOOP_MAP_DOWN == int(Core::Bloodborne::MapTravelBlock::Down) &&
              BBCOOP_MAP_BOSS == int(Core::Bloodborne::MapTravelBlock::BossFight) &&
              BBCOOP_MAP_TRAVELLING == int(Core::Bloodborne::MapTravelBlock::Travelling));

extern "C" int bbcoop_pings_view(bbcoop_pings* pings) {
    using namespace Core::Bloodborne;
    *pings = {};
    PingsView view;
    if (!ReadPings(view)) {
        return 0;
    }
    pings->has_heading = view.heading ? 1 : 0;
    pings->heading = view.heading.value_or(0.0f);
    for (const PingShown& ping : view.pings) {
        if (pings->count >= BBCOOP_PINGS) {
            break;
        }
        bbcoop_ping& out = pings->pings[pings->count++];
        std::snprintf(out.name, sizeof(out.name), "%s", ping.name.c_str());
        out.own = ping.member.empty() ? 1 : 0;
        out.map = ping.map;
        out.x = ping.at[0];
        out.y = ping.at[1];
        out.z = ping.at[2];
        out.enemy = ping.enemy != 0 ? 1 : 0;
        out.from_map = ping.from_map ? 1 : 0;
        out.here = ping.here ? 1 : 0;
        out.distance = ping.distance;
        out.bearing = ping.bearing;
        const CompassPlace place = PlaceOnStrip(ping.bearing);
        out.strip_x = place.x;
        out.strip_pinned = place.pinned ? 1 : 0;
        if (ping.screen) {
            out.on_screen = 1;
            out.screen_x = ping.screen->x;
            out.screen_y = ping.screen->y;
            out.pinned = ping.screen->pinned ? 1 : 0;
            out.arrow = ping.screen->arrow;
        }
        out.seconds_left = ping.seconds_left;
    }
    return pings->count > 0 ? 1 : 0;
}

extern "C" void bbcoop_ping_aim(void) {
    Core::Bloodborne::RequestAimPing();
}

extern "C" int bbcoop_ping_map(const bbcoop_ping_area* areas, int count, uint32_t shown,
                               float atlas_x, float atlas_z, float y) {
    using namespace Core::Bloodborne;
    std::vector<PingArea> list;
    for (int i = 0; i < count; ++i) {
        list.push_back({areas[i].map, areas[i].x0, areas[i].x1, areas[i].z0, areas[i].z1,
                        areas[i].dx, areas[i].dz});
    }
    const auto spot = PingAreaAt(list, shown, atlas_x, atlas_z);
    if (!spot) {
        return 0;
    }
    RequestMapPing(spot->map, {spot->x, y, spot->z});
    return 1;
}

extern "C" void bbcoop_set_team_pings(int on) {
    Core::Bloodborne::SetTeamPingsOn(on != 0);
}

extern "C" void bbcoop_ping_label(const char* name, int has_distance, float distance,
                                  const char* area, char* out, size_t size) {
    const std::string line = Core::Bloodborne::PingLabel(
        name != nullptr ? name : "", has_distance ? std::optional{distance} : std::nullopt,
        area != nullptr ? area : "");
    std::snprintf(out, size, "%s", line.c_str());
}

extern "C" void bbcoop_ping_label_place(float x, float y, float r, float width, float height,
                                        const float* taken, int count, float gap, float out[4]) {
    std::vector<Core::Bloodborne::PingBox> boxes;
    for (int i = 0; i < count; ++i) {
        boxes.push_back({taken[i * 4], taken[i * 4 + 1], taken[i * 4 + 2], taken[i * 4 + 3]});
    }
    const auto box = Core::Bloodborne::PlacePingLabel(x, y, r, width, height, boxes, gap);
    out[0] = box.x0;
    out[1] = box.y0;
    out[2] = box.x1;
    out[3] = box.y1;
}

extern "C" void bbcoop_map_travel(int32_t warp_point) {
    Core::Bloodborne::RequestMapTrip(warp_point);
}

extern "C" int bbcoop_map_explored(uint32_t map, int16_t* xz, int max) {
    static_assert(BBCOOP_MAP_CELL == int(Core::Bloodborne::ExploredCellMetres));
    const auto cells = Core::Bloodborne::ExploredOn(map);
    int count = 0;
    for (const auto& cell : cells) {
        if (count >= max) {
            break;
        }
        xz[count * 2] = cell[0];
        xz[count * 2 + 1] = cell[1];
        ++count;
    }
    return count;
}

extern "C" int32_t bbcoop_map_area_name_id(uint32_t map) {
    return Core::Bloodborne::AreaNameId(map);
}

static_assert(BBCOOP_PORTRAIT_SIDE == Core::Bloodborne::PortraitSide);

extern "C" int bbcoop_member_portrait(const char* member, uint8_t* rgba, size_t size, uint32_t* id) {
    uint32_t seen = 0;
    const bool shown = member != nullptr &&
                       Core::Bloodborne::ReadMemberPortrait(member, std::span<uint8_t>{rgba, size}, seen);
    if (id != nullptr) {
        *id = seen;
    }
    return shown ? 1 : 0;
}

extern "C" float bbcoop_revive_share(uint32_t landed, uint32_t needed) {
    return Core::Bloodborne::ReviveShare(landed, needed);
}

extern "C" void bbcoop_portrait_model(const uint8_t* rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return;
    }
    Core::Bloodborne::NoteStatusModel(
        std::span<const uint8_t>{rgba, std::size_t(width) * std::size_t(height) * 4}, width, height);
}

extern "C" int bbcoop_portrait_offscreen_wanted(void) {
    return Core::Bloodborne::OffscreenPortraitWanted() ? 1 : 0;
}

extern "C" uint32_t bbcoop_portrait_version(int slot) {
    return Core::Bloodborne::PortraitSlotVersion(slot);
}

extern "C" int bbcoop_portrait_slot(int slot, uint8_t* rgba, size_t size, uint32_t* version) {
    uint32_t seen = 0;
    const bool shown = Core::Bloodborne::ReadPortraitSlot(slot, std::span<uint8_t>{rgba, size}, seen);
    if (version != nullptr) {
        *version = seen;
    }
    return shown ? 1 : 0;
}

namespace {

void CopyName(char* out, size_t size, const std::string& text) {
    const size_t n = std::min(size - 1, text.size());
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

int LobbyStateOf(bbcoop::LobbyPlayer::State state) {
    return state == bbcoop::LobbyPlayer::PLAYING ? BBCOOP_LOBBY_PLAYING
           : state == bbcoop::LobbyPlayer::TITLE ? BBCOOP_LOBBY_TITLE
                                                 : BBCOOP_LOBBY_OFFLINE;
}

} // namespace

extern "C" int bbcoop_lobby_view(bbcoop_lobby* lobby) {
    namespace Coop = Core::Bloodborne::Coop;
    *lobby = {};
    if (!Coop::TeamModeOn()) {
        return 0;
    }
    // Refused (another release): the screen says why instead of "reaching", also over a lobby the
    // game's sign-in got before its build was known (the build check on join refuses the hello
    // that names it; nothing of that lobby works then).
    const std::string refusal = Libraries::Net::ServerLinkRefusal();
    const auto state = Coop::TeamLobbyState();
    if (!state || !refusal.empty()) {
        static_assert(sizeof(lobby->notice) > BloodborneCoop::RefusalNoticeMax);
        CopyName(lobby->notice, sizeof(lobby->notice),
                 Core::Bloodborne::LinkRefusalNotice(refusal));
        return 1;
    }
    const std::string me = Coop::TeamLobbyMember();
    CopyName(lobby->me, sizeof(lobby->me), me);
    lobby->signed_in = 1;
    lobby->menu = Coop::TeamMenuOpen() ? 1 : 0;
    bool in_hunt = false;
    lobby->playing = Coop::TeamPlaying(&in_hunt) ? 1 : 0;
    lobby->in_hunt = in_hunt ? 1 : 0;
    // Another installation than the others' links; the screen says so (Kaniel 2026-10-25).
    CopyName(lobby->notice, sizeof(lobby->notice),
             Core::Bloodborne::TeamScreenNotice(state->notice(),
                                                Libraries::Net::ServerLinkWarning()));
    {
        // Kaniel 2026-10-25: the host rules the hunt plays by.
        const Core::Bloodborne::HostRules rules = Coop::ActiveHostRules();
        lobby->rule_rot = rules.rot ? 1 : 0;
        lobby->rule_travel = rules.travel_together ? 1 : 0;
        lobby->rule_vote = rules.travel_vote ? 1 : 0;
        lobby->rule_scale = rules.enemy_scaling ? 1 : 0;
    }
    if (state->has_team()) {
        const bbcoop::Team& team = state->team();
        lobby->in_team = 1;
        lobby->leader = team.leader() == me;
        lobby->started = team.started();
        // The code its leader hands out (the server sends it to the leader only).
        CopyName(lobby->code, sizeof(lobby->code), BloodborneCoop::FormatTeamCode(team.code()));
        lobby->allow_invaders = team.allow_invaders() ? 1 : 0;
        lobby->death_mode = static_cast<int>(Core::Bloodborne::TeamDeathMode(*state));
        // The leader first.
        std::vector<const bbcoop::TeamMember*> members;
        for (const bbcoop::TeamMember& m : team.members()) {
            if (m.member() == team.leader()) {
                members.insert(members.begin(), &m);
            } else {
                members.push_back(&m);
            }
        }
        for (const bbcoop::TeamMember* m : members) {
            if (lobby->member_count == BBCOOP_LOBBY_TEAM) {
                break;
            }
            bbcoop_lobby_entry& out = lobby->members[lobby->member_count++];
            CopyName(out.name, sizeof(out.name), m->name());
            CopyName(out.id, sizeof(out.id), m->member());
            out.state = LobbyStateOf(m->state());
            out.ready = m->ready();
        }
    }
    // Kaniel 2026-10-17: the server's worlds (any team's to pick, the leader's or, before a team, the
    // player's own pick), as the game's Load Game lists its saves.
    const std::string pick = state->has_team() ? state->team().world() : state->world();
    const std::string own_team = state->has_team() ? state->team().id() : std::string{};
    lobby->world = -1;
    const auto local_time = [](int64_t ms, char* out, size_t size, const char* format) {
        if (ms <= 0) {
            return;
        }
        const std::time_t at = static_cast<std::time_t>(ms / 1000);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &at);
#else
        localtime_r(&at, &local);
#endif
        std::strftime(out, size, format, &local);
    };
    for (const bbcoop::TeamWorldInfo& w : state->worlds()) {
        if (lobby->world_count == BBCOOP_LOBBY_WORLDS) {
            break;
        }
        if (w.id() == pick) {
            lobby->world = lobby->world_count;
        }
        bbcoop_lobby_world& out = lobby->worlds[lobby->world_count++];
        CopyName(out.id, sizeof(out.id), w.id());
        CopyName(out.name, sizeof(out.name), Core::Bloodborne::TeamWorldName(w));
        out.lamps = static_cast<int>(w.lamps());
        local_time(w.played_ms(), out.when, sizeof(out.when), "%d %b %H:%M");
        local_time(w.created_ms(), out.created, sizeof(out.created), "%d %b %Y");
        std::string hunters;
        for (const std::string& name : w.characters()) {
            hunters += (hunters.empty() ? "" : ", ") + name;
        }
        CopyName(out.characters, sizeof(out.characters), hunters);
        out.busy = !w.played_by().empty() && w.played_by() != own_team;
    }
    for (const bbcoop::LobbyPlayer& player : state->players()) {
        const bool mine = state->has_team() && player.team() == state->team().id();
        if (mine || lobby->player_count == BBCOOP_LOBBY_PLAYERS) {
            continue;
        }
        bbcoop_lobby_entry& out = lobby->players[lobby->player_count++];
        CopyName(out.name, sizeof(out.name), player.name());
        CopyName(out.id, sizeof(out.id), player.member());
        out.state = LobbyStateOf(player.state());
    }
    for (const bbcoop::TeamInvite& invite : state->invites()) {
        if (lobby->invite_count == BBCOOP_LOBBY_INVITES) {
            break;
        }
        bbcoop_lobby_entry& out = lobby->invites[lobby->invite_count++];
        CopyName(out.name, sizeof(out.name), invite.from_name());
        CopyName(out.id, sizeof(out.id), invite.team());
    }
    return 1;
}

static_assert(BBCOOP_TEAM_GO_START == int(Core::Bloodborne::TeamGate::Choice::Start) &&
              BBCOOP_TEAM_GO_ALONE == int(Core::Bloodborne::TeamGate::Choice::Alone));

extern "C" int bbcoop_team_screen(void) {
    return Core::Bloodborne::Coop::TeamScreenUp() ? 1 : 0;
}

extern "C" int bbcoop_team_load_view(bbcoop_team_load* view) {
    const Core::Bloodborne::TeamLoad::View load = Core::Bloodborne::Coop::TeamLoadView();
    if (!load.up || view == nullptr) {
        return 0;
    }
    *view = {};
    for (const auto& row : load.rows) {
        if (view->count >= 4) {
            break;
        }
        bbcoop_team_load_row& out = view->rows[view->count++];
        std::snprintf(out.name, sizeof(out.name), "%s", row.name.c_str());
        out.stage = static_cast<int>(row.stage);
        out.percent = static_cast<int>(row.percent);
        out.self = row.self ? 1 : 0;
    }
    return 1;
}

extern "C" int bbcoop_sound_held(void) {
    return Core::Bloodborne::Coop::TeamLoadCovers() ? 1 : 0;
}

extern "C" int bbcoop_cutscene_skip(void) {
    return Core::Bloodborne::Coop::CutsceneSkipPressed() ? 1 : 0;
}

extern "C" int bbcoop_cutscene_vote_pad(int options) {
    using Pad = Core::Bloodborne::CutsceneVote::Pad;
    switch (Core::Bloodborne::Coop::CutsceneVotePad(options != 0)) {
    case Pad::Hide:
        return 1;
    case Pad::Press:
        return 2;
    case Pad::Leave:
        break;
    }
    return 0;
}

extern "C" int bbcoop_cutscene_vote(int* want, int* watching) {
    std::uint32_t voted = 0;
    std::uint32_t all = 0;
    const bool shown = Core::Bloodborne::Coop::CutsceneVoteView(voted, all);
    *want = static_cast<int>(voted);
    *watching = static_cast<int>(all);
    return shown ? 1 : 0;
}

extern "C" void bbcoop_team_menu(int open) {
    Core::Bloodborne::Coop::TeamMenu(open != 0, open == 1);
}

extern "C" int bbcoop_team_world_list_asked(void) {
    return Core::Bloodborne::Coop::TeamWorldListAsked() ? 1 : 0;
}

namespace {
std::atomic<int> map_menu{}; // how the map screen was opened (bbcoop_map_menu), 0 shut
std::mutex map_back_mutex;
Core::Bloodborne::TeamMenuBack map_back;
} // namespace

extern "C" void bbcoop_map_menu(int open) {
    const int was = map_menu.exchange(open);
    if (open == 0 && was != 0) {
        std::scoped_lock lock{map_back_mutex};
        map_back.Closed(Core::Bloodborne::Coop::SteadyMilliseconds(), was == 1);
    }
}

extern "C" void bbcoop_set_hud_shift(float dx, float dy) {
    Core::Bloodborne::SetHudEdgeShift(dx, dy);
}

extern "C" void bbcoop_map_from_pad(void) {
    Core::Bloodborne::MapScreenView view;
    if (map_menu.load() == 0 && Core::Bloodborne::ReadMapScreen(view, false) && view.in_world) {
        bbcoop_map_menu(2);
    }
}

extern "C" int bbcoop_map_screen(void) {
    return map_menu.load() != 0 ? 1 : 0;
}

extern "C" int bbcoop_team_menu_back(void) {
    bool map_page = false;
    {
        std::scoped_lock lock{map_back_mutex};
        map_page = map_back.Pressed(Core::Bloodborne::Coop::SteadyMilliseconds());
    }
    return Core::Bloodborne::Coop::TeamMenuBackPressed() || map_page ? 1 : 0;
}

extern "C" void bbcoop_team_join_hunt(const char* team) {
    Core::Bloodborne::Coop::TeamJoinHunt(team ? team : "");
}

extern "C" void bbcoop_invade(int on) {
    Core::Bloodborne::Coop::InvasionRequest(on == 0);
}

extern "C" void bbcoop_team_allow_invaders(int allow) {
    Core::Bloodborne::Coop::TeamAllowInvaders(allow != 0);
}

static_assert(BBCOOP_INVASION_INVADER_DIED == bbcoop::InvasionEnd::INVADER_DIED &&
              BBCOOP_INVASION_TARGET_KILLED == bbcoop::InvasionEnd::TARGET_KILLED &&
              BBCOOP_INVASION_PARTY_WIPED == bbcoop::InvasionEnd::PARTY_WIPED &&
              BBCOOP_INVASION_NOT_FOUND == bbcoop::InvasionEnd::NOT_FOUND &&
              BBCOOP_INVASION_REFUSED == bbcoop::InvasionEnd_Reason_REFUSED);

extern "C" int bbcoop_invasion_view(bbcoop_invasion* view) {
    namespace Coop = Core::Bloodborne::Coop;
    using Core::Bloodborne::InvasionView;
    *view = {};
    if (!Coop::TeamModeOn()) {
        return 0;
    }
    const InvasionView now = Coop::InvasionViewNow();
    if (now.role == InvasionView::Role::None || now.phase == InvasionView::Phase::None) {
        return 0;
    }
    view->role = now.role == InvasionView::Role::Target ? BBCOOP_INVASION_TARGET
                                                        : BBCOOP_INVASION_INVADER;
    static_assert(static_cast<int>(InvasionView::Phase::Searching) == BBCOOP_INVASION_SEARCHING &&
                  static_cast<int>(InvasionView::Phase::Ended) == BBCOOP_INVASION_ENDED);
    view->phase = static_cast<int>(now.phase); // the same order
    view->seconds = now.seconds;
    view->reason = static_cast<int>(now.reason);
    // The hunted host is a board name: its character's name where the lobby knows it.
    const std::string shown = Coop::CharacterNameOf(now.name);
    CopyName(view->name, sizeof(view->name), shown.empty() ? now.name : shown);
    return 1;
}

extern "C" void bbcoop_team_choose(int what) {
    Core::Bloodborne::Coop::TeamChoose(static_cast<Core::Bloodborne::TeamGate::Choice>(what));
}

static_assert(BBCOOP_TEAM_WORLD == bbcoop::TeamRequest::WORLD &&
              BBCOOP_TEAM_FORGET_WORLD == bbcoop::TeamRequest::FORGET_WORLD &&
              BBCOOP_TEAM_JOIN_CODE == bbcoop::TeamRequest::JOIN_CODE &&
              BBCOOP_TEAM_NEW_CODE == bbcoop::TeamRequest::NEW_CODE &&
              BBCOOP_TEAM_INVITE_NAME == bbcoop::TeamRequest::INVITE_NAME &&
              BBCOOP_TEAM_DEATH_MODE == bbcoop::TeamRequest::DEATH_MODE &&
              BBCOOP_DEATH_SPECTATE == int(Core::Bloodborne::DeathMode::Spectate));

extern "C" void bbcoop_lobby_request(int action, const char* id) {
    Core::Bloodborne::Coop::TeamLobbyRequest(static_cast<bbcoop::TeamRequest::Action>(action),
                                             id ? id : "");
}

extern "C" void bbcoop_file_opened(const char* guest_path) {
    Core::Bloodborne::NoteSaveFileOpened(guest_path);
}

extern "C" void bbcoop_trophy_unlocked(int32_t trophy_id) {
    Libraries::Np::NpTrophy::NotifyTrophyUnlocked(trophy_id);
}

extern "C" int bbcoop_rot_view(bbcoop_rot* rot) {
    *rot = {};
    const Core::Bloodborne::Coop::RotView view = Core::Bloodborne::Coop::ReadRotView();
    if (!view.on) {
        return 0;
    }
    for (const u32 kind : view.kinds) {
        if (rot->count < BBCOOP_ROT_MAX) {
            rot->kinds[rot->count++] = static_cast<int>(kind);
        }
    }
    return rot->count > 0 ? 1 : 0;
}

extern "C" int bbcoop_team_travel_view(bbcoop_team_travel* travel) {
    using namespace Core::Bloodborne;
    *travel = {};
    const auto view = Coop::ReadTeamTravel();
    if (!view) {
        return 0;
    }
    travel->question = view->question == TeamTravelView::Question::WithTheTeam ? 1
                       : view->question == TeamTravelView::Question::Vote      ? 2
                                                                               : 0;
    travel->point = view->point;
    travel->lamp_name_id = view->point == HuntersDreamWarpPoint ? -1 : LampNameId(view->point);
    const std::string& from = view->question == TeamTravelView::Question::Vote ? view->from
                                                                               : view->notice_from;
    std::string name = Coop::CharacterNameOf(from);
    CopyName(travel->from, sizeof(travel->from), name.empty() ? from : name);
    travel->seconds_left = view->seconds_left;
    travel->waiting = view->waiting ? 1 : 0;
    travel->notice = view->notice == TeamTravelNotice::SaidNo    ? 1
                     : view->notice == TeamTravelNotice::Stopped ? 2
                                                                 : 0;
    return 1;
}

extern "C" void bbcoop_team_travel_answer(int yes) {
    Core::Bloodborne::Coop::AnswerTeamTravel(yes != 0);
}
