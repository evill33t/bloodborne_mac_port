// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/coop_types.h"

// Cheat watch (Kaniel 2026-11-04: "track if any people are cheating or using any save edit in the
// server multiplayer"): shadNet looks at what each game says of its character (CharacterSheet:
// level, attributes, echoes, insight, HP, and how the game was built and started), its vitals and
// its trades, and notes what no unmodified game can show: values past the game's limits, a level
// its attributes do not add up to, attributes that went down, a level up that cost nothing, echoes
// or insight out of nowhere, cheats or the save editor on. It only reports (the server's log and
// suspects.jsonl, the admin's `suspects`); nobody is refused or banned for it.
//
// Within one run of a game (a restart, a new character or a hello again starts over) changes are
// judged against the last sheet; between runs nothing is (players play offline too).
namespace BloodborneCoop {

// How sure a finding is.
enum class Suspicion : u8 {
    Info,    // worth knowing (a dev build, a rig script), not cheating by itself
    Likely,  // an unmodified game should not, though timing or an unknown rule might
    Certain, // no unmodified game can
};

inline std::string_view SuspicionName(Suspicion level) {
    switch (level) {
    case Suspicion::Info:
        return "info";
    case Suspicion::Likely:
        return "likely";
    case Suspicion::Certain:
        return "certain";
    }
    return "?";
}

struct CheatFinding {
    std::string kind; // a short key, one finding of a kind per character and run
    Suspicion level = Suspicion::Info;
    std::string text; // for the log: what was seen
    bool operator==(const CheatFinding&) const = default;
};

// The game's limits (1.09).
constexpr s32 AttributeMin = 1;
constexpr s32 AttributeMax = 99;
constexpr s32 LevelMax = 544; // all six attributes at 99
// Every origin's level is its attributes' sum less 50 (Milquetoast 60 -> 10, Waste of Skin 54 ->
// 4), and each level up adds one point to one attribute.
constexpr s32 LevelBelowAttributes = 50;
constexpr s64 EchoesMax = 999'999'999;
constexpr s32 InsightMax = 99;
constexpr u32 VialsMax = 20;
constexpr u32 BulletsMax = 25; // 20, and the Formless Oedon rune's +5
// Beyond any Vitality 99 character with every HP rune and gem.
constexpr s32 HpMaxLimit = 5000;
// More echoes at once than any kill, item or boss of NG+7 gives (trades, and echoes the character
// had before, such as its bloodstain's, are allowed for).
constexpr s64 EchoesJump = 2'000'000;
// Insight: a boss gives 3, an item 1; more at once than this is not play.
constexpr s32 InsightJump = 10;

constexpr std::array<std::string_view, 6> AttributeNames{"Vitality", "Endurance",  "Strength",
                                                         "Skill",    "Bloodtinge", "Arcane"};

// What a game says of its character (bbcoop::CharacterSheet).
struct CharacterSheet {
    std::string character; // its name
    s32 level = 0;
    std::array<s32, 6> attributes{};
    s64 echoes = 0;
    s32 insight = 0;
    s32 hp = 0;
    s32 hp_max = 0;
    // How the game was built and started.
    bool production = false; // a release build (no cheats, no save editor, no debug tools)
    std::string cheats;      // the cheat patches it started with ("" or "none": none)
    std::string dev_tools;   // debug menu, debug camera: what it started with ("": none)
    u32 save_edits = 0;      // changes the save editor made this run
    bool autoplay = false;   // the rig's script drives it
};

inline s32 AttributeSum(const CharacterSheet& sheet) {
    s32 sum = 0;
    for (const s32 value : sheet.attributes) {
        sum += value;
    }
    return sum;
}

// One game's run, as shadNet hears it.
class CheatWatch {
public:
    // A new run, a new character or a hello again: nothing before counts any more, and what is
    // found is said again.
    void StartOver() {
        last.reset();
        echoes_peak = 0;
        insight_peak = 0;
        granted = 0;
        said.clear();
    }

    // Echoes a trade hands the character (a grant on its way): a rise by as much is no jump.
    void EchoesGranted(s64 echoes) {
        granted += std::max<s64>(echoes, 0);
    }

