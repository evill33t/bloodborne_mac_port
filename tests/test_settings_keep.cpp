// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu's Save() keeps the lines it does not own (the launcher's fps_cap, fullscreen).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/// Lines equal to `line`.
static int Count(const std::string& text, const std::string& line) {
    int n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        n += l == line;
    }
    return n;
}

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-settings-keep-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    unsetenv("BB_METAL_FRAMEGEN");
    unsetenv("BB_SCREEN_LIGHTING");
    unsetenv("BB_SCREEN_LIGHTING_AO");
    unsetenv("BB_SCREEN_LIGHTING_GI");
    FILE* f = std::fopen(path, "w");
    assert(f);
    std::fprintf(f, "# a comment\nupscaler=fsr3\nfps_cap=40\ngame_timing=adaptive\nfullscreen=1\nrenderer=metal\npreset=2\n"
                    "mouse_look=1\nmouse_sensitivity=2.50\nmouse_invert_y=1\nhdr=1\nhdr_highlights=0.75\nhdr_paper_white=1.25\nhdr_peak=4\nscreen_lighting=low\nscreen_lighting_ao=0.6\nscreen_lighting_gi=1.2\n");
    std::fclose(f);
    Load();
    assert(!Get().framegen && !Get().startup_framegen && !Get().framegen_forced);
    assert(!FrameGenerationNeedsRestart());
    assert(Get().upscaler == UpscalerFsr3 && Get().preset == 2);
    assert(Get().mouse_look && Get().mouse_sensitivity == 2.5f && Get().mouse_invert_y);
    assert(Get().achievement_banners);
    assert(Get().pad_menu_combo);
    Get().achievement_banners = false;
    Get().pad_menu_combo = false;
    Get().mouse_look = true;
    Get().mouse_sensitivity = 2.25f;
    Get().mouse_vertical = 0.75f;
    Get().mouse_invert_y = true;
    assert(Get().hdr && Get().startup_hdr && Get().hdr_paper_white == 1.25f && Get().hdr_peak == 4.0f);
    Get().preset = NativeAA;
    assert(NativeHdrSupported());
    Get().preset = 1;
    assert(!NativeHdrSupported());
    Get().preset = NativeAA;
    setenv("BB_RENDER_RES", "1920x1080", 1);
    assert(!NativeHdrSupported());
    unsetenv("BB_RENDER_RES");
    Get().hdr = false; Get().hdr_paper_white = 0.75f; Get().hdr_peak = 0.0f;
    assert(Get().screen_lighting == ScreenLightingLow && Get().screen_lighting_ao == 0.6f &&
           Get().screen_lighting_gi == 1.2f);
    Get().preset = 3;
    Get().map_view = 3;Get().compass=false;Get().compass_position=2;
    Get().bloodstains=false;Get().team_pings=false;Get().ping_key=3;Get().free_camera=true;
    Get().menu_language_auto=false;Get().menu_language=MenuLanguage::De;
    Save();
    std::string text = Read(path);
    assert(Count(text, "hdr_highlights=0.75") == 0);
    assert(Get().startup_hdr); // menu edits do not change the running layer
    assert(Count(text, "achievement_banners=0") == 1);
    assert(Count(text, "pad_menu_combo=0") == 1);
    assert(Count(text, "mouse_look=1") == 1);
    assert(Count(text, "mouse_sensitivity=2.25") == 1);
    assert(Count(text, "mouse_vertical=0.75") == 1);
    assert(Count(text, "mouse_invert_y=1") == 1);
    Get().achievement_banners = true;
    Get().pad_menu_combo = true;
    Get().mouse_look = false;
    Get().mouse_sensitivity = 1.0f;
    Get().mouse_vertical = 1.0f;
    Get().mouse_invert_y = false;
    Load();
    assert(!Get().achievement_banners);
    assert(!Get().pad_menu_combo);
    assert(Get().mouse_look && Get().mouse_sensitivity == 2.25f && Get().mouse_invert_y);
    assert(Get().mouse_vertical == 0.75f);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "game_timing=adaptive") == 1);
    assert(Count(text, "mouse_look=1") == 1 && Count(text, "mouse_sensitivity=2.25") == 1);
    assert(Count(text, "mouse_invert_y=1") == 1);
    assert(Count(text, "hdr=0") == 1 && Count(text, "hdr_paper_white=0.75") == 1 && Count(text, "hdr_peak=0.00") == 1);
    assert(Count(text, "renderer=") == 0); // the removed renderer setting is dropped
    assert(Count(text, "preset=3") == 1 && Count(text, "preset=2") == 0);
    assert(Count(text, "screen_lighting=low") == 1 && Count(text, "screen_lighting_ao=0.60") == 1 &&
           Count(text, "screen_lighting_gi=1.20") == 1);
    setenv("BB_SCREEN_LIGHTING", "high", 1);
    setenv("BB_SCREEN_LIGHTING_AO", "nan", 1);
    setenv("BB_SCREEN_LIGHTING_GI", "90", 1);
    Load();
    assert(Get().screen_lighting == ScreenLightingHigh && Get().screen_lighting_ao == 0.5f &&
           Get().screen_lighting_gi == 2.0f);
    unsetenv("BB_SCREEN_LIGHTING"); unsetenv("BB_SCREEN_LIGHTING_AO"); unsetenv("BB_SCREEN_LIGHTING_GI");
    Load();
    assert(Count(text, "map_view=3") == 1);
    Get().map_view = 0;
    Load();
    assert(Get().map_view == 3 && !Get().compass && Get().compass_position==2);
    assert(!Get().bloodstains && !Get().team_pings && Get().ping_key==3 && Get().free_camera);
    assert(Get().menu_language==MenuLanguage::De && !Get().menu_language_auto);
    assert(std::string(BbLanguage::Text(MenuLanguage::De,"Begin the hunt"))!="Begin the hunt");
    assert(std::string(BbLanguage::Text(MenuLanguage::Russian,"Begin the hunt"))!="Begin the hunt");
    assert(std::string(BbLanguage::GameFolder(MenuLanguage::De))=="deude");
    assert(Count(text, "upscaler=fsr3") == 1 && Count(text, "# a comment") == 0);
    Save(); // again: still once each
    text = Read(path);
    assert(Count(text, "fps_cap=40") == 1 && Count(text, "fullscreen=1") == 1);
    assert(Count(text, "game_timing=adaptive") == 1);
    assert(Count(text, "preset=3") == 1);
    assert(Count(text, "mouse_look=1") == 1 && Count(text, "mouse_sensitivity=2.25") == 1 &&
           Count(text, "mouse_invert_y=1") == 1);
    assert(Count(text, "achievement_banners=0") == 1 && Count(text, "mouse_look=1") == 1);
    assert(Count(text, "pad_menu_combo=0") == 1);
    assert(Count(text, "mouse_sensitivity=2.25") == 1 && Count(text, "mouse_invert_y=1") == 1);
    assert(Count(text, "mouse_vertical=0.75") == 1);
    setenv("BB_ULTRAWIDE_RES", "3840x1080", 1);
    f = std::fopen(path, "w");
    assert(f);
    std::fprintf(f, "output_res=3840x2160\nupscaler=metalfx\npreset=3\nlive_resolution=1\nultrawide_res=3840x1080\nstandard_output_res=2560x1440\n");
    std::fclose(f);
    Load();
    assert(Get().output_res == OutputDefault && Get().upscaler == UpscalerOff);
    assert(!Get().hdr && !Get().startup_hdr && Get().hdr_paper_white == 1.0f && Get().hdr_peak == 0.0f);
    assert(Get().preset == NativeAA && Get().live_resolution == 0);
    Save();
    text = Read(path);
    assert(Count(text, "ultrawide_res=3840x1080") == 1);
    assert(Count(text, "standard_output_res=2560x1440") == 1);
    assert((OutputSize() == std::array<int, 2>{3840, 1080}));
    unsetenv("BB_ULTRAWIDE_RES");
    Load();
    assert((OutputSize() == std::array<int, 2>{1920, 1080}));
    setenv("BB_ULTRAWIDE_RES", "3840x1080garbage", 1);
    Load();
    assert(Get().ultrawide_width == 0);
    unsetenv("BB_ULTRAWIDE_RES");
    // Frame generation is saved once, waits for restart, and the environment can
    // force either launch state. Unsupported hardware keeps the saved preference.
    Get().framegen = true;
    assert(FrameGenerationNeedsRestart() && !Get().startup_framegen);
    Save(); Save();
    assert(Count(Read(path), "framegen=1") == 1);
    Load();
    assert(Get().framegen && Get().startup_framegen && !FrameGenerationNeedsRestart());
    Get().framegen_supported = false;
    Save();
    assert(Count(Read(path), "framegen=1") == 1);
    setenv("BB_METAL_FRAMEGEN", "0", 1);
    Load();
    assert(!Get().framegen && !Get().startup_framegen && Get().framegen_forced);
    Get().show_fps = true; // an unrelated menu change must not save the override
    Save(); Save(); Load();
    assert(Count(Read(path), "framegen=1") == 1);
    assert(!Get().framegen && Get().saved_framegen);
    unsetenv("BB_METAL_FRAMEGEN");
    Load();
    assert(Get().framegen && Get().startup_framegen && !Get().framegen_forced);
    Get().framegen = false;
    Save(); Load();
    setenv("BB_METAL_FRAMEGEN", "1", 1);
    Load();
    assert(Get().framegen && Get().startup_framegen && Get().framegen_forced);
    Get().show_fps = false;
    Save(); Save(); Load();
    assert(Count(Read(path), "framegen=0") == 1);
    assert(Get().framegen && !Get().saved_framegen);
    unsetenv("BB_METAL_FRAMEGEN");
    Load();
    assert(!Get().framegen && !Get().startup_framegen && !Get().framegen_forced);
    // An absent preference must also stay opt-in after an overridden launch.
    f = std::fopen(path, "w");
    assert(f);
    std::fputs("fps_cap=60\n", f);
    std::fclose(f);
    setenv("BB_METAL_FRAMEGEN", "1", 1);
    Load(); Save();
    assert(Get().framegen && Count(Read(path), "framegen=0") == 1);
    unsetenv("BB_METAL_FRAMEGEN");
    Load();
    assert(!Get().framegen && !Get().startup_framegen);
    std::remove(path);
    std::puts("PASS: settings menu keeps the launcher's keys");
    return 0;
}
