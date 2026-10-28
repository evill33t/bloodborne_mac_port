// SPDX-License-Identifier: GPL-2.0-or-later
#include "collision_scene.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <functional>
using namespace BbMapEditor;
void Reject(const std::function<void()>& f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed);}
void AddFloor(std::vector<float>& out,float x,float y,float z){const float v[]={x,y,z,x+1,y,z,x,y,z+1,x+1,y,z,x+1,y,z+1,x,y,z+1};out.insert(out.end(),std::begin(v),std::end(v));}
int main(int argc,char** argv) {
    std::vector<float> triangles;AddFloor(triangles,0,0,0);AddFloor(triangles,0,3,0);
    auto scene=CollisionScene::FromTriangles(triangles);assert(scene.Triangles()==4);
    assert(scene.Below({.25f,5,.25f},10)==3);assert(scene.Below({.25f,2,.25f},10)==0);assert(!scene.Below({.25f,2,.25f},1));
    assert(scene.Below({.25f,5,.25f},10,.5f)==3.5f);assert(!scene.Below({2,5,2},10));assert(!scene.Below({.25f,-1,.25f},10));assert(scene.Below({0,1,0},10)==0);
    Reject([&]{scene.Below({NAN,0,0},10);});Reject([&]{scene.Below({0,0,0},-1);});Reject([&]{scene.Below({0,0,0},10,NAN);});
    auto bad=triangles;bad.push_back(0);Reject([&]{CollisionScene::FromTriangles(bad);});bad=triangles;bad[0]=INFINITY;Reject([&]{CollisionScene::FromTriangles(bad);});
    std::vector<float> grid;for(int x=0;x<32;++x)for(int z=0;z<32;++z)AddFloor(grid,float(x),float(x+z),float(z));auto indexed=CollisionScene::FromTriangles(grid);
    for(int x=0;x<32;++x)for(int z=0;z<32;++z)assert(indexed.Below({x+.25f,float(x+z)+2,z+.25f},4)==float(x+z));
    const float slope[]={0,0,0,1,1,0,0,0,1};assert(std::abs(*CollisionScene::FromTriangles(slope).Below({.25f,2,.25f},4)-.25f)<.0001f);
    const float wall[]={0,0,0,0,1,0,0,0,1};assert(CollisionScene::FromTriangles(wall).Triangles()==0);
    if(argc>1) {
        const auto root=std::filesystem::path(argv[1])/"dvdroot_ps4";const std::string map=argc>2?argv[2]:"m24_01_00_01",base=map.substr(0,6)+"_00_00";
        auto doc=Document::Open(root/"map/MapStudio"/(map+".msb.dcx"));const auto studio=BbMapArt::ReadMapStudio(doc.Inflated());assert(studio);
        auto read=[&](const std::string& path){try{return ReadFile(root/path);}catch(const std::exception&){return std::vector<std::uint8_t>{};}};
        auto range=[&](const std::string& path,std::uint64_t at,std::uint64_t count){std::ifstream f(root/path,std::ios::binary|std::ios::ate);std::vector<std::uint8_t>b;if(!f||f.tellg()<0||at>std::uint64_t(f.tellg())||count>std::uint64_t(f.tellg())-at||count>512*1024*1024)return b;b.resize(count);f.seekg(at);if(!f.read(reinterpret_cast<char*>(b.data()),b.size()))b.clear();return b;};
        const auto real=CollisionScene::Read(*studio,base,read,range);assert(real.Triangles());std::cout<<map<<": "<<real.Triangles()<<" collision floor triangles, "<<real.missing_models<<" missing models\n";
        if(map=="m24_01_00_01") {
            const auto it=std::ranges::find(doc.Parts(),std::string("c2630_0000"),&Part::name);assert(it!=doc.Parts().end());auto raised=it->transform.position;raised[1]+=2;const auto floor=real.Below(raised,4);assert(floor && std::abs(*floor-it->transform.position[1])<1);std::cout<<it->name<<" MSB y="<<it->transform.position[1]<<" collision="<<*floor<<"\n";
        }
    }
    std::cout<<"Collision floor checks passed\n";
}
