// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Phase 5: the team lobby on the game's side (docs/SERVER_COOP_PLAN.md, "The team lobby").
// In the port (always, phase 6) the party is not a setting: the game signs in to the server's
// lobby, and a team's Start names the party (its key and who hosts). The load of a save is held
// on the title until then (TeamGate): a member's Continue or Load Game is its Ready, the leader's
// is the team's Start.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"
#include "coop.pb.h"

namespace Core::Bloodborne {

// Whether every other member of the team of `me` is online and ready, or the team started already
// (the rig's leader begins then; Kaniel 2026-10-20, the lobby itself starts without one not ready
// yet, which goes in once its screen says ready).
bool TeamCanBegin(const bbcoop::LobbyState& lobby, const std::string& me);
// Whether `me` is in a team and ready in it (its team screen says so by itself).
bool TeamMemberReady(const bbcoop::LobbyState& lobby, const std::string& me);

// Kaniel 2026-10-15: the player slot a guest joins the host's session with (GameMan +0x16F4), its
// own in the team: 7 for the first guest (the summon sign's), 6 for the second, 5 for the third.
// Phase 7: a member with no slot of its own (the leader) joining another member's session (that
// member hosts it now, host migration) takes the slot the session's host left.
u32 TeamGuestSign(const bbcoop::LobbyState& lobby, const std::string& me,
                  const std::string& session_host = {});

// Phase 7: what the team screen says when the server refused this game's link (another build),
// from the server's reason; empty when it did not.
std::string LinkRefusalNotice(std::string_view refusal);
// The team screen's notice: the lobby's own (`lobby_notice`) first, else the first line of the
// server's warning that this game's installation differs (`link_warning`, Kaniel 2026-10-25).
std::string TeamScreenNotice(std::string_view lobby_notice, std::string_view link_warning);

// The members the team loading screen shows (member, name), the leader first: this game and those
// coming (ready, not offline); not one gone or staying at the title (Kaniel 2026-10-15).
std::vector<std::pair<std::string, std::string>> TeamLoadMembers(const bbcoop::LobbyState& lobby,
                                                                 const std::string& me);

class TeamGate {
public:
    enum class Step { Load, Hold };
    // The team screen's buttons (bbport_overlay.cpp): the leader's Start (a member's is its
    // Ready), or, in no team, this save plays alone. Ready is the screen's own (Kaniel
    // 2026-10-20: no Ready to press): Decision::ready.
    enum class Choice { Ready, Start, Alone };
    // A member's Ready not answered by the lobby is said again after this long.
    static constexpr s64 ReadyAgainMs = 3000;
    struct Decision {
        Step step = Step::Load;
        std::optional<bbcoop::TeamStart> start; // with Load: the team the save plays in
        bool ready = false; // send this save's Ready now (Choose(Choice::Ready))
    };

    void TakeLobby(const bbcoop::LobbyState& state) {
        lobby = state;
    }
    void TakeStart(const bbcoop::TeamStart& start) {
        started = start;
    }
    // Every frame the title wants to load the save in `slot`: held until the team starts or the
    // player plays alone.
    Decision Tick(const std::string& me, u32 slot, s64 now_ms);
    // A button of the screen: the requests to send.
    std::vector<bbcoop::TeamRequest> Choose(Choice choice, const std::string& me);
    // Back at the title: the next load asks again.
    void Reset();

