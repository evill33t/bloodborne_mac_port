// SPDX-License-Identifier: GPL-2.0-or-later
#include "collision_scene.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace BbMapEditor {
CollisionScene CollisionScene::Read(const BbMapArt::MapStudio& studio,const std::string& map,
                                   const BbMapArt::ReadFile& read,const BbMapArt::ReadRange& range,std::stop_token stop) {
    if(!ValidMapName(map) || !range)throw std::runtime_error("Collision loading requires a valid area and range reader");
    struct Entry {std::string path;std::uint64_t offset,size;};std::map<std::string,Entry> entries;
    const auto stem="map/"+map+"/h"+map.substr(1);
    for(const auto* suffix:{"",".patch"}) {
        if(stop.stop_requested())return {};
        for(const auto& e:BbMapArt::ReadArchiveHeader(read(stem+".hkxbhd"+suffix))) {
            auto name=e.name.substr(e.name.find_last_of("\\/")+1);name=name.substr(0,name.find('.'));
            entries[name]={stem+".hkxbdt"+suffix,e.offset,e.size};
        }
    }
    std::map<std::string,std::vector<float>> models;std::vector<float> world;std::size_t missing=0;
    constexpr std::size_t max_floats=4000000u*9;
    for(const auto& c:studio.collisions) {
        if(stop.stop_requested())return {};
        const auto& p=c.piece;auto found=models.find(p.model);
        if(found==models.end()) {
            std::vector<float> mesh;auto entry=entries.find(p.model);
            if(entry==entries.end() && p.model.size()>1)entry=entries.find("h"+map.substr(1)+"_"+p.model.substr(1));
            if(entry!=entries.end()) {
                const auto& e=entry->second;
                if(e.size<=512u*1024*1024) {
                    const auto packed=range(e.path,e.offset,e.size);
                    if(packed.size()!=e.size)throw std::runtime_error("Short collision archive read");
                    if(!BbMapArt::AppendCollisionTriangles(UnpackAsset(packed),mesh))mesh.clear();
                }
            }
            if(mesh.empty())++missing;
            if(mesh.size()>max_floats)throw std::runtime_error("Collision model exceeds the geometry budget");
            found=models.emplace(p.model,std::move(mesh)).first;
        }
        const Transform transform{p.position,p.rotation,p.scale};
        if(found->second.size()>max_floats-world.size())throw std::runtime_error("Area collision exceeds four million triangles");
        for(std::size_t i=0;i+2<found->second.size();i+=3){const auto point=WorldPoint(transform,found->second.data()+i);world.insert(world.end(),point.begin(),point.end());}
    }
    auto result=FromTriangles(world);result.missing_models=missing;return result;
}
CollisionScene CollisionScene::FromTriangles(std::span<const float> world) {
    if(world.size()%9 || world.size()>4000000u*9)throw std::runtime_error("Invalid collision triangle budget");
    for(float f:world)if(!std::isfinite(f)||std::abs(f)>1e6f)throw std::runtime_error("Invalid collision coordinates");
    CollisionScene result;const auto floors=BbMapArt::Floors(world);result.triangles.reserve(floors.size()/9);
    BbMapArt::MeshBuilder builder(.1f);const std::uint32_t green[3]={0xff78b886u,0xff78b886u,0xff78b886u};
    for(std::size_t at=0;at+8<floors.size();at+=9) {
        Triangle t;for(int v=0;v<3;++v)std::copy_n(floors.data()+at+v*3,3,t.v[v].begin());result.triangles.push_back(t);builder.Add(floors.data()+at,green);
    }
    result.preview=std::make_shared<const BbMapArt::AreaMesh>(builder.Finish());
    result.order.resize(result.triangles.size());std::iota(result.order.begin(),result.order.end(),0u);
    if(!result.order.empty())result.Build(0,std::uint32_t(result.order.size()));return result;
}
std::uint32_t CollisionScene::Build(std::uint32_t begin,std::uint32_t end) {
    const auto index=std::uint32_t(nodes.size());Node node{{FLT_MAX,FLT_MAX,FLT_MAX},{-FLT_MAX,-FLT_MAX,-FLT_MAX},begin,end};
    for(auto at=begin;at<end;++at)for(const auto& p:triangles[order[at]].v)for(int c=0;c<3;++c){node.lo[c]=std::min(node.lo[c],p[c]);node.hi[c]=std::max(node.hi[c],p[c]);}
    nodes.push_back(node);
    if(end-begin>16) {
        const int axis=node.hi[0]-node.lo[0]>=node.hi[2]-node.lo[2]?0:2;const auto mid=begin+(end-begin)/2;
        auto center=[&](std::uint32_t i){const auto& v=triangles[i].v;return v[0][axis]+v[1][axis]+v[2][axis];};
        std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](auto a,auto b){return center(a)<center(b);});
        const auto left=Build(begin,mid),right=Build(mid,end);nodes[index].left=left;nodes[index].right=right;
    }
    return index;
}
std::optional<float> CollisionScene::Below(BbMapArt::Vec3 p,float distance,float offset) const {
    for(float f:p)if(!std::isfinite(f)||std::abs(f)>1e6f)throw std::runtime_error("Invalid snap coordinates");
    if(!std::isfinite(distance)||distance<=0||distance>1000||!std::isfinite(offset)||std::abs(offset)>1000)throw std::runtime_error("Snap range and offset must be finite and within 1000 metres");
    if(nodes.empty())return {};
    const double high=double(p[1])-offset+.002,low=double(p[1])-offset-distance;std::optional<double> best;std::vector<std::uint32_t> stack{0};
    while(!stack.empty()) {
        const auto& node=nodes[stack.back()];stack.pop_back();
        if(p[0]<node.lo[0]-.0001 || p[0]>node.hi[0]+.0001 || p[2]<node.lo[2]-.0001 || p[2]>node.hi[2]+.0001 || node.lo[1]>high || node.hi[1]<low || (best && node.hi[1]<*best))continue;
        if(node.right){stack.push_back(node.left);stack.push_back(node.right);continue;}
        for(auto i=node.begin;i<node.end;++i) {
            const auto& v=triangles[order[i]].v;
            const double ax=v[1][0]-double(v[0][0]),az=v[1][2]-double(v[0][2]),bx=v[2][0]-double(v[0][0]),bz=v[2][2]-double(v[0][2]),dx=p[0]-double(v[0][0]),dz=p[2]-double(v[0][2]);
            const double det=ax*bz-az*bx;if(std::abs(det)<1e-12)continue;
            const double a=(dx*bz-dz*bx)/det,b=(ax*dz-az*dx)/det;if(a<-.00001 || b<-.00001 || a+b>1.00001)continue;
            const double height=v[0][1]+a*(v[1][1]-double(v[0][1]))+b*(v[2][1]-double(v[0][1]));
            if(height<=high && height>=low && (!best || height>*best))best=height;
        }
    }
    return best?std::optional(float(*best+offset)):std::nullopt;
}
}
