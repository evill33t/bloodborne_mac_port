// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The cheat watch (coop/cheat_watch.h): what an unmodified game never shows is
// found, once per run, and what play does (level ups that cost echoes, a
// bloodstain picked up, a trade's echoes) is not.
#include <iostream>
#include <string>

#include "coop/cheat_watch.h"

using namespace BloodborneCoop;

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

// A Milquetoast at level 10 (its attributes add up to 60), a release build.
CharacterSheet Hunter() {
  CharacterSheet sheet;
  sheet.character = "Gehrman's pupil";
  sheet.level = 10;
  sheet.attributes = {11, 10, 12, 10, 9, 8};
  sheet.echoes = 5000;
  sheet.insight = 2;
  sheet.hp = 600;
  sheet.hp_max = 600;
  sheet.production = true;
  sheet.cheats = "none";
  return sheet;
}

bool Has(const std::vector<CheatFinding> &found, std::string_view kind) {
  for (const CheatFinding &finding : found) {
    if (finding.kind == kind) {
      return true;
    }
  }
  return false;
}

Suspicion LevelOf(const std::vector<CheatFinding> &found,
                  std::string_view kind) {
  for (const CheatFinding &finding : found) {
    if (finding.kind == kind) {
      return finding.level;
    }
  }
  return Suspicion::Info;
}

} // namespace