    std::vector<CheatFinding> Sheet(const CharacterSheet& now) {
        std::vector<CheatFinding> found;
        // Another character (or save) in the same run. A name read late (none in the first
        // sheets) is the same character.
        if (last && !last->character.empty() && last->character != now.character) {
            StartOver();
        }
        Build(now, found);
        if (now.level == 0) {
            return Keep(std::move(found)); // no character in the world (the title, a load)
        }
        Limits(now, found);
        if (last) {
            Changes(*last, now, found);
        }
        echoes_peak = std::max(echoes_peak, now.echoes);
        insight_peak = std::max(insight_peak, now.insight);
        last = now;
        return Keep(std::move(found));
    }

    // The member's vitals (bbcoop::Vitals): what it carries.
    std::vector<CheatFinding> Vitals(u32 vials, u32 bullets, s32 hp_max, u32 level) {
        std::vector<CheatFinding> found;
        if (vials > VialsMax) {
            found.push_back({"vials", Suspicion::Likely,
                             "carries " + std::to_string(vials) + " Blood Vials (at most " +
                                 std::to_string(VialsMax) + ")"});
        }
        if (bullets > BulletsMax) {
            found.push_back({"bullets", Suspicion::Likely,
                             "carries " + std::to_string(bullets) +
                                 " Quicksilver Bullets (at most " + std::to_string(BulletsMax) +
                                 ")"});
        }
        if (hp_max > HpMaxLimit) {
            found.push_back({"hp-max", Suspicion::Likely,
                             "max HP " + std::to_string(hp_max) + " (no character passes " +
                                 std::to_string(HpMaxLimit) + ")"});
        }
        if (level > static_cast<u32>(LevelMax)) {
            found.push_back(
                {"level-range", Suspicion::Certain,
                 "level " + std::to_string(level) + " (at most " + std::to_string(LevelMax) + ")"});
        }
        return Keep(std::move(found));
    }

    // An offer of echoes (the giver's game took them out first, so the last sheet still holds
    // them, or the next one shows them gone).
    std::vector<CheatFinding> EchoesOffered(s64 echoes) {
        std::vector<CheatFinding> found;
        if (echoes > EchoesMax) {
            found.push_back({"trade-echoes", Suspicion::Certain,
                             "offered " + std::to_string(echoes) + " echoes (at most " +
                                 std::to_string(EchoesMax) + ")"});
        } else if (last && echoes > std::max(last->echoes, echoes_peak) + granted + EchoesJump) {
            found.push_back({"trade-echoes", Suspicion::Likely,
                             "offered " + std::to_string(echoes) + " echoes, its character had " +
                                 std::to_string(last->echoes)});
        }
        return Keep(std::move(found));
    }

    // An offer's item count past what one stack holds.
    std::vector<CheatFinding> ItemsOffered(u32 count) {
        std::vector<CheatFinding> found;
        if (count > 999) {
            found.push_back({"trade-count", Suspicion::Certain,
                             "offered " + std::to_string(count) + " of one item"});
        }
        return Keep(std::move(found));
    }

    const std::optional<CharacterSheet>& Last() const {
        return last;
    }

private:
    static void Build(const CharacterSheet& now, std::vector<CheatFinding>& found) {
        const bool cheats = !now.cheats.empty() && now.cheats != "none";
        if (cheats) {
            found.push_back({"cheats", Suspicion::Certain,
                             std::string{now.production ? "a release build with cheat patches "
                                                        : "cheats on: "} +
                                 now.cheats});
        }
        if (now.save_edits > 0) {
            found.push_back(
                {"save-editor", Suspicion::Certain,
                 "used the save editor (" + std::to_string(now.save_edits) + " changes this run)"});
        }
        if (!now.dev_tools.empty()) {
            found.push_back({"dev-tools", now.production ? Suspicion::Certain : Suspicion::Likely,
                             std::string{now.production ? "a release build with " : ""} +
                                 now.dev_tools + " on"});
        }
        if (!now.production) {
            found.push_back({"dev-build", Suspicion::Info,
                             "a development build (cheats and the save editor are in it)"});
        }
        if (now.autoplay) {
            found.push_back({"autoplay", Suspicion::Info, "a rig script drives it"});
        }
    }

