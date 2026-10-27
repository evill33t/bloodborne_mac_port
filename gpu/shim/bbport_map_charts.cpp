// SPDX-License-Identifier: GPL-2.0-or-later
// Uses lKaniel's BbMapArt parsers/rasterizer; no game geometry or textures are distributed.
#include "bbport_map_charts.h"
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <pthread.h>
#include <unistd.h>

namespace BbMapCharts {
namespace {
namespace fs = std::filesystem;
std::vector<std::uint8_t> Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Save(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    if(bytes.empty()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    auto temp=path;temp += ".tmp-"+std::to_string(getpid());
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if(file) fs::rename(temp,path,ec);
}

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::uint32_t> queue;
    std::set<std::uint32_t> asked;
    std::map<std::uint32_t,std::string> status;
    std::map<std::uint32_t,std::shared_ptr<const BbMapArt::Chart>> charts;
    std::map<std::uint32_t,std::shared_ptr<const BbMapArt::AreaMesh>> meshes,mesh_previews;
    std::map<std::uint32_t,std::shared_ptr<const BbMapArt::Chart>> previews;
    std::map<std::uint32_t,std::uint64_t> used;
    std::uint64_t clock=0;
    std::uint32_t selected_mesh=0;
    void Retain(std::uint32_t key,std::shared_ptr<const BbMapArt::Chart> result) {
        // Full detail for recent areas, plus bounded 512px atlas pictures of every prepared area.
        auto preview=std::make_shared<BbMapArt::Chart>(*result);
        preview->storeys={};
        while(std::max(preview->width,preview->height)>512) *preview=BbMapArt::Halved(*preview);
        if(result->storeys.width && result->storeys.height) {
            auto& levels=preview->storeys;levels.width=(preview->width+1)/2;levels.height=(preview->height+1)/2;
            const auto& from=result->storeys;const size_t cells=size_t(levels.width)*levels.height;
            levels.roof.resize(cells);levels.level.resize(cells*BbMapArt::Chart::Levels);levels.rgb.resize(cells*BbMapArt::Chart::Levels);
            for(int y=0;y<levels.height;++y) for(int x=0;x<levels.width;++x) {
                const size_t at=size_t(y)*levels.width+x,source=size_t(std::min(from.height-1,int(y*result->ppm/preview->ppm)))*from.width+std::min(from.width-1,int(x*result->ppm/preview->ppm));
                levels.roof[at]=from.roof[source];
                for(int n=0;n<BbMapArt::Chart::Levels;++n) {levels.level[at*BbMapArt::Chart::Levels+n]=from.level[source*BbMapArt::Chart::Levels+n];levels.rgb[at*BbMapArt::Chart::Levels+n]=from.rgb[source*BbMapArt::Chart::Levels+n];}
            }
        }
        previews[key]=std::move(preview);charts[key]=std::move(result);used[key]=++clock;
        const auto bytes=[](const auto& c) {return c->rgba.size()+c->storeys.roof.size()*2+c->storeys.level.size()*2+c->storeys.rgb.size()*4;};
        size_t total=0;for(const auto& [id,c]:charts) total+=bytes(c);
        while(total>128u*1024*1024 && charts.size()>1) {
            auto oldest=charts.end();
            for(auto it=charts.begin();it!=charts.end();++it)
                if(it->first!=key && (oldest==charts.end() || used[it->first]<used[oldest->first])) oldest=it;
            total-=bytes(oldest->second);used.erase(oldest->first);charts.erase(oldest);
        }
    }
    // Declared last: join the worker before destroying its queues and loaded data.
    std::jthread worker;
    ~State() { worker.request_stop();wake.notify_all(); }

    void Work(std::stop_token stop) {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        const char* game=std::getenv("BB_MAP_GAME_DIR");
        if(!game) game=std::getenv("BB_GAME_DIR");
        const fs::path root=game ? fs::path(game)/"dvdroot_ps4" : fs::path{};
        const char* user=std::getenv("BB_GPU_USER_DIR");
        const fs::path cache=(user?fs::path(user):fs::path("user"))/"map";
        // Cache identity follows file contents' source metadata, including mod-view symlinks.
        // Compute once on this utility worker: opening the map never scans assets on the UI thread.
        std::uint64_t hash=14695981039346656037ull;
        auto mix=[&](const std::string& s) {for(unsigned char c:s) {hash^=c;hash*=1099511628211ull;}};
        std::error_code ec;
        std::map<std::string,std::string> stamps;
        if(!root.empty()) for(fs::recursive_directory_iterator it(root/"map",fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied,ec),end;it!=end && !ec && !stop.stop_requested();it.increment(ec)) {
            if(it->is_regular_file(ec)) {
                const auto size=it->file_size(ec);
                const auto time=it->last_write_time(ec).time_since_epoch().count();
                stamps[it->path().lexically_relative(root).string()]=std::to_string(size)+":"+std::to_string(static_cast<long long>(time));
            }
        }
        for(const auto& [path,stamp]:stamps) {mix(path);mix(stamp);}
        char identity[40];std::snprintf(identity,sizeof(identity),"metal-v1-%016llx",static_cast<unsigned long long>(hash));
        const auto folder=cache/identity;
        const auto read=[&](const std::string& relative) {return stop.stop_requested() || root.empty()?std::vector<std::uint8_t>{}:Read(root/relative);};
        const auto range=[&](const std::string& relative,std::uint64_t offset,std::uint64_t size) {
            std::vector<std::uint8_t> bytes;
            if(stop.stop_requested() || root.empty() || size>(1ull<<30)) return bytes;
            std::ifstream file(root/relative,std::ios::binary);
            if(!file) return bytes;
            file.seekg(offset);bytes.resize(size);
            file.read(reinterpret_cast<char*>(bytes.data()),size);bytes.resize(std::max<std::streamsize>(0,file.gcount()));return bytes;
        };
        while(!stop.stop_requested()) {
            std::uint32_t key;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop.stop_requested() || !queue.empty();});
             if(stop.stop_requested()) return;
             key=queue.front();queue.pop_front();status[key]="Rendering terrain from your game files...";}
            const bool mesh=key&8;const int view=int(key&7);const auto map=key&~15u;
            const auto path=folder/(BbMapArt::MapNameOf(map)+(mesh?".bbmesh":view?("_v"+std::to_string(view)+".bbmap"):".bbmap"));
            const auto start=std::chrono::steady_clock::now();
            bool ready=false,cached=false;
            try {
                if(mesh) {
                    auto data=BbMapArt::LoadMesh(Read(path));cached=bool(data);
                    if(!data) data=BbMapArt::MeshOf(map,read,1,range,.5f);
                    if(data && !stop.stop_requested()) {
                        if(!cached) Save(path,BbMapArt::SaveMesh(*data));
                        auto result=std::make_shared<const BbMapArt::AreaMesh>(std::move(*data));
                        BbMapArt::MeshBuilder builder(3.f);
                        for(size_t i=0;i+2<result->indices.size();i+=3) {
                            float triangle[9];uint32_t colors[3];
                            for(int n=0;n<3;++n) {auto at=result->indices[i+n];std::copy_n(result->positions.data()+at*3,3,triangle+n*3);colors[n]=result->colors[at];}
                            builder.Add(triangle,colors);
                        }
                        auto preview=std::make_shared<const BbMapArt::AreaMesh>(builder.Finish());
                        std::scoped_lock lock(mutex);if(map==selected_mesh) {meshes.clear();meshes[map]=std::move(result);}
                        mesh_previews[map]=std::move(preview);ready=true;
                    }
                } else {
                    auto data=BbMapArt::Load(Read(path));cached=bool(data);
                    // One utility worker bounds peak generation memory; full charts use an LRU budget.
                    if(!data) data=BbMapArt::ChartArea(map,read,6.f,4096,1,range,true,view);
                    if(data && !stop.stop_requested()) {
                        if(!cached) Save(path,BbMapArt::Save(*data));
                        auto result=std::make_shared<const BbMapArt::Chart>(std::move(*data));
                        std::scoped_lock lock(mutex);Retain(key,std::move(result));ready=true;
                    }
                }
            } catch(const std::exception& e) {std::fprintf(stderr,"Map: %s\n",e.what());}
            std::printf("Map: %s %s %s in %.1fs\n",BbMapArt::MapNameOf(map).c_str(),mesh?"mesh":"terrain",ready?(cached?"cached":"generated"):"unavailable",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
            std::scoped_lock lock(mutex);
            status[key]=ready?"":"Terrain unavailable for this area. The coordinate map is still usable.";
            asked.erase(key);
        }
    }
    void Ask(std::uint32_t key,bool priority=false) {
        if(!key || (status.contains(key) && !status[key].empty() && !asked.contains(key))) return;
        if(asked.insert(key).second) {queue.push_back(key);status[key]="Preparing terrain...";}
        if(priority) {auto it=std::find(queue.begin(),queue.end(),key);if(it!=queue.end()) {queue.erase(it);queue.push_front(key);}}
        if(!worker.joinable()) worker=std::jthread([this](std::stop_token stop){Work(stop);});
        wake.notify_one();
    }
};
State& Shared() {static State s;return s;}
}
std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t map,int view) {
    map|=std::clamp(view,0,4);
    auto& s=Shared();std::scoped_lock lock(s.mutex);
    if(auto it=s.charts.find(map);it!=s.charts.end()) {s.used[map]=++s.clock;return it->second;}
    s.Ask(map,true);return {};
}
std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t map) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);s.selected_mesh=map;
    if(auto it=s.meshes.find(map);it!=s.meshes.end()) return it->second;
    if(map) s.Ask(map|8u,true);return {};
}
std::map<std::uint32_t,std::shared_ptr<const BbMapArt::Chart>> Atlas(int view) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);
    std::map<std::uint32_t,std::shared_ptr<const BbMapArt::Chart>> result;
    for(const auto& [key,chart]:s.previews) if((key&7)==unsigned(view)) result[key&~15u]=chart;
    return result;
}
void Prepare(std::uint32_t map,int view) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);const auto key=map|unsigned(std::clamp(view,0,4));
    if(!s.previews.contains(key)) s.Ask(key);
}
std::map<std::uint32_t,std::shared_ptr<const BbMapArt::AreaMesh>> MeshAtlas() {
    auto& s=Shared();std::scoped_lock lock(s.mutex);return s.mesh_previews;
}
void PrepareMesh(std::uint32_t map) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);if(!s.mesh_previews.contains(map)) s.Ask(map|8u);
}
std::string Status(std::uint32_t map,bool mesh) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);return s.status[map|(mesh?8u:0u)];
}
}