int main() {
  {
    // A clean character of a release build: nothing, and nothing as it plays.
    CheatWatch watch;
    CharacterSheet sheet = Hunter();
    CHECK(watch.Sheet(sheet).empty());
    // Waste of Skin: level 4, attributes 54.
    CharacterSheet waste = Hunter();
    waste.character = "Waste";
    waste.level = 4;
    waste.attributes = {10, 9, 10, 9, 7, 9};
    CHECK(CheatWatch{}.Sheet(waste).empty());
    // A level up at the Doll: one point, echoes spent.
    sheet.level = 11;
    sheet.attributes[0] = 12;
    sheet.echoes = 4000;
    CHECK(watch.Sheet(sheet).empty());
    // A death drops the echoes; the bloodstain gives them back: no jump.
    sheet.echoes = 0;
    CHECK(watch.Sheet(sheet).empty());
    sheet.echoes = 4000;
    CHECK(watch.Sheet(sheet).empty());
    // Kill echoes, insight from a boss.
    sheet.echoes = 90'000;
    sheet.insight = 5;
    CHECK(watch.Sheet(sheet).empty());
    // A title screen's sheet (no character): nothing.
    CharacterSheet title = Hunter();
    title.level = 0;
    title.attributes = {};
    CHECK(CheatWatch{}.Sheet(title).empty());
    // Plausible vitals and offers.
    CHECK(watch.Vitals(20, 25, 1800, 120).empty());
    CHECK(watch.EchoesOffered(90'000).empty());
    CHECK(watch.ItemsOffered(99).empty());
  }
  {
    // Edited saves: values past the game's limits, a level its attributes do
    // not make.
    CharacterSheet sheet = Hunter();
    sheet.attributes[2] = 150;
    auto found = CheatWatch{}.Sheet(sheet);
    CHECK(Has(found, "attribute-range"));
    CHECK(LevelOf(found, "attribute-range") == Suspicion::Certain);
    CHECK(Has(found, "level-mismatch"));

    sheet = Hunter();
    sheet.level = 5; // attributes say 10
    found = CheatWatch{}.Sheet(sheet);
    CHECK(Has(found, "level-mismatch"));
    CHECK(found.size() == 1);
    CHECK(found[0].text == "level 5, but its attributes make level 10");

    sheet = Hunter();
    sheet.echoes = 2'000'000'000;
    sheet.insight = 120;
    sheet.hp_max = 9999;
    found = CheatWatch{}.Sheet(sheet);
    CHECK(Has(found, "echoes-range"));
    CHECK(Has(found, "insight-range"));
    CHECK(Has(found, "hp-max"));
    CHECK(LevelOf(found, "hp-max") == Suspicion::Likely);
  }
  {
    // The save editor in a run: attributes raised with the level (it keeps them
    // together) but no echoes paid; then a stat lowered; echoes and insight out
    // of nowhere.
    CheatWatch watch;
    CharacterSheet sheet = Hunter();
    CHECK(watch.Sheet(sheet).empty());
    sheet.level = 20;
    sheet.attributes[1] = 20;
    auto found = watch.Sheet(sheet);
    CHECK(Has(found, "free-level"));
    CHECK(LevelOf(found, "free-level") == Suspicion::Likely);
    sheet.attributes[1] = 15;
    sheet.level = 15;
    found = watch.Sheet(sheet);
    CHECK(Has(found, "attribute-down"));
    CHECK(found[0].text == "Endurance went down from 20 to 15");
    sheet.echoes += 50'000'000;
    sheet.insight += 40;
    found = watch.Sheet(sheet);
    CHECK(Has(found, "echoes-jump"));
    CHECK(Has(found, "insight-jump"));
    // Each kind once per run.
    sheet.echoes += 50'000'000;
    CHECK(!Has(watch.Sheet(sheet), "echoes-jump"));
    // A new run says it again.
    watch.StartOver();
    CHECK(watch.Sheet(Hunter()).empty());
    CharacterSheet again = Hunter();
    again.echoes += 50'000'000;
    CHECK(Has(watch.Sheet(again), "echoes-jump"));
  }
  {
    // The level alone, its attributes not (a poke of +0x90): apart.
    CheatWatch watch;
    CharacterSheet sheet = Hunter();
    watch.Sheet(sheet);
    sheet.level = 11;
    sheet.echoes = 0;
    const auto found = watch.Sheet(sheet);
    CHECK(Has(found, "level-apart"));
    CHECK(Has(found, "level-mismatch"));
  }
  {
    // A trade's echoes are no jump; the same rise without one is.
    CheatWatch watch;
    CharacterSheet sheet = Hunter();
    watch.Sheet(sheet);
    watch.EchoesGranted(3'000'000);
    sheet.echoes += 3'000'000;
    CHECK(watch.Sheet(sheet).empty());
    sheet.echoes += 3'000'000;
    CHECK(Has(watch.Sheet(sheet), "echoes-jump"));
  }
  {
    // Another character in the same run: judged on its own.
    CheatWatch watch;
    watch.Sheet(Hunter());
    CharacterSheet other = Hunter();
    other.character = "Second";
    other.level = 50;
    other.attributes = {25, 20, 15, 15, 9, 16};
    other.echoes = 100;
    CHECK(watch.Sheet(other).empty());
  }
  {
    // Its name read late (none in the first sheets): the same character, said
    // once.
    CheatWatch watch;
    CharacterSheet sheet = Hunter();
    sheet.production = false;
    sheet.character = "";
    CHECK(Has(watch.Sheet(sheet), "dev-build"));
    sheet.character = "Hunter";
    CHECK(watch.Sheet(sheet).empty());
  }
  {
    // How the game was built: cheats, the save editor, debug tools, a dev
    // build, the rig.
    CharacterSheet sheet = Hunter();
    sheet.cheats = "no death, stealth";
    auto found = CheatWatch{}.Sheet(sheet);
    CHECK(Has(found, "cheats"));
    CHECK(LevelOf(found, "cheats") == Suspicion::Certain);
    CHECK(found[0].text ==
          "a release build with cheat patches no death, stealth");

    sheet = Hunter();
    sheet.production = false;
    sheet.save_edits = 3;
    sheet.dev_tools = "debug menu";
    sheet.autoplay = true;
    found = CheatWatch{}.Sheet(sheet);
    CHECK(Has(found, "save-editor"));
    CHECK(LevelOf(found, "dev-tools") == Suspicion::Likely);
    CHECK(LevelOf(found, "dev-build") == Suspicion::Info);
    CHECK(Has(found, "autoplay"));

    sheet = Hunter();
    sheet.dev_tools = "debug camera";
    CHECK(LevelOf(CheatWatch{}.Sheet(sheet), "dev-tools") ==
          Suspicion::Certain);
  }
  {
    // Vitals and trades past the limits.
    CheatWatch watch;
    auto found = watch.Vitals(99, 99, 20000, 900);
    CHECK(Has(found, "vials"));
    CHECK(Has(found, "bullets"));
    CHECK(Has(found, "hp-max"));
    CHECK(Has(found, "level-range"));
    CHECK(Has(watch.EchoesOffered(5'000'000'000), "trade-echoes"));
    CHECK(Has(watch.ItemsOffered(5000), "trade-count"));
    CheatWatch poor;
    poor.Sheet(Hunter());
    CHECK(LevelOf(poor.EchoesOffered(500'000'000), "trade-echoes") ==
          Suspicion::Likely);
  }
  std::cout << "cheat watch: ok\n";
  return 0;
}
