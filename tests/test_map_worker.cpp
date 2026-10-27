// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shim/bbport_map_charts.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
namespace fs=std::filesystem;
int main() {
    char temp[]="/tmp/bb-map-worker-XXXXXX";assert(mkdtemp(temp));
    const fs::path root(temp);fs::create_directories(root/"dvdroot_ps4/map");
    setenv("BB_MAP_GAME_DIR",temp,1);setenv("BB_GPU_USER_DIR",temp,1);
    const auto cache=root/"map/metal-v1-cbf29ce484222325";fs::create_directories(cache);
    BbMapArt::Chart chart;chart.width=chart.height=2;chart.ppm=1;chart.x0=-1;chart.z1=1;
    chart.rgba.assign(16,255);
    const auto bytes=BbMapArt::Save(chart);assert(!bytes.empty());
    for(auto name:{"m24_01_00_00","m21_00_00_00"}) {
        std::ofstream file(cache/(std::string(name)+".bbmap"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    }
    const auto get=[&](uint32_t map) {
        std::shared_ptr<const BbMapArt::Chart> result;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!(result=BbMapCharts::Get(map)) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(result && result->rgba==chart.rgba);return result;
    };
    assert(!BbMapCharts::Get(0));
    const auto first=get(0x18010000);
    get(0x15000000); // evicts the first resident area
    assert(first->rgba==chart.rgba); // consumers can still hold the evicted result safely
    get(0x18010000); // reloads its disk cache
    BbMapCharts::Get(0x19000000); // missing MSB: terminal fallback, no busy retry loop
    for(int i=0;i<1000 && BbMapCharts::Status(0x19000000).find("unavailable")==std::string::npos;++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(BbMapCharts::Status(0x19000000).find("unavailable")!=std::string::npos);
    fs::remove_all(root);
    std::puts("map-worker: cache, eviction, unavailable-area fallback and shutdown passed");
}
