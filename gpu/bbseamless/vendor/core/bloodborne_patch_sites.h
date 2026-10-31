// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>

#include "common/types.h"

namespace Core::Bloodborne {

// Every place in the 01.09 eboot that seamless co-op changes or relies on, in one table. The game
// patches the launcher applies first (bbport/scripts/patches.py: the FPS presets, the effect
// switches, third-party XML) can write over the same bytes: "Uncap FPS++" rewrote the sign
// timers, and our patch was skipped for two days without anyone noticing (slow joins). The
// launcher warns about such an overlap and the eboot test applies the default patch set and
// checks every required site. bbport/patches/coop-sites.json is this table for the launcher and
// tools/re/check_party_log.py, written by the unit test CoopSites.MatchTheCommittedJson
// (BB_WRITE_COOP_SITES=1 rewrites it).

// A co-op feature: what InstallSeamlessCoopPatches reports as on or skipped. A required one
// missing breaks a party (a guest sent home, slow joins); an optional one costs a nicety or a
// diagnostic.
struct CoopFeature {
    std::string_view name;
    bool required;
};

enum class CoopSiteKind : u8 {
    Patch, // bytes written over the game's (`patched`)
    Hook,  // a jump to a guest code hook over whole instructions
    Call,  // a function of the game we call, checked by its first bytes
};

// Bytes are hex pairs separated by spaces. `accepted` lists other bytes the site may hold
// instead of `original` (other game patches' versions we patch all the same), separated by '|',
// with "??" for any byte.
struct CoopSite {
    std::string_view name;
    std::string_view feature;
    CoopSiteKind kind;
    u64 offset;
    std::string_view original;
    std::string_view patched = {};
    std::string_view accepted = {};
};

inline constexpr auto CoopFeatures = std::to_array<CoopFeature>({
    {"seamless core", true},
    {"world teardown", false},
    {"ghost death", true},
    {"guest bloodstain", false},
    {"no echo theft", false},
    {"host death", true},
    {"boss clear", true},
    {"join anywhere", true},
    {"quiet summon banners", false},
    {"movie player delete delay", false},
    {"send pace", false},
    {"boss death tracer", false},
    {"bullet tracer", false},
    {"native packet trace", false},
    {"channel table", false},
    {"game packet link", false},
    {"enemy owner", false},
    {"team lobby", false},
    {"team join", false},
    {"cutscene skip", false},
    {"no multiplayer banners", false},
    {"silent key items", false},
    {"character names", false},
    {"no lost-connection popup", false},
    {"own player numbers", false},
    {"host leaves", true},
    {"seamless travel", true},
    {"stop request", true},
    {"member leave log", false},
    {"lamps", false},
    {"talk diagnostics", false},
    {"talk phantom", false},
    {"talk host", false},
    {"level sync", false},
    {"solid NPCs", false},
    {"guests hit NPCs", false},
    {"full echoes", false},
    {"echo diagnostics", false},
    {"trading", false},
    {"menu client", false},
    {"map item sync", false},
    {"shared loot", false},
    {"map item pickup", false},
    {"item diagnostics", false},
    {"item button diagnostics", false},
    {"lamp registration", false},
    {"boss items", false},
    {"keep local flags", false},
    {"party death", false},
    {"own hits", false},
    {"effect hits", false},
    {"real revive hits", false},
    {"no hit reaction while down", false},
    {"no damage number on downed bodies", false},
    {"no area events while down", false},
    {"party nameplates", false},
    {"party list in the HUD", false},
    {"sign timers", true},
    {"summon slot hold", false},
    {"server owns leaves", true},
    {"floor guard", false},
    {"flag block guard", false},
    {"chalice altar pick", false},
    {"packet guard", false},
    {"co-op camera", false},
    {"boss fight deaths", false},
    {"boss tempo", false},
});

inline constexpr auto CoopSites = std::to_array<CoopSite>({
    // Seamless core (InstallSeamlessCoopPatches): all of it or none.
    // Beckoning Bell: the area comparison becomes signed (`ja` -> `jg`) and the area flag result
    // reads as allowed.
    {"Beckoning area comparison", "seamless core", CoopSiteKind::Patch, 0x0157F8C8,
     "0F 87 90 00 00 00", "0F 8F 90 00 00 00"},
    {"Beckoning area flag", "seamless core", CoopSiteKind::Patch, 0x0157F960, "34 01", "31 C0"},
    // Small Resonant Bell (the responder): area and common results allowed.
    {"responder Bell area", "seamless core", CoopSiteKind::Patch, 0x0157F6D1, "88 C3", "B1 01"},
    {"responder Bell common", "seamless core", CoopSiteKind::Patch, 0x0157F6D3,
     "4C 89 F7 E8 F5 9F 34 00", "E9 E9 09 00 00 90 90 90"},
    // A Bell already ringing: signed area comparison, area flag allowed.
    {"active Bell area comparison", "seamless core", CoopSiteKind::Patch, 0x015068BB, "77 4A",
     "7F 4A"},
    {"active Bell area flag", "seamless core", CoopSiteKind::Patch, 0x015068FC, "88 C3 80 F3 01",
     "B3 01 90 90 90"},
    // The responder's search: signed area range.
    {"responder search area range", "seamless core", CoopSiteKind::Patch, 0x0191A8C3,
     "18 C9 20 C1 EB 02", "7D 04 88 C1 EB 02"},
    // SOS status: no area restriction (r13d = 0).
    {"SOS status area", "seamless core", CoopSiteKind::Patch, 0x018700D3,
     "84 C0 41 BD FF FF FF FF 44 0F 44 EB 41 C1 ED 1F",
     "45 31 ED 90 90 90 90 90 90 90 90 90 90 90 90 90"},
    // Summon candidates and the summon builder: across maps, world states and negative events.
    {"summon candidate area", "seamless core", CoopSiteKind::Patch, 0x014B714A, "0F 85 C0 03 00 00",
     "90 90 90 90 90 90"},
    {"summon builder area", "seamless core", CoopSiteKind::Patch, 0x01874B79, "74 0D", "EB 0D"},
    {"summon builder world state", "seamless core", CoopSiteKind::Patch, 0x018749E8,
     "0F 85 48 01 00 00", "90 90 90 90 90 90"},
    {"summon builder negative event", "seamless core", CoopSiteKind::Patch, 0x018749F0,
     "0F 88 40 01 00 00", "90 90 90 90 90 90"},
    // The world teardown's stop-matching call keeps matching instead.
    {"stage transition keeps matching", "seamless core", CoopSiteKind::Patch, 0x019471B1,
     "E8 EA 95 58 00", "E8 EA 8E 58 00"},
    {"SummonBuild.Entry", "seamless core", CoopSiteKind::Hook, 0x01874710, "55 48 89 E5 41 57"},
    {"CSMultiPlayerIns.CrossMapGuestHandoff", "seamless core", CoopSiteKind::Hook, 0x01E4FF3A,
     "48 8D 83 ED 00 00 00"},
    {"Warp.StageDescriptor.Finalize", "seamless core", CoopSiteKind::Hook, 0x01944F23,
     "B8 00 00 00 FF"},
    {"SosStatus.Update", "seamless core", CoopSiteKind::Hook, 0x01872360, "55 48 89 E5 41 57"},
    {"HealingFountain.Availability.Host", "seamless core", CoopSiteKind::Hook, 0x012F836E,
     "41 80 7D 48 00"},
    // The game functions the cross-map summon calls (CrossMapNativeCalls).
    {"SetForcedSummonMap", "seamless core", CoopSiteKind::Call, 0x0156CF10,
     "48 8B 05 61 97 FE 03 8B 0F 89 88 F0 14 00 00 C3"},
    {"SetForcedSummonPosition", "seamless core", CoopSiteKind::Call, 0x0156CF20,
     "48 8B 05 51 97 FE 03 C5 F8 28 07 C5 F8 29 80 00"},
    {"SetForcedSummonOrientation", "seamless core", CoopSiteKind::Call, 0x0156CF40,
     "48 8B 05 31 97 FE 03 C5 F8 28 07 C5 F8 29 80 10"},
    {"SetForcedSummonWarp", "seamless core", CoopSiteKind::Call, 0x0156CF60,
     "48 8B 05 11 97 FE 03 C6 80 20 15 00 00 01 C3"},
    {"SelectSummonedPlacement", "seamless core", CoopSiteKind::Call, 0x01332BC0,
     "55 48 89 E5 41 57 41 56 53 48 83 EC 18"},
    {"SummonedMapReload", "seamless core", CoopSiteKind::Call, 0x01336B90,
     "55 48 89 E5 48 8D 05 2D 45 20 04 48 83 38 00"},
    {"StageTransition", "seamless core", CoopSiteKind::Call, 0x013CDE30,
     "55 48 89 E5 41 57 41 56 41 55 41 54 53 50"},
    {"SetSummonReloadState", "seamless core", CoopSiteKind::Call, 0x0178D9A0,
     "C7 87 84 00 00 00 03 00 00 00 C3"},
    {"MultiPlayStopRequest", "seamless core", CoopSiteKind::Call, 0x01ED07A0,
     "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC"},
    {"AddSpEffect", "seamless core", CoopSiteKind::Call, 0x018C6DB0,
     "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC"},

    {"WorldTeardown.MatchingDecision", "world teardown", CoopSiteKind::Hook, 0x0194716E,
     "48 8B 18 83 BB 84 00 00 00 03"},

    // Deaths that keep the party. 01.09 runs the event scripts' death flow natively, so the
    // change is a few byte patches in PartyGhostDeath / PartyGhostDeath_1 plus a revive where the
    // menu used to be (PartyGhostDeath1.Revive).
    // PartyGhostDeath: SetEventFlag(4047, true) twice. 4047 marks a player on the way out; left
    // set, it would also swallow a later "host left" for the revived guest.
    {"PartyGhostDeath SetEventFlag 4047", "ghost death", CoopSiteKind::Patch, 0x01381FE7,
     "E8 B4 C5 FA FF", "0F 1F 44 00 00"},
    {"PartyGhostDeath SetEventFlag 4047 again", "ghost death", CoopSiteKind::Patch, 0x01381FF6,
     "E8 A5 C5 FA FF", "0F 1F 44 00 00"},
    // CustomLuaCallStart(4063, local player): the host's OnGameLeave drops us.
    {"PartyGhostDeath goodbye", "ghost death", CoopSiteKind::Patch, 0x0138201B, "E8 20 CD FA FF",
     "0F 1F 44 00 00"},
    // SetFlagInitState(2), the reset for the load into our own world.
    {"PartyGhostDeath flag reset", "ghost death", CoopSiteKind::Patch, 0x0138202F,
     "C7 00 02 00 00 00", "66 0F 1F 44 00 00"},
    // PartyGhostDeath_1: skip MissionDeadFailed and OnRequestMenuEnd(PartyGhostDeath_2).
    {"PartyGhostDeath_1 menu branch", "ghost death", CoopSiteKind::Patch, 0x01382359, "75 2C",
     "EB 2C"},
    // PartyGhostDeath_1's `cmp byte ptr [r14+0xC], 0` (ClearBoss), right before the menu branch.
    {"PartyGhostDeath1.Revive", "ghost death", CoopSiteKind::Hook, 0x01382354, "41 80 7E 0C 00"},

    // OnSelfBloodMark(4032, "SelfBloodMark", attributes) in the native g_Initialize:
    // `mov ecx, 0x30` -> 0x70, so a guest can pick up its own bloodstain.
    {"guest bloodstain mark", "guest bloodstain", CoopSiteKind::Patch, 0x0137EBC7, "B9 30 00 00 00",
     "B9 70 00 00 00"},

    // The steal (EnemyIns vtable +0x3E0, 0x018E75A0): cmp byte [rax + 0xC0], 0 (stolen
    // already); `je` to the steal -> nop nop (the return).
    {"echo theft", "no echo theft", CoopSiteKind::Patch, 0x018E7613, "74 09", "90 90"},

    // HostDead: `mov esi, 0xFCF` (start of the white phantom branch) -> `jmp` to the epilogue
    // (0x01381A50).
    {"HostDead white phantom branch", "host death", CoopSiteKind::Patch, 0x013819C6,
     "BE CF 0F 00 00", "E9 85 00 00 00"},
    // Character death step (0x018C49F0): skip removing the session's members from a dying host's
    // world (`jne` -> `jmp` to the same target, then a nop).
    {"character death member removal", "host death", CoopSiteKind::Patch, 0x018C4D1D,
     "0F 85 97 00 00 00", "E9 98 00 00 00 90"},
    // Native SoloPlayDeath_2, replaced while in a session (no world reload).
    {"SoloPlayDeath2.Respawn", "host death", CoopSiteKind::Hook, 0x01381DE0, "55 48 89 E5 41 57"},

    // BlockClear2_2: CustomLuaCallStart(4063, local player), the white phantom's goodbye.
    {"BlockClear2_2 goodbye", "boss clear", CoopSiteKind::Patch, 0x013857AC, "E8 8F 95 FA FF",
     "0F 1F 44 00 00"},
    // BlockClear2_2: OnKeyTime2(4050, "BlockClear2_3Leave"), LeaveSession for every member.
    {"BlockClear2_2 leave", "boss clear", CoopSiteKind::Patch, 0x01385877, "E8 44 59 FA FF",
     "0F 1F 44 00 00"},
    // BlockClear2_3: clear 4047 and 4000 for white phantoms too (`jne` -> 2-byte nop).
    {"BlockClear2_3 flags", "boss clear", CoopSiteKind::Patch, 0x0138596D, "75 1B", "66 90"},
    // BlockClear2_3: skip the white phantom's reset and warp home (`jne` -> `jmp`).
    {"BlockClear2_3 warp home", "boss clear", CoopSiteKind::Patch, 0x0138599C, "75 46", "EB 46"},
    // BlockClear2: the white phantom's SetLoadWait (event state +0x8D), which turns the action
    // buttons off (SprjActionButtonMan +0x52) until the trip home it no longer takes.
    {"BlockClear2 load wait", "boss clear", CoopSiteKind::Patch, 0x01384D03,
     "41 C6 86 8D 00 00 00 01", "0F 1F 84 00 00 00 00 00"},
    // SoloBlockClear (BlockClear2_1 outside a friends party, for a live player): return after
    // the boss flags instead of StopRequest and the tail jump to LeaveSession.
    {"SoloBlockClear leave", "boss clear", CoopSiteKind::Patch, 0x01385A4A,
     "48 8B 05 27 0C 1D 04 48 8B 98 F8 16 00 00", "48 83 C4 08 5B 41 5E 41 5F 5D C3 0F 1F 00"},

    // Co-op areas (0x0191A750): `call EventFlagIsSet` -> `xor eax, eax` + a 3-byte nop; the
    // `xor al, 1` after it then says the area is open.
    {"co-op area flag", "join anywhere", CoopSiteKind::Patch, 0x0191A8B5, "E8 36 F3 A7 FF",
     "31 C0 0F 1F 00"},

    // STEP_C2H_Init_forSelfPrologue: the arriving member's "Discovered the chime of a ...".
    {"chime banner for self", "quiet summon banners", CoopSiteKind::Patch, 0x0196B4C9,
     "E8 B2 49 29 00", "0F 1F 44 00 00"},
    // STEP_C2H_Init_forOthersPrologue: the same for the members it arrives to.
    {"chime banner for others", "quiet summon banners", CoopSiteKind::Patch, 0x0196B9F4,
     "E8 87 44 29 00", "0F 1F 44 00 00"},

    // A released Scaleform movie player is freed 60 frames later, not 5: GXRenderThread still drew
    // one the game had freed (New Game, Kaniel 2026-10-13, eboot+0x1F591B0).
    {"movie player free delay", "movie player delete delay", CoopSiteKind::Patch, 0x01F60837,
     "B9 05 00 00 00", "B9 3C 00 00 00"},

    // The send pace of the unlocked frame rate online (bloodborne_send_pace.h): where the game
    // loads or decrements its frame-counted send counters (CS ChrSync player state +0xC8, its
    // second counter +0xF8, the enemy HP round +0x680), and the event sync's send.
    {"player state send count", "send pace", CoopSiteKind::Hook, 0x01529496,
     "41 8B 9C 24 C8 00 00 00"},
    {"second sync send count", "send pace", CoopSiteKind::Hook, 0x0152C3E9, "89 93 F8 00 00 00"},
    {"enemy HP round count", "send pace", CoopSiteKind::Hook, 0x018EF279, "8B 83 80 06 00 00"},
    // The host's event sync (channel 0x31): an event whose condition holds sends it every frame.
    {"event sync send", "send pace", CoopSiteKind::Hook, 0x012EE540,
     "55 48 89 E5 41 57 41 56 41 55"},

    // The hooks below are described where they are installed (bloodborne_*.cpp).
    {"death flag", "boss death tracer", CoopSiteKind::Hook, 0x018BF941, "80 8B E3 01 00 00 01"},
    {"death flag in the character update", "boss death tracer", CoopSiteKind::Hook, 0x018CE6C5,
     "80 89 E3 01 00 00 01"},
    {"death flag of the game's kill", "boss death tracer", CoopSiteKind::Hook, 0x018BFAD3,
     "41 80 8F E3 01 00 00 01"},
    {"bullet creation", "bullet tracer", CoopSiteKind::Hook, 0x01A03620, "55 48 89 E5 41 57 41 56"},

    {"packet sent", "native packet trace", CoopSiteKind::Hook, 0x0178970D, "4C 8B A5 60 FF FF FF"},
    {"reliable packet queued", "native packet trace", CoopSiteKind::Hook, 0x01790E0E,
     "48 8B 4B 30 48 8B 3C C1"},
    {"unreliable packet queued", "native packet trace", CoopSiteKind::Hook, 0x01790FCA,
     "48 8D 75 C8 4C 89 EF"},
    // The channel table (InstallChannelTable): the send, and both receive paths' channel checks.
    {"packet send", "channel table", CoopSiteKind::Hook, 0x017894E0, "55 48 89 E5 41 57"},
    {"reliable channel check", "channel table", CoopSiteKind::Hook, 0x01790DDE,
     "48 63 43 64 48 83 F8 43"},
    {"unreliable channel check", "channel table", CoopSiteKind::Hook, 0x01790C73,
     "49 89 D6 83 F8 43"},
    // The game's packets in our frames (InstallGamePacketLink): the transport send (the member
    // queues' fill 0x01790C20 is called, not hooked).
    {"transport send", "game packet link", CoopSiteKind::Hook, 0x00C8F0A0, "55 48 89 E5 41 57"},
    {"net sync update", "game packet link", CoopSiteKind::Hook, 0x01789B60, "55 48 89 E5 41 57"},
    // Phase 4's enemy owners (InstallEnemyOwner, see EnemyRole): the HP sync's host-only checks,
    // the receive from the host and the hit on an enemy.
    {"enemy HP round check", "enemy owner", CoopSiteKind::Hook, 0x018EF26C, "48 8B 9D 08 FF FF FF"},
    {"enemy HP state read", "enemy owner", CoopSiteKind::Hook, 0x018EF578, "8B 87 24 01 00 00"},
    {"enemy HP send branch", "enemy owner", CoopSiteKind::Hook, 0x018EF587, "80 BB 64 06 00 00 00"},
    {"enemy HP receive branch", "enemy owner", CoopSiteKind::Hook, 0x018EF684, "49 89 DC 48 85 FF"},
    {"receive from host member", "enemy owner", CoopSiteKind::Hook, 0x01789495,
     "48 81 C3 D0 00 00 00"},
    {"enemy hit state read", "enemy owner", CoopSiteKind::Hook, 0x01A2A102, "8B 88 24 01 00 00"},
    {"activation pass", "enemy owner", CoopSiteKind::Hook, 0x019192A0, "55 48 89 E5 41 57"},
    {"activation pass end", "enemy owner", CoopSiteKind::Hook, 0x0191987E, "8A 5D 97 80 E3 01"},
    {"ownership step end", "enemy owner", CoopSiteKind::Hook, 0x019173B0, "55 48 89 E5 41 57"},
    // Enemy positions (see SendsFullPositionPacket): the owner's send step, a follower's receive.
    {"position packet entry", "enemy owner", CoopSiteKind::Hook, 0x018EE910,
     "48 8B 85 F8 FE FF FF"},
    {"position receive entry", "enemy owner", CoopSiteKind::Hook, 0x018EEBB0, "45 8B 6C 24 FC"},
    // The game's hand-out of enemies (see HandOutSeconds): a guest's receive of 0x17.
    {"hand-out received", "enemy owner", CoopSiteKind::Hook, 0x018ED083, "49 89 D8 89 C3 85 DB"},
    // Native port: include the cutscene observers in the same preflight as the team gate.
    {"cutscene Init", "team lobby", CoopSiteKind::Hook, 0x01CFA050, "55 48 89 E5 41 57 41 56"},
    {"cutscene Prolog", "team lobby", CoopSiteKind::Hook, 0x01CFA310, "55 48 89 E5 41 57 41 56"},
    {"cutscene Load", "team lobby", CoopSiteKind::Hook, 0x01CFA4C0, "55 48 89 E5 41 57 41 56"},
    {"cutscene Play", "team lobby", CoopSiteKind::Hook, 0x01CFA670, "55 48 89 E5 41 57 41 56"},
    {"cutscene End", "team lobby", CoopSiteKind::Hook, 0x01CFB060, "55 48 89 E5 41 57 41 56"},
    {"cutscene Epilog", "team lobby", CoopSiteKind::Hook, 0x01CFB350, "55 48 89 E5 41 57 41 56"},
    {"cutscene EpilogWait", "team lobby", CoopSiteKind::Hook, 0x01CFB5F0, "55 48 89 E5 41 56 53"},
    // Phase 5: the title's save load held for the team, and the title menu seen again.
    {"title load state", "team lobby", CoopSiteKind::Hook, 0x0195B480, "55 48 89 E5 41 57"},
    {"title menu state", "team lobby", CoopSiteKind::Hook, 0x0195AE80, "55 48 89 E5 41 57"},
    // Kaniel 2026-10-15: the cutscene player's (SprjRemoMan) steps, Init .. EpilogWait
    // (SprjRemoManStepOffsets): a cutscene under the team's loading screen is skipped.
    {"cutscene step init", "cutscene skip", CoopSiteKind::Hook, 0x01CFA050,
     "55 48 89 E5 41 57 41 56"},
    {"cutscene step 2", "cutscene skip", CoopSiteKind::Hook, 0x01CFA310, "55 48 89 E5 41 57 41 56"},
    {"cutscene step 3", "cutscene skip", CoopSiteKind::Hook, 0x01CFA4C0, "55 48 89 E5 41 57 41 56"},
    {"cutscene step 4", "cutscene skip", CoopSiteKind::Hook, 0x01CFA670, "55 48 89 E5 41 57 41 56"},
    {"cutscene step 5", "cutscene skip", CoopSiteKind::Hook, 0x01CFB060, "55 48 89 E5 41 57 41 56"},
    {"cutscene step 6", "cutscene skip", CoopSiteKind::Hook, 0x01CFB350, "55 48 89 E5 41 57 41 56"},
    {"cutscene step epilog wait", "cutscene skip", CoopSiteKind::Hook, 0x01CFB5F0,
     "55 48 89 E5 41 56 53"},
    // Kaniel 2026-10-16: the network banner, none in a party (ShowNetworkBannerPrologue).
    {"network banner", "no multiplayer banners", CoopSiteKind::Hook, 0x01BFFE80,
     "55 48 89 E5 41 57 41 56"},
    // Kaniel 2026-10-25: the left member's notice (MemberLeftNoticePrologue).
    {"member left notice", "no multiplayer banners", CoopSiteKind::Hook, 0x0178B0E0,
     "55 48 89 E5 41 57 41 56"},
    // And the session's end's "Session lost." window (SessionLostTest).
    {"session lost window", "no multiplayer banners", CoopSiteKind::Hook, 0x0138A602,
     "F6 80 05 0A 00 00 01"},
    // Website shots 2026-10-19: the game names players by their characters, not online ids
    // (PlayerNameForTextOffset).
    {"name by online id switch", "character names", CoopSiteKind::Patch, 0x01AE8713, "75 14",
     "90 90"},
    {"name by online id option", "character names", CoopSiteKind::Patch, 0x01AE8727, "74 5C",
     "EB 5C"},
    // Kaniel 2026-10-23: a joining guest's own key items go in with no item-received popup
    // (ItemGetQueuedOffset).
    {"item get popup", "silent key items", CoopSiteKind::Hook, 0x016D8850,
     "55 48 89 E5 41 57 41 56"},
    // Kaniel 2026-10-28: a boss's breathers shorten with the boss tempo rule
    // (AddSubGoalLifeHookOffset).
    {"AddSubGoal life", "boss tempo", CoopSiteKind::Hook, 0x0124CCAC, "49 89 FE 31 C9"},
    {"AddSubGoal_Front life", "boss tempo", CoopSiteKind::Hook, 0x0124D53C, "49 89 FE 31 C9"},
    // Kaniel 2026-10-18: no "Lost connection to the network" popup in a party (LanCutError...).
    {"lan cut error", "no lost-connection popup", CoopSiteKind::Hook, 0x01387130,
     "55 48 89 E5 41 56"},
    // Kaniel 2026-10-16: each joining member its own player number (PlayerNumberHandedOut).
    {"player number handed out", "own player numbers", CoopSiteKind::Hook, 0x0178C529,
     "89 85 8C FE FF FF 89 85 DC FE FF FF"},
    // Phase 6: the team join without signs or a bell (bloodborne_join.cpp, OpenTeamSession,
    // TeamSessionJoinData, JoinTeamSession): what the summon manager's update calls.
    {"CreateSession", "team join", CoopSiteKind::Call, 0x01788ED0,
     "55 48 89 E5 41 57 41 56 53 50 C5 FA"},
    {"JoinSession", "team join", CoopSiteKind::Call, 0x01789010,
     "55 48 89 E5 41 57 41 56 53 50 48 89"},
    {"SessionInfo", "team join", CoopSiteKind::Call, 0x00C8F500,
     "55 48 89 E5 41 57 41 56 41 54 53 41"},
    {"JoinParamCopy", "team join", CoopSiteKind::Call, 0x00C90270,
     "55 48 89 E5 41 57 41 56 41 55 41 54"},
    {"JoinParamValid", "team join", CoopSiteKind::Call, 0x00C90D80,
     "55 48 89 E5 41 56 53 49 89 FE 4C 89"},
    {"JoinParamWrite", "team join", CoopSiteKind::Call, 0x00C90B00,
     "55 48 89 E5 41 57 41 56 41 55 41 54"},
    {"JoinParamInit", "team join", CoopSiteKind::Call, 0x00C8FBF0,
     "55 48 89 E5 41 57 41 56 41 54 53 48"},
    {"JoinParamRead", "team join", CoopSiteKind::Call, 0x00C90DD0,
     "55 48 89 E5 41 57 41 56 41 55 41 54"},
    {"JoinParamFree", "team join", CoopSiteKind::Call, 0x00C90520,
     "55 48 89 E5 41 57 41 56 41 55 41 54"},
    {"SummonedNotice", "team join", CoopSiteKind::Call, 0x01FF58B0,
     "55 48 89 E5 41 57 41 56 53 48 81 EC"},

    {"leave limit", "host leaves", CoopSiteKind::Hook, 0x0138A4A0, "55 48 89 E5 53 50"},

    {"LeaveSession", "seamless travel", CoopSiteKind::Hook, 0x015BED10, "55 48 89 E5 41 57"},
    {"stage transition entry", "seamless travel", CoopSiteKind::Hook, 0x013CDE30,
     "55 48 89 E5 41 57"},
    {"CSMultiPlayMan StopRequest", "stop request", CoopSiteKind::Hook, 0x01ED07A0,
     "55 48 89 E5 41 57"},
    {"member leave", "member leave log", CoopSiteKind::Hook, 0x01E54DB0, "55 48 89 E5 41 57"},

    {"RestockItemClamp", "lamps", CoopSiteKind::Call, 0x014DBB70, "55 48 89 E5 48 8D 05"},
    {"RestockFromStorage", "lamps", CoopSiteKind::Call, 0x014DACE0, "55 48 89 E5 41 57"},
    {"BonfireRegene", "lamps", CoopSiteKind::Call, 0x018FAC80, "55 48 89 E5 41 57"},
    {"talk SetEventFlag range", "lamps", CoopSiteKind::Hook, 0x0204232E,
     "8B 88 A0 00 00 00 83 F9 FF"},
    {"ESD is multiplayer", "lamps", CoopSiteKind::Hook, 0x0203C55F, "85 C9 0F 9F C1 08 C1"},
    {"event restart state", "lamps", CoopSiteKind::Hook, 0x012F0FCB, "8B 88 6C 0A 00 00 83 F9 04"},
    {"respawn param check", "lamps", CoopSiteKind::Hook, 0x0190E445, "48 8B 40 08 48 85 C0"},
    {"respawn param check 2", "lamps", CoopSiteKind::Hook, 0x0190E572, "48 8B 40 08 48 85 C0"},
    {"ESD function id", "talk diagnostics", CoopSiteKind::Hook, 0x0203AD95, "89 C1 83 F8 3E"},
    {"ESD function result", "talk diagnostics", CoopSiteKind::Hook, 0x0203EECF,
     "41 C7 44 24 08 02 00 00 00"},
    {"ESD dialog result", "talk diagnostics", CoopSiteKind::Hook, 0x0203EBCB,
     "41 C7 44 24 08 02 00 00 00"},
    {"talk command id", "talk diagnostics", CoopSiteKind::Hook, 0x02040002, "89 C1 83 F8 57"},
    {"talk phantom check", "talk phantom", CoopSiteKind::Hook, 0x0203AC04, "8B 40 78 83 C8 08"},
    {"ESD talker type", "talk host", CoopSiteKind::Hook, 0x0203B7FB, "49 8B 47 18 48 85 C0"},

    {"level sync decision", "level sync", CoopSiteKind::Hook, 0x01507DB5,
     "41 0F B6 46 2C 0F B6 CB"},
    // The team relation tables ([attacker's team][victim's], 32 x 32 bytes: hit as an enemy
    // 0x04731560, hit as a friend 0x04731960; lock-on's 0x05127B90 is made at run time and left
    // alone, a swing hits without it): a co-op guest (team 20) could
    // not hit what the host (19) can, the friendly NPCs (teams 8, 9, 12, 26-28): the hag by the
    // Cathedral Ward lamp took no hits from guests (playtest 2026-10-13, rig ward-npc). Row 20
    // takes row 19's entries there.
    {"guest hits NPCs as enemies", "guests hit NPCs", CoopSiteKind::Patch, 0x047317E8,
     "00 00 00 00 00", "01 01 00 00 01"},
    {"guest hits NPCs as enemies 26-28", "guests hit NPCs", CoopSiteKind::Patch, 0x047317FA,
     "00 00 00", "01 01 01"},
    {"guest hits NPCs as friends", "guests hit NPCs", CoopSiteKind::Patch, 0x04731BE8,
     "00 00 00 00 00", "01 00 00 00 01"},
    {"guest hits NPCs as friends 26-28", "guests hit NPCs", CoopSiteKind::Patch, 0x04731BFA,
     "00 00 00", "01 00 01"},
    {"NPC wander check", "solid NPCs", CoopSiteKind::Hook, 0x018E4FB6,
     "8B 88 45 01 00 00 F6 C1 80"},
    {"NPC wander type check", "solid NPCs", CoopSiteKind::Hook, 0x018D8934,
     "8B 80 45 01 00 00 A8 80"},

    // The guest echo rate loads (`vmovss`/`vmulss` [rip+disp32]) pointed at the 1.0 at
    // 0x04927EBC (RetargetRipLoad).
    {"kill reward echo rate", "full echoes", CoopSiteKind::Patch, 0x018FC947,
     "C5 FA 10 1D 71 B5 02 03", "C5 FA 10 1D 6D B5 02 03"},
    {"echo rate calculation", "full echoes", CoopSiteKind::Patch, 0x018FCAA7,
     "C5 FA 10 1D 21 B4 02 03", "C5 FA 10 1D 0D B4 02 03"},
    {"boss reward echo rate", "full echoes", CoopSiteKind::Patch, 0x01335589,
     "C5 FA 59 05 8B 05 5F 03", "C5 FA 59 05 2B 29 5F 03"},
    {"AddSouls", "echo diagnostics", CoopSiteKind::Hook, 0x018FC600,
     "55 48 89 E5 41 56 53 48 83 EC 10"},

    {"menu client condition", "menu client", CoopSiteKind::Hook, 0x01A13FE5, "08 C1 0F B6 F1"},
    {"map item packet header", "map item sync", CoopSiteKind::Hook, 0x017D33E1,
     "8B 85 60 FE FF FF 66 83 F8 01"},
    {"map item create state check", "shared loot", CoopSiteKind::Hook, 0x017DC3CF,
     "83 B8 24 01 00 00 06"},
    {"map item create session check", "shared loot", CoopSiteKind::Hook, 0x017DC3DC,
     "83 BA 98 00 00 00 FF"},
    {"map item handle state check", "shared loot", CoopSiteKind::Hook, 0x017D52D2,
     "83 B8 24 01 00 00 06"},
    {"map item take state check", "shared loot", CoopSiteKind::Hook, 0x017D85F4,
     "83 B8 24 01 00 00 06"},
    {"map item handle session check", "shared loot", CoopSiteKind::Hook, 0x017D52DB,
     "41 83 BF 98 00 00 00 FF"},
    // Kaniel's playtest 2026-10-16: an enemy's drop (0x017D5320, MapItemMan +0x38) gets its own
    // handle on a guest too (DropHandleStateCheckOffset).
    {"drop handle state check", "shared loot", CoopSiteKind::Hook, 0x017D5642,
     "83 B8 24 01 00 00 06"},
    {"drop handle session check", "shared loot", CoopSiteKind::Hook, 0x017D564B,
     "41 83 BF 98 00 00 00 FF"},
    // Kaniel's playtest 2026-10-16 (the Cathedral Ward hag): an enemy's item lot on a guest keeps
    // only the items for co-op phantoms (the one-time ones go); a guest's rolls as solo play does.
    {"drop lot guest state check", "shared loot", CoopSiteKind::Hook, 0x017DD17C,
     "83 B8 24 01 00 00 06"},
    {"drop lot guest session check", "shared loot", CoopSiteKind::Hook, 0x017DD186,
     "41 83 BF 98 00 00 00 FF"},
    {"map item pickup check", "map item pickup", CoopSiteKind::Hook, 0x017D6831,
     "80 B8 92 15 00 00 00"},
    {"map item pickup check 2", "map item pickup", CoopSiteKind::Hook, 0x017D6AEF,
     "80 B8 92 15 00 00 00"},

    // ItemModuleChecks (SHADPS4_BLOODBORNE_RE_TRACE only).
    {"item check 0x017D3759", "item diagnostics", CoopSiteKind::Hook, 0x017D3759,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D4F54", "item diagnostics", CoopSiteKind::Hook, 0x017D4F54,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D6818", "item diagnostics", CoopSiteKind::Hook, 0x017D6818,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D6AD2", "item diagnostics", CoopSiteKind::Hook, 0x017D6AD2,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D83CB", "item diagnostics", CoopSiteKind::Hook, 0x017D83CB,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D853B", "item diagnostics", CoopSiteKind::Hook, 0x017D853B,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D88BB", "item diagnostics", CoopSiteKind::Hook, 0x017D88BB,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D9AA8", "item diagnostics", CoopSiteKind::Hook, 0x017D9AA8,
     "83 B9 24 01 00 00 06"},
    {"item check 0x017D9E1C", "item diagnostics", CoopSiteKind::Hook, 0x017D9E1C,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017D9EE0", "item diagnostics", CoopSiteKind::Hook, 0x017D9EE0,
     "83 B9 24 01 00 00 06"},
    {"item check 0x017DDCD9", "item diagnostics", CoopSiteKind::Hook, 0x017DDCD9,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017DE5EC", "item diagnostics", CoopSiteKind::Hook, 0x017DE5EC,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017DE738", "item diagnostics", CoopSiteKind::Hook, 0x017DE738,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017DE79D", "item diagnostics", CoopSiteKind::Hook, 0x017DE79D,
     "83 B8 24 01 00 00 03"},
    {"item check 0x017DE8CB", "item diagnostics", CoopSiteKind::Hook, 0x017DE8CB,
     "83 B8 24 01 00 00 06"},
    {"item check 0x017DE925", "item diagnostics", CoopSiteKind::Hook, 0x017DE925,
     "83 B8 24 01 00 00 03"},

    {"item button update", "item button diagnostics", CoopSiteKind::Hook, 0x012FD8CD,
     "48 8D 7D C0 41 BC 04 10 00 00"},
    {"item button found", "item button diagnostics", CoopSiteKind::Hook, 0x012FDC17,
     "8B 45 A8 89 45 9C 85 C0"},
    {"item button not busy", "item button diagnostics", CoopSiteKind::Hook, 0x012FDC84,
     "49 8B 85 B0 03 00 00"},
    {"item button pressed", "item button diagnostics", CoopSiteKind::Hook, 0x012FDD95,
     "49 8B 85 B0 03 00 00"},
    {"map item search examined", "item button diagnostics", CoopSiteKind::Hook, 0x017DA58B,
     "C4 C1 7A 10 46 30"},
    {"map item search in range", "item button diagnostics", CoopSiteKind::Hook, 0x017DA7CE,
     "41 8B 46 20 48 8B 8D 10 FF FF FF"},

    {"lamp registration start", "lamp registration", CoopSiteKind::Hook, 0x0146DB21,
     "44 89 63 14 45 85 E4"},
    {"lamp registration end", "lamp registration", CoopSiteKind::Hook, 0x0146DC0B,
     "44 89 7B 18 C6 43 11 01"},
    {"award item lot", "boss items", CoopSiteKind::Hook, 0x017DDC50,
     "55 48 89 E5 41 57 41 56 41 55"},
    {"remote set flag", "keep local flags", CoopSiteKind::Hook, 0x017C0565,
     "45 84 FF 0F 95 C0 0F B6 D0 89 DE"},
    {"remote end event", "keep local flags", CoopSiteKind::Hook, 0x017C0901,
     "84 DB 0F 95 C0 0F B6 C8 44 89 FE 44 89 F2"},
    {"IsNoDeadExit", "party death", CoopSiteKind::Hook, 0x01A57923, "44 88 F0 48 83 C4 08"},
    {"hit", "own hits", CoopSiteKind::Hook, 0x01A2A770, "55 48 89 E5 41 57 41 56 41 55"},
    {"hit applied", "own hits", CoopSiteKind::Hook, 0x01A2C05F, "49 8B 4E 08 48 8B 91 B0 03 00 00"},
    {"status effect SetHp", "effect hits", CoopSiteKind::Hook, 0x01A2FB5D, "31 D2 31 C9 4C 89 E7"},
    {"status effect SetHp 2", "effect hits", CoopSiteKind::Hook, 0x01A2FBF9,
     "31 D2 31 C9 4C 89 E7"},
    // Party death (HitDamageStored, HitReactionPrologue) and the port's party HUD
    // (HudTagUpdateExit, HudTicksTagsDone, HudConstruct).
    {"hit damage stored", "real revive hits", CoopSiteKind::Hook, 0x01A2BCA3, "89 81 2C 02 00 00"},
    {"hit reaction", "no hit reaction while down", CoopSiteKind::Hook, 0x01A2E570,
     "55 48 89 E5 41 57 41 56"},
    {"enemy bar damage", "no damage number on downed bodies", CoopSiteKind::Hook, 0x016F9A50,
     "45 31 C0 48 89 F8 31 C9"},
    {"entity in area", "no area events while down", CoopSiteKind::Hook, 0x013189A0,
     "55 48 89 E5 41 57 41 56"},
    {"HUD tag update exit", "party nameplates", CoopSiteKind::Hook, 0x01B5A3AE,
     "48 81 C4 C8 00 00 00"},
    {"HUD tags done", "party list in the HUD", CoopSiteKind::Hook, 0x01BF8A50,
     "49 8B BF F8 00 00 00"},
    {"HUD constructor", "party list in the HUD", CoopSiteKind::Hook, 0x01BF41C0,
     "55 48 89 E5 41 57 41 56"},

    // Trading (bloodborne_trade.cpp): the inventory's find and count change, and the pickup's give.
    {"InventoryFind", "trading", CoopSiteKind::Call, 0x014D9F60, "8B 06 83 F8 FF 0F 84 A6"},
    {"InventoryChangeCount", "trading", CoopSiteKind::Call, 0x014D0BE0, "55 48 89 E5 41 57 41 56"},
    {"GiveItems", "trading", CoopSiteKind::Call, 0x017D89F0, "55 48 89 E5 41 57 41 56"},

    // The sign poster's timers (SignTimerSites): `mov dword ptr [<reg> + 0x28], <seconds>`. Game
    // patches rewrite the re-post time (the bundled "Uncap FPS++" writes 60 s), and the store is
    // ours to rewrite all the same: any time is accepted.
    {"first sign post delay", "sign timers", CoopSiteKind::Patch, 0x018A9264,
     "C7 47 28 00 00 A0 40", "C7 47 28 00 00 00 00", "C7 47 28 ?? ?? ?? ??"},
    {"first sign post delay 2", "sign timers", CoopSiteKind::Patch, 0x018A9324,
     "C7 47 28 00 00 A0 40", "C7 47 28 00 00 00 00", "C7 47 28 ?? ?? ?? ??"},
    {"sign re-post time", "sign timers", CoopSiteKind::Patch, 0x018A9757, "C7 43 28 00 00 F0 41",
     "C7 43 28 00 00 80 3F", "C7 43 28 ?? ?? ?? ??"},
    {"sign re-post time 2", "sign timers", CoopSiteKind::Patch, 0x018A988F, "C7 43 28 00 00 F0 41",
     "C7 43 28 00 00 80 3F", "C7 43 28 ?? ?? ?? ??"},

    // How long a host's summon in flight holds a summon slot (PartySummonSlotHoldConstant): the
    // summon manager's `vucomiss xmm0, [rip+disp32]` against its 55 s and 180 s, pointed at a
    // 15.0 (RetargetRipLoad).
    {"summon slot hold", "summon slot hold", CoopSiteKind::Patch, 0x018729D9,
     "C5 F8 2E 05 77 50 0B 03", "C5 F8 2E 05 E3 4E 0B 03"},
    {"summon slot hold 2", "summon slot hold", CoopSiteKind::Patch, 0x018729ED,
     "C5 F8 2E 05 67 50 0B 03", "C5 F8 2E 05 CF 4E 0B 03"},

    // Phase 7: only the server ends a session (InstallServerOwnsLeaves). The game's own reasons,
    // each followed by its Lua event and MultiPlayStopRequest, take the stay branch: NetSync's
    // update (0x01789B60) with SprjSessionManager's idle timer +0x2E0 past its limit +0x2F0
    // (`jbe` -> `jmp`), and more than 1000 bytes in its resend queue (`jb` -> `jmp`); the room
    // time-out ("OnRoomTimeOut", `jbe` -> `jmp`). Not the session layer's reason 3 to NetSync
    // (0x0178F9A0, "OnIrregularLeaveSession" when a member's connection goes): skipping it left a
    // host whose guest went unable to pick anything up (rig crashes/leave).
    {"session idle timeout", "server owns leaves", CoopSiteKind::Patch, 0x01789C5D, "76 78",
     "EB 78"},
    {"session resend overflow", "server owns leaves", CoopSiteKind::Patch, 0x017897B2,
     "0F 82 B8 00 00 00", "E9 B9 00 00 00 90"},
    {"room timeout", "server owns leaves", CoopSiteKind::Patch, 0x01789F41, "76 79", "EB 79"},
    // Lua's OnMatchingCheck (0x013808B0): a member its check (0x01338CE0) does not find ends the
    // session (MultiPlayStopRequest) and sends this game home. The old host back as a guest after
    // a host migration (rig party3/leader-returns3) failed it and left every few seconds; the
    // failed check returns (`je` -> two nops), as its other branch does after a found member.
    {"matching check", "server owns leaves", CoopSiteKind::Patch, 0x0138095F, "74 0B", "90 90"},

    // The character floor update's read of the floor (InstallFloorGuard): a floor of a map block
    // that is gone is cleared first.
    {"character floor read", "floor guard", CoopSiteKind::Hook, 0x018CB14F,
     "48 8B B3 88 02 00 00 48 85 F6"},
    // And the "stands on part X" query's (StandsOnPartFloorRead; Kaniel's crash 2026-10-17).
    {"stands on part floor read", "floor guard", CoopSiteKind::Hook, 0x0191B79E,
     "48 8B 8E 88 02 00 00 48 85 C9"},
    // And a remote player's update's read of the object its physics stands on
    // (ClearContactObjectThatIsGone; Kaniel's crash 2026-10-19).
    {"contact object read", "floor guard", CoopSiteKind::Hook, 0x01524B0C, "4C 8B A8 A8 00 00 00"},

    // The event flag manager's read of a value over two flag blocks (0x013CFD80), where it reads
    // each block (InstallFlagBlockGuard): a missing block reads as zeros (rig 2026-10-16: a game
    // joining a session for the second time crashed at 0x013CFFBA).
    {"flag value first block", "flag block guard", CoopSiteKind::Hook, 0x013CFF79,
     "41 FF C9 44 89 C9 C1 E9 03"},
    {"flag value second block", "flag block guard", CoopSiteKind::Hook, 0x013CFFEC,
     "45 29 D0 41 8D 58 FF 89 D8"},

    // A chalice map's view registration reads the current altar's pick flags 9020-9026
    // (InstallChaliceAltarPick): a load builds the altar picked last, not the save's (Kaniel's
    // playtest 2026-10-28: a party trip built altar 6's old dungeon in altar 5's).
    {"chalice view pick read", "chalice altar pick", CoopSiteKind::Hook, 0x013BEB4D,
     "31 C9 85 C0 44 8B 75 D4"},

    // The game's per-character receive of a remote player's state (InstallMemberPacketGuard): only
    // the newest copy of a member takes its packets (Kaniel's playtest 2026-10-19).
    {"member receive", "packet guard", CoopSiteKind::Hook, 0x01A6D390, "55 48 89 E5 41 57"},

    // Kaniel 2026-10-25: the frame step's read of the camera manager after its update (0x014368B0,
    // 0x014377D0), before the frame's cameras are copied from it (InstallCoopCamera): the
    // spectator's and the free camera go in there.
    {"camera manager result", "co-op camera", CoopSiteKind::Hook, 0x0193BE5A,
     "49 8B 85 28 01 00 00"},

    // Kaniel 2026-10-25: the game's KillCharacter (an event's ForceCharacterDeath and the like),
    // which never asks IsNoDead of the player: a boss fight's kill of it spectates instead.
    {"kill character", "boss fight deaths", CoopSiteKind::Hook, 0x018BF9A0, "55 48 89 E5 41 57"},
});

// The flag block guard's choice: a block that is there, else the zeros it reads instead.
constexpr u64 GuardedFlagBlock(u64 block, u64 zeros) {
    return block != 0 ? block : zeros;
}

} // namespace Core::Bloodborne
