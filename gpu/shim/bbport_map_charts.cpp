// SPDX-License-Identifier: GPL-2.0-or-later
// Uses lKaniel's BbMapArt parsers/rasterizer; no game geometry or textures are distributed.
#include "bbport_map_charts.h"
#include <condition_variable>
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
    std::map<std::uint32_t,std::shared_ptr<const BbMapArt::AreaMesh>> meshes;
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
            const bool mesh=key&1;const auto map=key&~1u;
            const auto path=folder/(BbMapArt::MapNameOf(map)+(mesh?".bbmesh":".bbmap"));
            const auto start=std::chrono::steady_clock::now();
            bool ready=false,cached=false;
            try {
                if(mesh) {
                    auto data=BbMapArt::LoadMesh(Read(path));cached=bool(data);
                    if(!data) data=BbMapArt::MeshOf(map,read,1,range,.5f);
                    if(data && !stop.stop_requested()) {
                        if(!cached) Save(path,BbMapArt::SaveMesh(*data));
                        auto result=std::make_shared<const BbMapArt::AreaMesh>(std::move(*data));
                        std::scoped_lock lock(mutex);meshes.clear();meshes[map]=std::move(result);ready=true;
                    }
                } else {
                    auto data=BbMapArt::Load(Read(path));cached=bool(data);
                    // A single canvas limits memory while the game is running on a 16 GB Mac.
                    if(!data) data=BbMapArt::ChartArea(map,read,6.f,4096,1,range,true);
                    if(data && !stop.stop_requested()) {
                        if(!cached) Save(path,BbMapArt::Save(*data));
                        auto result=std::make_shared<const BbMapArt::Chart>(std::move(*data));
                        std::scoped_lock lock(mutex);charts.clear();charts[map]=std::move(result);ready=true;
                    }
                }
            } catch(const std::exception& e) {std::fprintf(stderr,"Map: %s\n",e.what());}
            std::printf("Map: %s %s %s in %.1fs\n",BbMapArt::MapNameOf(map).c_str(),mesh?"mesh":"terrain",ready?(cached?"cached":"generated"):"unavailable",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
            std::scoped_lock lock(mutex);
            status[key]=ready?"":"Terrain unavailable for this area. The coordinate map is still usable.";
            asked.erase(key);
        }
    }
    void Ask(std::uint32_t key) {
        if(!key || (status.contains(key) && !status[key].empty() && !asked.contains(key))) return;
        if(asked.insert(key).second) {queue.push_back(key);status[key]="Preparing terrain...";}
        if(!worker.joinable()) worker=std::jthread([this](std::stop_token stop){Work(stop);});
        wake.notify_one();
    }
};
State& Shared() {static State s;return s;}
}
std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t map) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);
    if(auto it=s.charts.find(map);it!=s.charts.end()) return it->second;
    s.Ask(map);return {};
}
std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t map) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);
    if(auto it=s.meshes.find(map);it!=s.meshes.end()) return it->second;
    if(map) s.Ask(map|1u);return {};
}
std::string Status(std::uint32_t map,bool mesh) {
    auto& s=Shared();std::scoped_lock lock(s.mutex);return s.status[map|(mesh?1u:0u)];
}
}
