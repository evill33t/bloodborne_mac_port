// SPDX-License-Identifier: GPL-2.0-or-later
// Mac launcher adapter for Kaniel / lKaniel's Map card and BbMapArt (8d5e54f).
#include "bbport_map_charts.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>

int main(int argc,char** argv) {
    std::string game,cache,area;int selected=0;
    for(int i=1;i+1<argc;i+=2) {
        std::string key=argv[i];
        if(key=="--game") game=argv[i+1];else if(key=="--cache") cache=argv[i+1];
        else if(key=="--view") selected=std::clamp(std::atoi(argv[i+1]),0,4);
        else if(key=="--area") area=argv[i+1];else return 2;
    }
    if(game.empty() || cache.empty()) {std::fputs("Usage: bbmap-prepare --game folder --cache folder [--view 0..4] [--area m24_01]\n",stderr);return 2;}
    setenv("BB_MAP_GAME_DIR",game.c_str(),1);setenv("BB_GPU_USER_DIR",cache.c_str(),1);
    std::set<uint32_t> maps;std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(game)/"dvdroot_ps4/map/MapStudio",error)) {
        const auto name=entry.path().filename().string();unsigned major=0,minor=0;
        if(!name.ends_with(".msb.dcx") || std::sscanf(name.c_str(),"m%u_%u_",&major,&minor)!=2 || major>255 || minor>255) continue;
        if(!area.empty() && !name.starts_with(area)) continue;
        maps.insert((major<<24)|(minor<<16));
    }
    if(error || maps.empty()) {std::fputs("No map geometry found in the selected game folder.\n",stderr);return 1;}
    const int views=selected?2:1,total=int(maps.size())*views;int prepared=0,failed=0;
    auto progress=[&](uint32_t map,int view) {std::printf("{\"prepared\":%d,\"total\":%d,\"failed\":%d,\"map\":\"%s\",\"view\":%d}\n",prepared,total,failed,BbMapArt::MapNameOf(map).c_str(),view);std::fflush(stdout);};
    for(auto map:maps) for(int n=0;n<views;++n) {
        const int view=n?selected:0;progress(map,view);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(10);
        while(!BbMapCharts::Get(map,view)) {
            if(BbMapCharts::Status(map|unsigned(view)).find("unavailable")!=std::string::npos || std::chrono::steady_clock::now()>deadline) {++failed;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ++prepared;progress(map,view);
    }
    return failed?1:0;
}
