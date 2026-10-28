// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_scene.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace BbMapEditor {
namespace {
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t at=0;
    template<class T>T Get(){if(at>bytes.size() || sizeof(T)>bytes.size()-at)throw std::runtime_error("Truncated source scene");T v;std::memcpy(&v,bytes.data()+at,sizeof(v));at+=sizeof(v);return v;}
    std::uint32_t Count(std::uint32_t max){const auto n=Get<std::uint32_t>();if(n>max)throw std::runtime_error("Source scene exceeds its size budget");return n;}
    std::string Text(){const auto n=Count(1048576);if(n>bytes.size()-at)throw std::runtime_error("Truncated source metadata");std::string s(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;if(s.find('\0')!=s.npos)throw std::runtime_error("Invalid source metadata");return s;}
    float Float(){auto f=Get<float>();if(!std::isfinite(f)||std::abs(f)>1e6f)throw std::runtime_error("Invalid source coordinates");return f;}
    BbMapArt::Vec3 Vector(){return {Float(),Float(),Float()};}
};
}
SourceScene SourceScene::Open(const std::filesystem::path& path){return Read(ReadFile(path));}
SourceScene SourceScene::Read(std::span<const std::uint8_t> bytes) {
    if(bytes.size()>512u*1024*1024 || bytes.size()<8 || std::memcmp(bytes.data(),"BBMSRC01",8))throw std::runtime_error("Unsupported source scene");
    Reader r{bytes,8};SourceScene result;result.game=r.Text();result.map=r.Text();result.notice=r.Text();
    if(!ValidMapName(result.map))throw std::runtime_error("Invalid source map identifier");
    const auto count=r.Count(20000);std::size_t total_vertices=0,total_indices=0;
    for(std::uint32_t n=0;n<count;++n) {
        auto name=r.Text();const auto vertices=r.Count(8000000),indices=r.Count(24000000),color=r.Get<std::uint32_t>();
        total_vertices+=vertices;total_indices+=indices;
        if(total_vertices>8000000 || total_indices>24000000 || indices%3 || !vertices || std::uint64_t(vertices)*12+std::uint64_t(indices)*4>bytes.size()-r.at)throw std::runtime_error("Invalid source geometry budget");
        auto mesh=std::make_shared<BbMapArt::AreaMesh>();mesh->positions.reserve(vertices*3);mesh->colors.assign(vertices,color);mesh->indices.reserve(indices);
        for(std::uint32_t v=0;v<vertices;++v)for(int c=0;c<3;++c)mesh->positions.push_back(r.Float());
        for(std::uint32_t i=0;i<indices;++i){const auto index=r.Get<std::uint32_t>();if(index>=vertices)throw std::runtime_error("Invalid source mesh index");mesh->indices.push_back(index);}
        if(!result.models.emplace(name,std::move(mesh)).second)throw std::runtime_error("Duplicate source model");
    }
    const auto placements=r.Count(200000);result.placements.reserve(placements);
    for(std::uint32_t n=0;n<placements;++n) {
        SourcePlacement p;p.name=r.Text();p.model=r.Text();p.kind=r.Count(3);p.source_id=r.Get<std::int32_t>();p.transform.position=r.Vector();p.transform.rotation=r.Vector();p.transform.scale=r.Vector();result.placements.push_back(std::move(p));
    }
    if(r.at!=bytes.size())throw std::runtime_error("Unexpected trailing source data");return result;
}
std::size_t SourceScene::Transfer(Document& doc,std::size_t target,std::size_t source,BbMapArt::Vec3 from,BbMapArt::Vec3 to,float yaw) const {
    const auto& p=placements.at(source);const auto& native=doc.Parts().at(target);
    if((p.kind!=1 && p.kind!=2) || (p.kind==2)!=(native.kind==PartKind::Enemy))throw std::runtime_error("Choose a Bloodborne template of the same placement kind");
    auto t=native.transform;const auto angle=yaw*.017453292519943295f;const auto x=p.transform.position[0]-from[0],z=p.transform.position[2]-from[2];
    t.position={to[0]+x*std::cos(angle)+z*std::sin(angle),to[1]+(p.transform.position[1]-from[1]),to[2]-x*std::sin(angle)+z*std::cos(angle)};
    t.rotation=p.transform.rotation;t.rotation[1]+=yaw;
    return doc.DuplicateAt(target,t);
}
}