    const std::optional<bbcoop::LobbyState>& Lobby() const {
        return lobby;
    }
    bool Holding() const {
        return holding_since.has_value();
    }
    // The team's Start as it came (a save playing alone takes it in game: TeamPlayStepOf), and
    // forgotten once the save is out of that team.
    const std::optional<bbcoop::TeamStart>& Start() const {
        return started;
    }
    void DropStart() {
        started.reset();
    }

private:
    std::optional<bbcoop::LobbyState> lobby;
    std::optional<bbcoop::TeamStart> started;
    std::optional<s64> holding_since;
    std::optional<s64> ready_said; // when this load's Ready went
    u32 slot = 0;
    bool alone = false;
};

// Kaniel 2026-10-16: the team menu in game (the team screen over the game, from the System menu:
// invite, remove a member, leave, join the hunt). What a game playing a save does about its team
// each frame: `party` is the team's party it plays in ("" alone), `start` the team's last Start.
//  - Leave: the lobby says it is out of that team (its leader removed it, or it left): it leaves
//    the team's session where it stands and plays on alone, in its own world (no reload).
//  - Adopt: playing alone, the Start of the team it is ready in came (it joined the hunt from the
//    menu): it takes the team's party, and joins the session in place (TeamJoin).
//  - Lead (Kaniel 2026-10-25): it plays in the team as a guest (not `hosts`) and the lobby made it
//    the leader (the host left): it takes the host's role.
//  - Follow (Kaniel 2026-10-25): it hosts, another member leads and that leader's session is the
//    team's (`session_host`, as the server last said; two guests took over at once): it leaves
//    its own session and joins the leader's as a guest.
// The team menu closed (TeamMenu): opened from the game's System page, the pad presses Circle from
// AfterMs after for HoldMs so that page closes too; opened from the port's Insert menu, nothing.
class TeamMenuBack {
public:
    static constexpr s64 AfterMs = 100;
    static constexpr s64 HoldMs = 150;
    void Closed(s64 now_ms, bool from_page) {
        closed_ms = from_page ? std::optional<s64>{now_ms} : std::nullopt;
    }
    bool Pressed(s64 now_ms) const {
        return closed_ms && now_ms - *closed_ms >= AfterMs &&
               now_ms - *closed_ms < AfterMs + HoldMs;
    }

private:
    std::optional<s64> closed_ms;
};

enum class TeamPlayStep { None, Adopt, Leave, Lead, Follow };
TeamPlayStep TeamPlayStepOf(const bbcoop::LobbyState* lobby, std::string_view party,
                            const bbcoop::TeamStart* start, const std::string& me = {},
                            bool hosts = true, std::string_view session_host = {});
// Kaniel 2026-10-25: the team's host now: its leader (the server's lobby makes the member hosting
// the session the leader) when the lobby speaks of the Start's team, else the Start's host.
std::string TeamHostOf(const bbcoop::LobbyState* lobby, const bbcoop::TeamStart& start);
// The menu's "Join the hunt" in game: the invitation to `invite_team` accepted first (when in no
// team), then this save ready with its slot and character; the leader's starts the team too.
// Kaniel 2026-10-16: the leader's also picks `world`, this save's world ("" a new one).
std::vector<bbcoop::TeamRequest> TeamJoinHuntRequests(const bbcoop::LobbyState& lobby,
                                                      const std::string& me,
                                                      const std::string& invite_team, u32 slot,
                                                      const std::string& character,
                                                      std::string_view world = {});

// The team screen's request `action` naming `id`, as the menu sends it (TeamLobbyRequest): a team
// (accept, decline), a world (pick, forget), a typed code (JOIN_CODE: cleaned up,
// NormalizeTeamCode; kept as typed when it cannot be one, the server's refusal says so), a typed
// name (INVITE_NAME), else a member.
bbcoop::TeamRequest TeamMenuRequest(bbcoop::TeamRequest::Action action, const std::string& id);

// Kaniel 2026-10-16: a new character got the team's old world. Kaniel 2026-10-17: the worlds are
// the server's, apart from its teams (bbcoop::TeamWorldInfo, LobbyState.worlds: any team plays
// any of them) and the leader picks the one its Start plays. The world the team screen starts on
// for the leader's save: the world it last played (`hint`, kept per save slot) while the server
// lists it; a new one ("") for a new character (`fresh`: the game starts it at the beginning) or
// a save that never played in a team.
std::string DefaultTeamWorld(const bbcoop::LobbyState& lobby, std::string_view hint, bool fresh);
// Kaniel 2026-10-17: the world is picked before the team is made. What the team screen asks first
// (a WORLD request's world): DefaultTeamWorld for a player in no team (its next team takes it) or
// a leader whose team does not play yet, when that is not the pick already; else nothing.
std::optional<std::string> TeamScreenWorldPick(const bbcoop::LobbyState& lobby,
                                               const std::string& me, std::string_view hint,
                                               bool fresh);
// What the screen calls a world.
std::string TeamWorldName(const bbcoop::TeamWorldInfo& world);
// Kaniel 2026-10-25 ("that should be an option to configure instead of nightreign deaths, this one
// should be off by default"): how a member's lethal hit plays out while a teammate stands on its
// map. One for the whole team (a per-player switch would split one party's deaths): the leader
// picks it on the team screen and the server sends its pick to every member (Team.death_mode).
// Down for a revive is the default, and what a game in no team plays.
enum class DeathMode : u8 { GoDown = 0, Spectate = 1 };
DeathMode TeamDeathMode(const bbcoop::LobbyState& lobby);
// Kaniel 2026-10-19: the team screen at the title called everyone Hunter until the save loaded.
// The name the lobby hears while the title loads a slot: the character name last seen in that
// slot (`slot_name`, a file's line, kept per slot), none for a new character (`fresh`) or a slot
// never played with its name kept (never the last character's name: it may be another one's).
std::string TitleLoadCharacterName(bool fresh, std::string_view slot_name);
// Whether the game's own character name is this save's: no title load since the start
// (`title_load_ms` 0), or the save file opened after the title's load (`save_opened_ms`) at least
// CharacterNameSettleMs ago (until then the game still holds the last character's).
constexpr s64 CharacterNameSettleMs = 3'000;
bool CharacterNameReadable(s64 title_load_ms, s64 save_opened_ms, s64 now_ms);

// Phase 5b: a team's world (bbcoop::TeamWorld). The world's part of the event flags (FlagOwner
// Host and Shared, docs/FLAGS.md; a character's own flags and its map item pickups, which go with
// the items they gave, are never in it): the ones on, in the
// flag blocks the store had. The team's host reports its own; every member's save takes the team's
// as it loads, its own kept aside (a file per save slot) until it plays alone again.
struct WorldFlags {
    std::vector<s32> on;     // sorted
    std::vector<u32> blocks; // sorted
    bool operator==(const WorldFlags&) const = default;
};
bool IsWorldFlag(s32 flag);
// One block of the event flag store: flag = id * flags_per_block + byte * 8 + bit (MSB first).
struct FlagBlockBits {
    u32 id = 0;
    std::span<const u8> bytes;
};
// `all`: every flag, not only the world's (an invader's own world kept aside for its visit,
// bloodborne_invasion.h).
WorldFlags CaptureWorldFlags(u32 flags_per_block, std::span<const FlagBlockBits> blocks,
                             bool all = false);
// What to set so the store's world flags (`all`: every flag) are `target`'s, in the blocks both
// have (a block the target does not cover keeps its flags): each flag and its value, in order.
std::vector<std::pair<s32, bool>> WorldFlagChanges(u32 flags_per_block,
                                                   std::span<const FlagBlockBits> blocks,
                                                   const WorldFlags& target, bool all = false);
// Whether a load takes the team's world in `block`: one the team's world covers (`covered`,
// sorted) that no earlier load took (`taken`), or on a guest's trip (`guest_trip`) every one. A
// trip's load gives the guest's game its own saved world back (Kaniel's play test 2026-10-18:
// the opening's flag, 12410000, only the team's world had was off again, and the opening
// played), the global flags too: the NPCs' steps (Kaniel's play test 2026-10-19: krolebord saw
// the newborn in Oedon Tomb's basement and Lisara NPCs by the Cathedral Ward lamp that
// the team's progress had not brought yet). The team's world is also newer than the last take:
// the host's progress since shows from the guest's next trip.
bool TakesTeamWorldBlock(u32 block, const std::set<u32>& taken, std::span<const u32> covered,
                         bool guest_trip);
// Kaniel's players 2026-10-28/29: an ending (the Dream's 12100000, 12100002, 12100180) starts the
// next playthrough (2003:21): the game puts its world back to a new game's and reloads the save,
// its clear count (GameDataMan +0x68, the GetClearCount event function) one up. The team's world
// from before it then went over that load: every lamp and boss back (Provi's day 4), and the
// Dream's ending flag on again, so the ending played at the next visit, NG+ 1, 2, 3 (Lisara's
// group). A load is the next playthrough's when its save slot's clear count is past the one
// the last load of that slot saw in this game.
class PlaythroughWatch {
public:
    // Notes a load of `slot` with its clear count: whether it starts a new playthrough.
    bool NextPlaythrough(int slot, s32 clear_count);

private:
    std::map<int, s32> seen;
};
// The lamps lit in the team's world in the blocks a load took (`blocks`), as lamp entities
// (LampOfActiveFlag). A lamp stays lit only for a character whose lamp list holds it, so they go on
// the guest's list as its flags go in (Kaniel's play test 2026-10-18: krolebord's lamps listed only
// Cathedral Ward, the one his own save had).
std::vector<s32> TeamWorldLamps(u32 flags_per_block, std::span<const u32> blocks,
                                const WorldFlags& target);
// The lamp flags (a lamp's active flag and the one after, LampOfActiveFlag) on in the team's world
// that are off in this game (`on`): a guest's join reload and its trips give its game its own saved
// world back, which put the team's lamps out again in every map but the host's (the rig's
// team-world-lamps), so each guest load sets them again. Never one off.
std::vector<s32> TeamLampFlagsToSet(const WorldFlags& team, const std::function<bool(s32)>& on);
// The personal world kept aside, as its file has it ("blocks" then "on", a number a line).
std::string FormatWorldFlags(const WorldFlags& world);
std::optional<WorldFlags> ParseWorldFlags(std::string_view text);
WorldFlags WorldFlagsOf(const bbcoop::TeamWorld& world);
// The time of day (9800 evening, 9801 night, 9802 blood moon: the maps' events pick their sky and
// enemies by them as they load) the team's world has and this game's (`on`) lacks: a guest away
// from the host's cutscene that brought it (Vicar Amelia's skull, Kaniel's playtest 2026-10-17)
// takes it, and its next load shows it. Never one back.
inline constexpr std::array<s32, 3> TimeOfDayFlags{9800, 9801, 9802};
std::vector<s32> TimeOfDayToFollow(const WorldFlags& team, const std::function<bool(s32)>& on);
// At a guest's load, the time of day flags that differ from the team's world, as (flag, on), back
// as well as on: the maps' events pick their characters by it as they load (Kaniel's playtest
// 2026-10-22: krolebord, alone in Old Yharnam, saw his own save's blood-moon characters). None
// when the team's world does not cover their block.
std::vector<std::pair<s32, bool>> TeamTimeOfDayAtLoad(const WorldFlags& team, u32 flags_per_block,
                                                      const std::function<bool(s32)>& on);
// The story the host moved on since `before` (quest steps, NPCs: Host flags outside the maps'
// 12xxxxxx-13xxxxxx and the item flags) in the blocks both team worlds cover, as (flag, on): a
// guest away from the host takes them as the host's reports bring them (Kaniel 2026-10-20: NPCs
// showed up for guests on maps without the host), not only at its next trip. Only the host's
// changes: what the guest's own game did to the others stays. Not the time of day
// (TimeOfDayToFollow, never back). None when the blocks differ (loads take that world).
std::vector<std::pair<s32, bool>> TeamStoryToFollow(const WorldFlags& before,
                                                    const WorldFlags& after);
// A guest's kept session change (KeepWorldChanges, `kept_ms` when this game kept it, none when it
// came from the file) that a team guest's load leaves to the team's world: a story step
// (TeamStoryToFollow's kind) in a block the team's world covers, kept longer ago than the host's
// reports take to carry it (Kaniel's playtest 2026-10-22: a guest alone on a map saw NPCs from its
// own game's steps, put over the team's world as the map loaded).
inline constexpr s64 KeptChangeReportMs = 20'000;
bool KeptChangeYieldsToTeam(s32 flag, std::optional<s64> kept_ms, s64 now_ms,
                            std::span<const u32> team_blocks, u32 flags_per_block);

// Phase 6: the team join without signs or a bell (docs/native-netcode/session.md, "Team join").
// The team's host opens its game session itself (NetSync CreateSession) once it stands in its
// world, and sends the session's join data (the game's own: what a summon sends its guest) to the
// server (bbcoop::TeamSession); every guest's game joins that session (NetSync JoinSession) when it
// stands alone in its world. TeamJoin says each frame what this game does.
class TeamJoin {
public:
    // TakeHost (phase 7): the server passed the room to this guest; it hosts the session now.
    // LeaveAlone (phase 7): a guest alone in its session for AloneLeaveMs leaves it.
    enum class Action { None, CreateSession, Publish, PublishClosed, Join, TakeHost, LeaveAlone };
    static constexpr s64 AloneLeaveMs = 5'000;
    struct Frame {
        bool host = false;
        s32 matching_state = 0; // the session manager's +0x124: 0 alone, 3 hosting, 6 a guest
        bool ready = false;     // the party controller's go (in the world, settled, retry spaced)
        bool session_known = false;   // a guest: the server sent its host's open session
        bool room_owner = false;      // the server passed the session's room to this game
        bool room_has_others = false; // another member is in that room (not the one who left)
        size_t session_members = 2;   // the game's session, this game counted
        bool standing = true;         // the player is up (not down or dead)
        s64 now_ms = 0;
    };
    Action Tick(const Frame& frame);

private:
    bool published = false;         // the host said its open session
    std::optional<s64> alone_since; // a guest alone in its session since
};

// Kaniel 2026-10-14: the bell items are out of the port's game (the team joins without them):
// Beckoning Bell 200, Small Resonant Bell 205, Sinister Resonant Bell 225, Old Hunter Bell 4312
// (EquipParamGoods ids, names from item.msgbnd). The Choir Bell (2050) is a hunter tool and stays.
inline constexpr std::array<u32, 4> BellGoods{200, 205, 225, 4312};
constexpr bool IsBellGoods(u32 goods) {
    for (const u32 bell : BellGoods) {
        if (bell == goods) {
            return true;
        }
    }
    return false;
}

// A goods the player holds and how many (the inventory's key items: its first part, below its
// split at +0x24).
struct HeldGoods {
    u32 goods = 0;
    s32 count = 0;
    bool operator==(const HeldGoods&) const = default;
};
// Kaniel 2026-10-18 (krolebord's Sword Hunter Badge): in the join's state (character type 1, a
// summoned phantom, until its first trip) a guest's key items are a list of the game's own
// (inventory +0x58 points elsewhere). What the guest gains there (a boss's badge or key, an NPC's
// gift, a pickup) the game drops when the guest's own list comes back at its first trip. What to
// give back then: the key items of that list's last look beyond its first look (gained there),
// less what the guest's own list holds now; never a bell (RemoveBellItems takes them).
std::vector<HeldGoods> PhantomKeyItemsToGiveBack(std::span<const HeldGoods> first_look,
                                                 std::span<const HeldGoods> last_look,
                                                 std::span<const HeldGoods> home);
// Kaniel's play test 2026-10-19: krolebord's Saw Hunter Badge and Workshop Tool were gone the whole
// session (he never took a trip, so he stayed in the join's state): the phantom's list has none of
// the guest's own key items. What of its own list (`own`, its last look before the join) to put in
// the phantom's (`phantom`) as it comes: each one the phantom's lacks, never a bell. The game drops
// them with that list, and PhantomKeyItemsToGiveBack gives none back twice (home holds them).
std::vector<HeldGoods> OwnKeyItemsForPhantom(std::span<const HeldGoods> own,
                                             std::span<const HeldGoods> phantom);

// Kaniel 2026-10-23: a joining guest saw the game's "item received" popup ("Oedon Tomb Key 1 /
// OK") once for each of its own key items OwnKeyItemsForPhantom put in the phantom's list. While
// one lives on a thread, the gives on that thread queue no popup (and no pickup sound): the hook
// on the popup's queue push (ItemGetQueuedOffset) skips it. Nested ones are fine.
class SilentItemGets {
public:
    SilentItemGets();
    ~SilentItemGets();
    SilentItemGets(const SilentItemGets&) = delete;
    SilentItemGets& operator=(const SilentItemGets&) = delete;
};
bool ItemGetsSilent();

// Event flag 4047 ("a player on the way out", docs/FLAGS.md): set when a session ends under a
// player; a summoned guest's reload home cleared it. A team's guest stays in its world (no reload),
// so once it stands out of a session with the flag on for ClearAfterMs, it is cleared: the party
// controller waits while it is on, and the guest would never join its host again.
class LeavingFlag {
public:
    static constexpr s32 Flag = 4047;
    static constexpr s64 ClearAfterMs = 5'000;
    bool Clear(bool flag_on, bool in_session, s64 now_ms) {
        if (!flag_on || in_session) {
            since_ms.reset();
            return false;
        }
        if (!since_ms) {
            since_ms = now_ms;
        }
        if (now_ms - *since_ms < ClearAfterMs) {
            return false;
        }
        since_ms.reset();
        return true;
    }

private:
    std::optional<s64> since_ms;
};

// A team's guest stays in its world (no reload home), so a white phantom whose session is over is
// made a player again, once it has stood alone (no session, matching idle) for ReturnAfterMs: a
// join makes it a phantom first, while the session is not yet the game's (rig 2026-10-14: a guest
// made a player too early could not be revived).
class PhantomHome {
public:
    static constexpr s64 ReturnAfterMs = 5'000;
    bool Due(bool phantom, bool alone, s64 now_ms) {
        if (!phantom || !alone) {
            since_ms.reset();
            return false;
        }
        if (!since_ms) {
            since_ms = now_ms;
        }
        if (now_ms - *since_ms < ReturnAfterMs) {
            return false;
        }
        since_ms.reset();
        return true;
    }

private:
    std::optional<s64> since_ms;
};

// Kaniel 2026-10-15: a cutscene that plays under the team's loading screen (a new character's
// opening) is skipped by itself: Options, the game's own skip, pressed for the player in short
// pulses while the cutscene player (SprjRemoMan) is in its playing step. `step` is that player's
// step (-1 none), entered at `step_ms`; true while Options is to be held down. The pad asks it
// as the game reads the pad (the game's own tick may stand still under a cutscene).
class CutsceneSkip {
public:
    // SprjRemoMan::STEP_PrologueWait and STEP_Update: the rig's opening spent ~100 s in the
    // first, the second only its last moment.
    static constexpr int PrologueStep = 3;
    static constexpr int PlayStep = 4;
    static constexpr s64 HoldMs = 120;        // a press
    static constexpr s64 EveryMs = 700;       // between presses
    static constexpr s64 LongestMs = 300'000; // in the step this long: stuck, left alone
    static constexpr int MostPresses = 200;   // about two minutes of presses: then left alone
    bool Press(bool covered, int step, s64 step_ms, s64 now_ms);
    int Presses() const {
        return presses;
    }

private:
    std::optional<s64> pressed_ms;
    int presses = 0;
};

// Kaniel 2026-10-17: a cutscene that the party's events play for everyone on the map (a boss's
// entrance, the Ward door's evening, Amelia's skull) was skipped by whoever pressed Options first,
// each for itself. Now Options is a vote while this game's cutscene plays in a party session: the
// game's skip is held back until every member on this map whose cutscene plays voted (others whose
// cutscene ended, or who left, no longer count), then pressed for the player as CutsceneSkip does.
// The others' votes come with their vitals (bbcoop::Vitals.cutscene).
class CutsceneVote {
public:
    enum : u32 { NotWatching = 0, Watching = 1, WantsSkip = 2 };
    static constexpr s64 HoldMs = CutsceneSkip::HoldMs;
    static constexpr s64 EveryMs = CutsceneSkip::EveryMs;
    struct Frame {
        bool watching = false;   // this game's cutscene plays, in a party session
        bool options = false;    // the player holds Options
        u32 others_watching = 0; // the other members on this map whose cutscene plays
        u32 others_skip = 0;     // of them, the ones that voted
        s64 now_ms = 0;
    };
    // What the pad does with Options: the player's own, held back, or pressed for the skip.
    enum class Pad { Leave, Hide, Press };
    Pad Tick(const Frame& frame);
    // This member's state, for the others.
    u32 Own() const {
        return !watching ? NotWatching : voted ? WantsSkip : Watching;
    }
    // "Want() of Watchers() want to skip", shown while others watch too and someone voted.
    bool Shown() const {
        return watching && watchers > 1 && want > 0;
    }
    u32 Want() const {
        return want;
    }
    u32 Watchers() const {
        return watchers;
    }

private:
    bool watching = false;
    bool voted = false;
    u32 want = 0;
    u32 watchers = 0;
    std::optional<s64> pressed_ms;
};

// Kaniel 2026-10-14: the team loading screen, as in League of Legends. From the team's Start every
// member's game shows each hunter with a bar (bbcoop::LoadProgress through the server) and holds
// until all of them are in the team's session (or LimitMs), so the hunt opens for everyone at once.
// Kaniel 2026-10-16: ready (OwnStage) only once it has held for SettleMs: the others' characters
// stand in the world, not just started to appear. Nobody else coming: at once. And never while a
// cutscene plays (krolebord's opening started after the screen lifted, so nothing skipped it: his
// game stood still two minutes, his copy flew off on the others' screens and he missed the door
// the host opened): the screen stays and the cutscene is skipped under it.
class ReadySettle {
public:
    static constexpr s64 SettleMs = 5'000;
    bbcoop::LoadProgress::Stage Settle(bbcoop::LoadProgress::Stage stage, size_t others_needed,
                                       s64 now_ms, bool cutscene = false) {
        if (stage == bbcoop::LoadProgress::READY && cutscene) {
            since.reset();
            return bbcoop::LoadProgress::JOINING;
        }
        if (others_needed == 0) {
            since.reset();
            return stage;
        }
        if (stage != bbcoop::LoadProgress::READY) {
            // A character missing for a moment (a copy's flicker) does not start it over.
            if (since && now_ms - last_ready_ms > FlickerMs) {
                since.reset();
            }
            return stage;
        }
        last_ready_ms = now_ms;
        if (!since) {
            since = now_ms;
        }
        return now_ms - *since >= SettleMs ? stage : bbcoop::LoadProgress::JOINING;
    }
    static constexpr s64 FlickerMs = 1'000;

private:
    std::optional<s64> since;
    s64 last_ready_ms = 0;
};

class TeamLoad {
public:
    using Stage = bbcoop::LoadProgress::Stage;
    static constexpr s64 LimitMs = 90'000;
    // A save's load fills its bar to LoadedPercent over about this long (the game says nothing of
    // its own progress); joining and ready take it on to 100.
    static constexpr s64 LoadingGuessMs = 20'000;
    static constexpr u32 LoadedPercent = 70;
    struct Row {
        std::string member;
        std::string name;
        Stage stage = bbcoop::LoadProgress::WAITING;
        u32 percent = 0;
        bool self = false;
    };
    struct View {
        bool up = false;
        std::vector<Row> rows;
    };

