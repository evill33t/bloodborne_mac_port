// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Kaniel / lKaniel's bbport_settings.{h,cpp}, pinned 8d5e54f.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace BbLanguage {
enum class MenuLanguage { English, Russian,
#define BB_MENU_LANG(name, ...) name,
#include "bbport_menu_texts.inc"
#undef BB_MENU_LANG
};
inline constexpr int MenuLanguageCount=int(MenuLanguage::Tr)+1;
struct LanguageInfo {
    const char* code;
    const char* name;
    std::vector<int> ps4_ids;
};

inline const std::vector<LanguageInfo>& Languages() {
    static const std::vector<LanguageInfo> languages = {
        {"en", "English", {1, 18}},
        {"ru", "Русский", {8}},
#define BB_MENU_LANG(name, code, own_name, ...) {code, own_name, {__VA_ARGS__}},
#include "bbport_menu_texts.inc"
#undef BB_MENU_LANG
    };
    return languages;
}

constexpr int TableLanguages = MenuLanguageCount - 2;
struct TableText {
    const char* english;
    const char* texts[TableLanguages];
};
constexpr TableText table_texts[] = {
#define BB_MENU_TEXT(english, ...) {english, __VA_ARGS__},
#include "bbport_menu_texts.inc"
#undef BB_MENU_TEXT
};

inline const char* Translate(MenuLanguage language, const char* english, const char* russian) {
    if (language == MenuLanguage::English) {
        return english;
    }
    if (language == MenuLanguage::Russian) {
        static const std::unordered_map<std::string_view,const char*> ru={
#define BB_COOP_RUSSIAN(en, ru) {en, ru},
#include "bbport_coop_russian.inc"
#undef BB_COOP_RUSSIAN
        };
        const auto found=ru.find(english);
        return found==ru.end() ? (russian?russian:english) : found->second;
    }
    static const std::unordered_map<std::string_view, const TableText*> by_english = [] {
        std::unordered_map<std::string_view, const TableText*> map;
        for (const TableText& text : table_texts) {
            map.emplace(text.english, &text);
        }
        return map;
    }();
    const auto found = by_english.find(english);
    const int column = int(language) - 2;
    if (found == by_english.end() || column < 0 || column >= TableLanguages) {
        return english;
    }
    const char* text = found->second->texts[column];
    return text ? text : english;
}

inline MenuLanguage LanguageFromPs4(int id) {
    const auto& languages = Languages();
    for (std::size_t l = 0; l < languages.size(); ++l) {
        if (std::ranges::find(languages[l].ps4_ids, id) != languages[l].ps4_ids.end()) {
            return MenuLanguage(l);
        }
    }
    return MenuLanguage::English;
}

inline MenuLanguage GameLanguage() {
    // runtime_services.c answers the game's language query from the same variable.
    const char* id = std::getenv("BB_LANGUAGE");
    return LanguageFromPs4(id && *id ? std::atoi(id) : 1);
}

inline const char* LanguageCode(MenuLanguage language) {
    return Languages()[std::size_t(language)].code;
}

inline const char* LanguageName(MenuLanguage language) {
    return Languages()[std::size_t(language)].name;
}

inline std::optional<MenuLanguage> LanguageFromCode(std::string_view code) {
    const auto& languages = Languages();
    for (std::size_t l = 0; l < languages.size(); ++l) {
        if (code == languages[l].code) {
            return MenuLanguage(l);
        }
    }
    return std::nullopt;
}


// Bloodborne's message/font folders (the menu locale and game language use the same IDs).
inline const char* GameFolder(MenuLanguage language) {
    constexpr const char* folders[]={"engus","rusru","jpnjp","frafr","spaes","spaar","deude","itait","engus","porpt","korkr","zhotw","zhocn","engus","engus","engus","engus","polpl","porbr","engus"};
    return folders[std::clamp(int(language),0,MenuLanguageCount-1)];
}
inline const char* Text(MenuLanguage language,const char* english) {
    if(language==MenuLanguage::English) return english;
    struct NativeText {const char* english;const char* translated[MenuLanguageCount-1];};
    static constexpr NativeText native[]={
#define BB_COOP_TEXT(en, ...) {en,__VA_ARGS__},
#include "bbport_coop_texts.inc"
#undef BB_COOP_TEXT
    };
    for(const auto& row:native) if(std::string_view(english)==row.english) return row.translated[int(language)-1];
    static const std::unordered_map<std::string_view,const char*> aliases={
{"Connecting to the co-op server...","Reaching the co-op server..."},
{"Create team","Create a team"},
{"Downed","Down"},
{"End duel","End the duel"},
{"Entering the hunt","Joining the hunt"},
{"Fog of war","Map fog of war"},
{"Give to","Give"},
{"Leader of the hunt","Leader, hosts the hunt"},
{"Leave invasion","Leave the invasion"},
{"Leave team","Leave the team"},
{"New team code","New code"},
{"No teammate nearby","No teammate stands here"},
{"Play alone","Alone"},
{"Search for an invasion","Invade a hunt"},
{"Searching for a hunt to invade...","Looking for a world to invade"},
{"Start hunt","Begin the hunt"},
{"The invasion ended","The invasion is over"},
{"The server refused the invasion request","The server refused the invasion"},
{"Travel to lamp?","Travel to this lamp?"},
{"Travel with team","Travel with the team?"},
{"World map","Map"},
{"World Map","Map"},
{"All areas","Whole map"},
{"3D view","3D map"},
{"Live 3D","Live"},
{"In use","Another team is hunting here now"},
{"Gather your hunters","Accept an invitation to join a team's hunt, or create a team."},
{"Game language","Automatic"},
{"The hunt","The Hunt Together"},
{"Online display","Display"},
{"Trade items","Give"},
{"Trade with teammate","Give"},
{"Offer...","Give"},
{"Send offer","Give"},
{"Decline duel","Decline"},
{"Accept duel","Join"},
{"Your location","Centre"},
{"Your hunter","Hunter"},
{"Companion","Hunter"},
{"Standing","Ready"},
{"Choosing a character","No hunter yet"},
{"Connected","Ready"},
{"Down and revive","Go down (revive)"},
{"View","Map"},
{"Level","Floors"},
{"Return to game","Close"},
{"Return to title","Back"},
{"Close map","Back"},
{"An adversary arrives in %d seconds","An invader approaches: %d"},
{"Invading a hunt in %d seconds","Invading in %d"},
{"No hunt found for an invasion","No world to invade was found"},
{"The host was defeated","The invader prevailed"},
{"The hunt was defeated","The invader prevailed"},
{"Your companions will join you when the hunt is ready","The hunt opens for everyone once all hunters are ready"},
{"Every hunt in a world shares its progress. Your character keeps its own equipment.","Each world keeps its own progress on the server; any team may hunt in it."},
{"Hold Circle / Left Shift to wake at your lamp","hold to wake at your lamp"},
{"L1 / R1: next companion","Next teammate"},
{"Skip cutscene: %d / %d votes","%d of %d want to skip"},
{"Free camera  -  F7 to return","or F7: back to your hunter"},
{"Team duel invitation","Duel"},
{"Waiting for hunters","The hunt opens for everyone once all hunters are ready"},
{"Check the server address and connection if this persists. You can return to offline play from the launcher.","Reaching the co-op server..."},
{"on","On"},
{"off","Off"},
};
    auto found=aliases.find(english);return Translate(language,found==aliases.end()?english:found->second,nullptr);
}
}
