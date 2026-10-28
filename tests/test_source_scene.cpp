// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_scene.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
using namespace BbMapEditor;
static void Reject(const std::function<void()>& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed);}
template<class T>void Add(std::vector<std::uint8_t>& b,T v){const auto at=b.size();b.resize(at+sizeof(v));std::memcpy(b.data()+at,&v,sizeof(v));}
static void Text(std::vector<std::uint8_t>& b,const std::string& s){Add(b,std::uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
int main(int argc,char** argv) {
    std::vector<std::uint8_t>b={'B','B','M','S','R','C','0','1'};Text(b,"DS2S");Text(b,"m10_02_00_00");Text(b,"Preview only");Add(b,1u);Text(b,"m0000");Add(b,3u);Add(b,3u);Add(b,0xffaaaaaau);
    const auto coordinates=b.size();for(float f:{0.f,0.f,0.f,1.f,0.f,0.f,0.f,0.f,1.f})Add(b,f);
    const auto indices=b.size();for(auto i:{0u,1u,2u})Add(b,i);Add(b,1u);Text(b,"generator_1000");Text(b,"EnemyId:123");Add(b,2u);Add(b,123);
    for(float f:{10.f,20.f,30.f,0.f,90.f,0.f,1.f,1.f,1.f})Add(b,f);
    auto scene=SourceScene::Read(b);assert(scene.models.size()==1 && scene.placements.size()==1 && scene.placements[0].source_id==123);
    auto bad=b;bad.push_back(0);Reject([&]{SourceScene::Read(bad);});
    bad=b;bad.resize(b.size()-1);Reject([&]{SourceScene::Read(bad);});
    bad=b;std::uint32_t index=3;std::memcpy(bad.data()+indices,&index,4);Reject([&]{SourceScene::Read(bad);});
    bad=b;float nan=NAN;std::memcpy(bad.data()+coordinates,&nan,4);Reject([&]{SourceScene::Read(bad);});
    for(std::size_t size=0;size<coordinates;++size)Reject([&]{SourceScene::Read(std::span(b).first(size));});
    if(argc>1){auto real=SourceScene::Open(argv[1]);assert(!real.models.empty());std::size_t triangles=0;for(const auto& [name,m]:real.models)triangles+=m->indices.size()/3;
        std::cout<<real.game<<" "<<real.map<<" models="<<real.models.size()<<" placements="<<real.placements.size()<<" triangles="<<triangles<<"\n";
        if(argc>2){auto doc=Document::Open(argv[2]);const auto original=doc.Save();const auto count=doc.Parts().size();const auto it=std::ranges::find(doc.Parts(),PartKind::Enemy,&Part::kind);assert(it!=doc.Parts().end());const auto native=it-doc.Parts().begin();const auto native_model=it->model;
            const auto source=std::ranges::find(real.placements,2u,&SourcePlacement::kind);assert(source!=real.placements.end());const auto from=source->transform.position;const auto target=it->transform.position;
            const auto copy=real.Transfer(doc,native,source-real.placements.begin(),from,target,90);assert(doc.Parts()[copy].transform.position==target && doc.Parts()[copy].model==native_model);
            const auto copy_name=doc.Parts()[copy].name;
            assert(doc.Undo() && doc.Parts().size()==count && doc.Save()==original);assert(doc.Redo() && doc.Parts().size()==count+1);
            const auto reopened=Document::Read(doc.Save());const auto found=std::ranges::find(reopened.Parts(),copy_name,&Part::name);assert(found!=reopened.Parts().end() && found->transform.position==target);
            auto relative=real;relative.placements[source-real.placements.begin()].transform.position[0]+=2;relative.placements[source-real.placements.begin()].transform.position[1]+=3;relative.placements[source-real.placements.begin()].transform.position[2]+=4;
            const auto rotated=relative.Transfer(doc,native,source-real.placements.begin(),from,target,90);const auto pos=doc.Parts()[rotated].transform.position;assert(std::abs(pos[0]-target[0]-4)<.001f && std::abs(pos[1]-target[1]-3)<.001f && std::abs(pos[2]-target[2]+2)<.001f);assert(doc.Undo());
            const auto before=doc.Parts().size();auto t=doc.Parts()[native].transform;t.scale[0]*=2;Reject([&]{doc.DuplicateAt(native,t);});assert(doc.Parts().size()==before);
            Reject([&]{real.Transfer(doc,native,real.placements.size(),from,target,0);});
        }
    }
    std::cout<<"Source scene checks passed\n";
}