    // This game's stage: loading its world, then joining, and ready only once it is in the
    // session and `others_seen` of the `others_needed` other hunters' characters stand in its
    // world (Kaniel 2026-10-15: the screen stays until every guest's character has spawned).
    static Stage OwnStage(bool loading, bool in_session, size_t others_seen, size_t others_needed);
    // The other members this game waits to see: `members` but this one (`me`) and those the map
    // board (`maps`: member, map) has on another map than `map` (0: loading, still waited for).
    static size_t OthersHere(std::span<const std::pair<std::string, std::string>> members,
                             std::string_view me, std::span<const std::pair<std::string, u32>> maps,
                             u32 map);
    void Start(s64 now_ms);
    void SetStage(Stage stage, s64 now_ms);
    void Take(const bbcoop::LoadProgress& other, s64 now_ms);
    // `members`: the team's (member, name), the leader first.
    View Tick(std::span<const std::pair<std::string, std::string>> members, const std::string& me,
              s64 now_ms);
    // This game's progress to send, when it changed.
    std::optional<bbcoop::LoadProgress> ToSend(s64 now_ms);

private:
    struct Heard {
        bbcoop::LoadProgress load;
        s64 at_ms = 0;
    };
    u32 OwnPercent(s64 now_ms) const;
    std::optional<s64> started_ms;
    Stage own = bbcoop::LoadProgress::WAITING;
    s64 own_since_ms = 0;
    bool open = false;
    std::optional<std::pair<Stage, u32>> sent;
    std::vector<std::pair<std::string, Heard>> others;
};

} // namespace Core::Bloodborne
