// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::Bloodborne {

struct PartyPanel;
struct CompassView;
struct PingsView;

std::optional<std::string> GetSeamlessHostPlacementHeader();
bool SetSeamlessHostPlacementHeader(std::string_view value);
void ClearSeamlessHostPlacementHeader();
// The server handed this game a summon (its claim on our sign). Called by the HTTP worker.
void NoteSummonDelivered();
// The game posted a summon search (/summon_messenger/get). Called by the HTTP layer as it sends.
void NoteSummonSearchPosted();
// Logs the eboot addresses on the current (guest) stack: who in the game called an HLE function.
void LogGameCallChain(const char* what);
void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id);
void InstallSeamlessCoopPatches();
void InstallReverseEngineeringTrace();
// This build's revision and the hashes of its event files (EventFilesHash), eboot and patches.bin
// (FileHash; 0 unknown): with the environment's settings its build fingerprint
// (CoopBuildFingerprint), for the server link's hello (build check on join) and the hello the
// game sends its room peers (PeerHelloType). Before it is called no hello goes out.
void SetBuildIdentity(std::string_view revision, std::uint64_t event_files, std::uint64_t eboot = 0,
                      std::uint64_t patches_bin = 0);
// Tells the party code which character save slot the game opened last.
void NoteSaveFileOpened(std::string_view guest_path);
// Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY): the buttons and sticks (lx, ly, rx, ry) it holds now;
// false when it is off. Called by the pad reader.
bool AutoplayPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks);
// Party death (SHADPS4_BLOODBORNE_PARTY_DEATH): sees the pad (holding Circle to give up, attacks
// that revive a downed member) and, while the player is down, lets go of all but Options and the
// camera stick. Called by the pad reader on what the game will read (autoplay's buttons in).
void FilterPartyDeathPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks,
                         std::uint8_t& l2, std::uint8_t& r2);
// Kaniel 2026-10-25: the co-op cameras (spectating teammates, the free camera). The mouse's motion
// (counts) while one drives the view: true when it took it (the game's mouse camera gets none).
// The free camera: allowed by the port's setting (bbport.ini free_camera), toggled by its hotkey
// (F7, or Square with L3 held: FreeCameraPad sees that one), and while it flies the pad is its
// own: true, and the game gets nothing.
bool CoopCameraMouse(float dx, float dy);
void SetFreeCameraAllowed(bool allowed);
void ToggleFreeCamera();
bool FreeCameraPad(std::uint32_t& buttons, std::array<std::uint8_t, 4>& sticks, std::uint8_t& l2,
                   std::uint8_t& r2);
// Party death's panel (bloodborne_party.h) as of the last party tick, for the port's overlay:
// false when there is nothing to draw (no party session, nobody else on the board, not down).
bool ReadPartyPanel(PartyPanel& panel);
// The party HUD's own party list drew members within the last half second (the overlay leaves its
// list out then).
bool NativePartyListShown();
// Kaniel 2026-10-18: the party list shows its members' vials and bullets in the HUD's own fields
// (CoopSupply0-2) within the last half second (the overlay then draws none).
bool NativePartySuppliesShown();
// The party HUD's prompt (CoopPrompt) showed party death's prompt within the last half second (the
// overlay then draws only its button glyph).
bool NativePartyPromptShown();
// The game's HUD ticked within the last half second (the port's party HUD): not in a load or a
// cutscene. Always true without the party HUD.
bool GameHudUp();
// Kaniel 2026-10-17: the teammate compass as of the last party tick (bloodborne_compass.h); false
// with `view.why` when it shows nothing.
bool ReadCompass(CompassView& view);
// Kaniel 2026-10-17: the port's map screen (bloodborne_map.h), as the game thread read it last
// while the screen was open: where this player stands, the lamps lit (their warp points, the
// Hunter's Dream always) and what would stop a trip now.
struct MapScreenView {
    bool in_world = false; // the game's HUD is up: not a load, a cutscene or the title
    u32 map = 0;
    std::optional<std::array<float, 3>> me;
    std::optional<float> heading; // the camera's (bloodborne_compass.h)
    std::vector<s32> lit;
    bool down = false;
    bool boss_fight = false;
    bool travelling = false;
    std::vector<u32> cleared; // areas whose main bosses are down (AreaBossFlags)
};
// `open`: the screen is up (the game thread reads for it only while it is). False before its
// first reading.
bool ReadMapScreen(MapScreenView& view, bool open);
// A trip from the map to the lamp, started by the game thread when MapTravelCheck allows it.
void RequestMapTrip(s32 warp_point);
// Kaniel 2026-10-18 (fog of war): the cells (bloodborne_map.h's ExploredCellMetres squares) this
// character has walked on the map.
std::vector<std::array<s16, 2>> ExploredOn(u32 packed_map);
// Kaniel 2026-10-24: team pings (bloodborne_pings.h). A ping where the camera aims (on the
// locked-on enemy while locked on) or on the map (`map`'s metres), placed by the game thread on
// its next tick, at most one a second; the pings shown as of the last tick (false: none);
// whether pings are on (the Insert menu's Team pings: off, none are placed, shown or heard).
void RequestAimPing();
void RequestMapPing(u32 map, const std::array<float, 3>& at);
bool ReadPings(PingsView& view);
void SetTeamPingsOn(bool on);
// Party portraits (bloodborne_portrait.h). The status page's model (its render target's RGBA
// rows) as the port's texture cache read it back: this character's portrait once the page closes.
void NoteStatusModel(std::span<const std::uint8_t> rgba, int width, int height);
// The off-screen body renderer has drawn this character (TickPortraits): the port reads its
// 560x768 target back when it is drawn, not only when the status page samples it.
bool OffscreenPortraitWanted();
// The party list slot's picture (PortraitBytes into `rgba`; false when it shows none) and its
// version, which changes whenever the picture does.
bool ReadPortraitSlot(int slot, std::span<std::uint8_t> rgba, std::uint32_t& version);
std::uint32_t PortraitSlotVersion(int slot);
// Kaniel 2026-10-17 (the compass): a member's portrait by its board name (round, as the slots'):
// `id` changes with the picture, 0 and false when it has none.
bool ReadMemberPortrait(std::string_view member, std::span<std::uint8_t> rgba, std::uint32_t& id);

} // namespace Core::Bloodborne
