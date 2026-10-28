// SPDX-License-Identifier: GPL-2.0-or-later
#include "map_scene.h"
#include "bbport_game_art.h"
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace BbMapEditor {
namespace {
template<class T> T Get(std::span<const std::uint8_t> b,std::size_t at) {
    if(at>b.size() || sizeof(T)>b.size()-at) throw std::runtime_error("Truncated object archive");
    T v;std::memcpy(&v,b.data()+at,sizeof(T));return v;
}
std::vector<std::span<const std::uint8_t>> Binder(std::span<const std::uint8_t> b) {
    if(b.size()<64 || std::memcmp(b.data(),"BND4",4) || b[9] || b[10]!=1) throw std::runtime_error("Unsupported object binder");
    auto count=Get<std::uint32_t>(b,12);auto stride=Get<std::uint64_t>(b,32);
    if(stride!=0x24 || count>10000 || count>(b.size()-64)/stride) throw std::runtime_error("Invalid object binder table");
    std::vector<std::span<const std::uint8_t>> files;
    for(std::size_t i=0;i<count;++i) {
        auto at=64+i*stride;auto size=Get<std::uint64_t>(b,at+8);auto offset=Get<std::uint32_t>(b,at+24);
        if(b[at]!=0x40 || offset>b.size() || size>b.size()-offset) throw std::runtime_error("Unsupported or invalid object binder entry");
        files.push_back(b.subspan(offset,size));
    }
    return files;
}
std::shared_ptr<const BbMapArt::AreaMesh> ObjectMesh(std::span<const std::uint8_t> packed) {
    auto b= BbGameArt::InflateDcx(packed);if(!b) return {};
    auto entries=Binder(*b);
    std::map<std::string,std::uint32_t> texture_ids;
    std::vector<BbMapArt::Texture> textures;
    for(auto entry:entries) if(entry.size()>16 && !std::memcmp(entry.data(),"TPF\0",4)) {
        auto parsed=BbGameArt::ParseTpf(entry);if(!parsed) continue;
        for(std::size_t i=0;i<parsed->size();++i) {
            auto one=std::vector<std::uint8_t>(entry.begin(),entry.end());
            std::uint32_t count=1;std::memcpy(one.data()+8,&count,4);
            std::memcpy(one.data()+16,entry.data()+16+i*0x24,0x24);
            if(auto t=BbMapArt::DecodeTexture(one,128)) {
                texture_ids[(*parsed)[i].name]=std::uint32_t(textures.size());textures.push_back(std::move(*t));
            }
        }
    }
    auto color=[&](std::string_view names) {
        auto first=std::string(names.substr(0,names.find(';')));
        auto found=texture_ids.find(first);
        return found==texture_ids.end()?0xffaaa6a0u:BbMapArt::TexturedColor(found->second);
    };
    BbMapArt::MeshBuilder builder(.025f);
    for(auto entry:entries) if(entry.size()>8 && !std::memcmp(entry.data(),"FLVER\0L\0",8)) {
        std::vector<float> triangles,uv;std::vector<std::uint32_t> colors;
        if(!BbMapArt::AppendTriangles(entry,triangles,&colors,color,&uv)) continue;
        for(std::size_t at=0,n=0;at+9<=triangles.size();at+=9,++n) {
            std::uint32_t corners[3];
            for(int c=0;c<3;++c) {
                auto rgb=colors[n];
                if(BbMapArt::IsTextured(rgb)) {
                    const auto& tex=textures.at(rgb&0xffffffu);
                    rgb=tex.Sample(uv[n*12+c*4],uv[n*12+c*4+1]);
                }
                corners[c]=0xff000000u | (rgb&0xff)<<16 | (rgb&0xff00) | (rgb>>16&0xff);
            }
            builder.Add(triangles.data()+at,corners);
        }
        // The first FLVER is the object's main model; later entries may be other states/LODs.
        break;
    }
    auto mesh=builder.Finish();
    return mesh.indices.empty()?nullptr:std::make_shared<const BbMapArt::AreaMesh>(std::move(mesh));
}
}
Scene LoadScene(const std::filesystem::path& game,const std::string& map,
                const std::function<void(std::string)>& progress,std::stop_token stop) {
    if(!ValidMapName(map)) throw std::runtime_error("Invalid map name");
    const auto root=game/"dvdroot_ps4";Scene result;
    progress("Reading map placements...");
    result.document=Document::Open(root/"map/MapStudio"/(map+".msb.dcx"));
    const auto base_map=map.substr(0,6)+"_00_00";
    const auto read=[&](const std::string& relative) {
        if(stop.stop_requested()) return std::vector<std::uint8_t>{};
        // The in-game atlas uses base layouts. An editor must render the exact selected phase
        // layout while continuing to resolve terrain models/textures from the area's base folder.
        const auto path=relative=="map/MapStudio/"+base_map+".msb.dcx"?"map/MapStudio/"+map+".msb.dcx":relative;
        try {return ReadFile(root/path);} catch(const std::exception&) {return std::vector<std::uint8_t>{};}
    };
    const auto range=[&](const std::string& relative,std::uint64_t offset,std::uint64_t size) {
        std::vector<std::uint8_t> b;if(stop.stop_requested() || size>512*1024*1024) return b;
        std::ifstream f(root/relative,std::ios::binary|std::ios::ate);if(!f) return b;
        const auto length=f.tellg();if(length<0 || offset>std::uint64_t(length) || size>std::uint64_t(length)-offset) return b;
        b.resize(size);f.seekg(offset);if(!f.read(reinterpret_cast<char*>(b.data()),b.size())) b.clear();return b;
    };
    progress("Loading terrain with the in-game map renderer...");
    const auto major=std::stoul(map.substr(1,2)),minor=std::stoul(map.substr(4,2));
    if(auto mesh=BbMapArt::MeshOf(std::uint32_t(major<<24|minor<<16),read,1,range,.35f))
        result.terrain=std::make_shared<const BbMapArt::AreaMesh>(std::move(*mesh));
    if(stop.stop_requested()) return result;
    if(!result.terrain || result.terrain->indices.empty()) throw std::runtime_error("No terrain geometry could be loaded");
    std::set<std::string> wanted;
    for(const auto& o:result.document.Objects()) wanted.insert(o.model);
    std::size_t loaded=0;
    for(const auto& model:wanted) {
        if(stop.stop_requested()) return result;
        progress("Loading object models "+std::to_string(++loaded)+" / "+std::to_string(wanted.size()));
        // Identifiers from the MSB never become arbitrary paths.
        if(model.empty() || model[0]!='o' || model.find_first_not_of("o0123456789")!=std::string::npos) continue;
        try {result.models[model]=ObjectMesh(read("obj/"+model+".objbnd.dcx"));}
        catch(const std::exception&) {result.models[model]={};}
        if(!result.models[model]) ++result.missing_models;
    }
    progress("Ready");return result;
}
} // namespace BbMapEditor