    static void Limits(const CharacterSheet& now, std::vector<CheatFinding>& found) {
        for (std::size_t i = 0; i < now.attributes.size(); ++i) {
            const s32 value = now.attributes[i];
            if (value < AttributeMin || value > AttributeMax) {
                found.push_back(
                    {"attribute-range", Suspicion::Certain,
                     std::string{AttributeNames[i]} + " " + std::to_string(value) + " (1 to 99)"});
                break;
            }
        }
        if (now.level < 1 || now.level > LevelMax) {
            found.push_back({"level-range", Suspicion::Certain,
                             "level " + std::to_string(now.level) + " (1 to " +
                                 std::to_string(LevelMax) + ")"});
        }
        const s32 sum = AttributeSum(now);
        if (now.level != sum - LevelBelowAttributes) {
            found.push_back({"level-mismatch", Suspicion::Certain,
                             "level " + std::to_string(now.level) +
                                 ", but its attributes make level " +
                                 std::to_string(sum - LevelBelowAttributes)});
        }
        if (now.echoes < 0 || now.echoes > EchoesMax) {
            found.push_back({"echoes-range", Suspicion::Certain,
                             std::to_string(now.echoes) + " echoes (at most " +
                                 std::to_string(EchoesMax) + ")"});
        }
        if (now.insight < 0 || now.insight > InsightMax) {
            found.push_back({"insight-range", Suspicion::Certain,
                             std::to_string(now.insight) + " insight (at most " +
                                 std::to_string(InsightMax) + ")"});
        }
        if (now.hp_max > HpMaxLimit) {
            found.push_back({"hp-max", Suspicion::Likely,
                             "max HP " + std::to_string(now.hp_max) + " (no character passes " +
                                 std::to_string(HpMaxLimit) + ")"});
        }
    }

    void Changes(const CharacterSheet& before, const CharacterSheet& now,
                 std::vector<CheatFinding>& found) {
        for (std::size_t i = 0; i < now.attributes.size(); ++i) {
            if (now.attributes[i] < before.attributes[i]) {
                found.push_back({"attribute-down", Suspicion::Certain,
                                 std::string{AttributeNames[i]} + " went down from " +
                                     std::to_string(before.attributes[i]) + " to " +
                                     std::to_string(now.attributes[i])});
                break;
            }
        }
        const s32 levels = now.level - before.level;
        const s32 points = AttributeSum(now) - AttributeSum(before);
        if (levels != points) {
            found.push_back({"level-apart", Suspicion::Certain,
                             "level " + std::to_string(before.level) + " -> " +
                                 std::to_string(now.level) + " while its attributes rose by " +
                                 std::to_string(points)});
        } else if (levels > 0 && now.echoes >= before.echoes) {
            found.push_back({"free-level", Suspicion::Likely,
                             "levelled up " + std::to_string(before.level) + " -> " +
                                 std::to_string(now.level) + " without spending echoes (" +
                                 std::to_string(before.echoes) + " -> " +
                                 std::to_string(now.echoes) + ")"});
        }
        // Echoes it had before (a bloodstain picked up) and trades' echoes are no jump.
        const s64 known = std::max(before.echoes, echoes_peak) + granted;
        if (now.echoes > known + EchoesJump) {
            found.push_back({"echoes-jump", Suspicion::Likely,
                             "echoes " + std::to_string(before.echoes) + " -> " +
                                 std::to_string(now.echoes) + " at once"});
        }
        if (now.echoes > before.echoes) {
            granted = std::max<s64>(0, granted - (now.echoes - before.echoes));
        }
        if (now.insight > std::max(before.insight, insight_peak) + InsightJump) {
            found.push_back({"insight-jump", Suspicion::Likely,
                             "insight " + std::to_string(before.insight) + " -> " +
                                 std::to_string(now.insight) + " at once"});
        }
    }

    // Each kind once per run (the log says it once; the store counts the run).
    std::vector<CheatFinding> Keep(std::vector<CheatFinding> found) {
        std::erase_if(
            found, [&](const CheatFinding& finding) { return !said.insert(finding.kind).second; });
        return found;
    }

    std::optional<CharacterSheet> last;
    s64 echoes_peak = 0;
    s32 insight_peak = 0;
    s64 granted = 0;
    std::set<std::string> said;
};

} // namespace BloodborneCoop
