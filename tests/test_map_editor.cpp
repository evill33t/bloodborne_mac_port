// SPDX-License-Identifier: GPL-2.0-or-later
#include "map_document.h"
#include "miniz.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace BbMapEditor;
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value) {std::memcpy(b.data()+at,&value,sizeof(value));}
void Text(std::vector<std::uint8_t>& b,std::size_t at,const std::string& s) {for(char c:s) {Put<std::uint16_t>(b,at,c);at+=2;}}
std::vector<std::uint8_t> Fixture() {
    std::vector<std::uint8_t> b(0x600,0);
    const std::uint8_t header[]={0x4d,0x53,0x42,0x20,1,0,0,0,0x10,0,0,0,0,0,1,0xff};
    std::memcpy(b.data(),header,16);
    const char* names[]={"MODEL_PARAM_ST","EVENT_PARAM_ST","POINT_PARAM_ST","PARTS_PARAM_ST"};
    for(int i=0;i<4;++i) {
        std::size_t at=0x10+i*0x40;bool record=i==0 || i==3;
        Put<std::uint32_t>(b,at,3);Put<std::uint32_t>(b,at+4,record?2:1);Put<std::int64_t>(b,at+8,0x180+i*0x40);
        Text(b,0x180+i*0x40,names[i]);
        if(record)Put<std::int64_t>(b,at+16,i==0?0x300:0x400);
        Put<std::int64_t>(b,at+16+(record?8:0),i==3?0:at+0x40);
    }
    Put<std::int64_t>(b,0x300,0x40);Text(b,0x340,"o240083");
    Put<std::int64_t>(b,0x408,0xd0);Text(b,0x4d0,"o240083_1025");
    Put<std::uint32_t>(b,0x414,1);Put<std::int32_t>(b,0x41c,0);
    Put<BbMapArt::Vec3>(b,0x428,{1,2,3});Put<BbMapArt::Vec3>(b,0x434,{10,20,30});Put<BbMapArt::Vec3>(b,0x440,{1,1,1});
    for(std::size_t i=0x500;i<b.size();++i)b[i]=std::uint8_t(i*13); // unknown data must survive
    return b;
}
std::vector<std::uint8_t> EnemyFixture() {
    auto b=Fixture();b.resize(0x1000);
    // Mixed records exercise full-part collision indices and subtype-only patrol indices.
    Put<std::uint32_t>(b,0x14,3);Put<std::int64_t>(b,0x28,0x380);Put<std::int64_t>(b,0x30,0x50);
    Put<std::int64_t>(b,0x380,0x40);Text(b,0x3c0,"c3030");
    Put<std::uint32_t>(b,0x54,3);Put<std::int64_t>(b,0x60,0xb00);Put<std::int64_t>(b,0x68,0xb80);Put<std::int64_t>(b,0x70,0x90);
    Put<std::int64_t>(b,0xb00,0x30);Put<std::uint32_t>(b,0xb0c,1);Text(b,0xb30,"Sound");
    Put<std::int64_t>(b,0xb80,0x30);Put<std::uint32_t>(b,0xb8c,14);Text(b,0xbb0,"Patrol");
    Put<std::uint32_t>(b,0x94,2);Put<std::int64_t>(b,0xa0,0xc00);Put<std::int64_t>(b,0xa8,0xd0);
    Put<std::int64_t>(b,0xc00,0x80);Text(b,0xc80,"Region");
    Put<std::uint32_t>(b,0xd4,4);Put<std::int64_t>(b,0xe8,0x600);Put<std::int64_t>(b,0xf0,0x900);Put<std::int64_t>(b,0xf8,0);
    Put<std::int64_t>(b,0x608,0xd0);Text(b,0x6d0,"c3030_0001");
    Put<std::uint32_t>(b,0x614,2);Put<std::int32_t>(b,0x61c,1);Put<std::int32_t>(b,0x610,14);
    Put<BbMapArt::Vec3>(b,0x628,{5,6,7});Put<BbMapArt::Vec3>(b,0x640,{1,1,1});
    Put<std::int64_t>(b,0x6b0,0x100);Put<std::int64_t>(b,0x6b8,0x120);Put<std::int32_t>(b,0x700,2400200);
    Put<std::int32_t>(b,0x728,303000);Put<std::int32_t>(b,0x72c,303001);Put<std::int32_t>(b,0x730,-1);
    Put<std::int32_t>(b,0x738,-1);Put<std::int32_t>(b,0x73c,2);Put<std::int16_t>(b,0x740,0);
    for(int i=0;i<8;++i)Put<std::int16_t>(b,0x748+i*2,i==0?0:i==1?9:-1);
    Put<std::int32_t>(b,0x758,7000);Put<std::int32_t>(b,0x75c,-1);
    Put<std::int64_t>(b,0x908,0xd0);Put<std::uint32_t>(b,0x914,5);Text(b,0x9d0,"h0000");
    return b;
}
void BE(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v) {for(int i=0;i<4;++i)b[at+i]=std::uint8_t(v>>(24-i*8));}
std::vector<std::uint8_t> Pack(const std::vector<std::uint8_t>& b) {
    std::vector<std::uint8_t> out(0x4c,0);std::memcpy(out.data(),"DCX\0",4);BE(out,4,0x10000);BE(out,8,0x18);BE(out,12,0x24);BE(out,16,0x44);BE(out,20,0x4c);
    std::memcpy(out.data()+0x18,"DCS\0",4);BE(out,0x1c,b.size());std::memcpy(out.data()+0x24,"DCP\0DFLT",8);BE(out,0x2c,32);out[0x30]=9;BE(out,0x40,0x10100);std::memcpy(out.data()+0x44,"DCA\0",4);BE(out,0x48,8);
    mz_ulong n=mz_compressBound(b.size());std::vector<std::uint8_t> zipped(n);assert(mz_compress2(zipped.data(),&n,b.data(),b.size(),6)==MZ_OK);
    BE(out,0x20,n);out.insert(out.end(),zipped.begin(),zipped.begin()+n);return out;
}
template<class F> void Reject(F f) {bool threw=false;try {f();}catch(const std::exception&){threw=true;}assert(threw);}
int main(int argc,char** argv) {
    auto raw=Fixture(),packed=Pack(raw);
    for(auto input:{raw,packed}) {
        auto d=Document::Read(input);assert(d.Save()==input);assert(d.Inflated()==raw);assert(!d.Changed());assert(d.Parts().size()==1);
        auto t=d.Parts()[0].transform;t.position[0]+=4;t.rotation[1]+=10;assert(d.Set(0,t));assert(d.Changed());
        auto edited=d.Inflated();
        for(std::size_t i=0;i<raw.size();++i)if(i<0x428 || i>=0x44c)assert(raw[i]==edited[i]);
        auto reopened=Document::Read(d.Save());assert(reopened.Parts()[0].transform==t);assert(reopened.Inflated()==edited);
        assert(d.Undo());assert(!d.Changed());assert(d.Save()==input);assert(!d.Undo());assert(d.Redo());assert(d.Parts()[0].transform==t);
        assert(d.Undo());t.position[2]+=1;assert(d.Set(0,t));assert(!d.Redo());
        t.scale[0]=0;Reject([&]{d.Set(0,t);});t.scale[0]=1;t.position[0]=NAN;Reject([&]{d.Set(0,t);});
    }
    auto enemies=EnemyFixture();
    auto dummy=enemies;Put<std::uint32_t>(dummy,0x614,10);
    for(auto input:{enemies,Pack(enemies),dummy,Pack(dummy)}) {
        auto d=Document::Read(input);assert(d.Parts().size()==2);assert(d.Save()==input);
        const auto original=d.Inflated();
        const auto& part=d.Parts()[1];assert(part.kind==PartKind::Enemy && part.enemy);
        const auto& e=*part.enemy;
        assert(e.entity_id==2400200 && e.instance_id==14 && e.think_param==303000 && e.npc_param==303001);
        assert(e.talk_id==-1 && e.chara_init==-1 && e.init_anim==7000 && e.damage_anim==-1);
        assert(e.collision.name=="h0000" && e.patrol.name=="Patrol" && e.move_points[0].name=="Region");
        assert(e.move_points[1].Missing() && !e.move_points[2].Missing());
        auto t=part.transform;t.position[2]+=3;t.rotation[1]=90;d.Set(1,t);
        auto edited=d.Inflated();for(std::size_t i=0;i<original.size();++i)if(i<0x628 || i>=0x640)assert(edited[i]==original[i]);
        auto reopen=Document::Read(d.Save());assert(reopen.Parts()[1].transform==t && reopen.Parts()[1].enemy->collision.name=="h0000");
        assert(d.Undo() && d.Save()==input);assert(d.Redo());
        t.scale[0]=2;Reject([&]{d.Set(1,t);});
    }
    auto invalid_enemy=enemies;Put<std::int64_t>(invalid_enemy,0x6b8,0x2e0);Reject([&]{Document::Read(invalid_enemy);});
    invalid_enemy=enemies;Put<std::int64_t>(invalid_enemy,0x6b0,0x28);Reject([&]{Document::Read(invalid_enemy);});
    auto unicode=enemies;Put<std::uint16_t>(unicode,0xc80,0x65e5);Put<std::uint16_t>(unicode,0xc82,0);assert(Document::Read(unicode).Parts()[1].enemy->move_points[0].name=="\xe6\x97\xa5");
    auto bad=raw;Put<std::int64_t>(bad,0x28,0x10);Reject([&]{Document::Read(bad);});
    bad=raw;Put<std::uint32_t>(bad,0x14,0xffffffff);Reject([&]{Document::Read(bad);});
    bad=raw;Put<std::int64_t>(bad,0x408,0x28);Reject([&]{Document::Read(bad);});
    bad=raw;Put<std::int32_t>(bad,0x41c,9);Reject([&]{Document::Read(bad);});
    bad=raw;Put<float>(bad,0x428,std::numeric_limits<float>::infinity());Reject([&]{Document::Read(bad);});
    for(std::size_t n=0;n<0x4e8;n+=7)Reject([&]{Document::Read(std::span(raw.data(),n));});
    bad=packed;bad[0x20]^=1;Reject([&]{Document::Read(bad);});
    bad=packed;bad[0x28]='K';Reject([&]{Document::Read(bad);});
    auto d=Document::Read(packed);char temp[]="/tmp/bb-map-editor-XXXXXX";assert(mkdtemp(temp));
    auto folder=std::filesystem::path(temp)/"mod";auto path=d.Export(folder,"m24_01_00_00");assert(ReadFile(path)==packed);
    Reject([&]{d.Export(folder,"m24_01_00_00");});assert(ReadFile(path)==packed);
    Reject([&]{d.Export(std::filesystem::path(temp)/"bad","../x");});
    auto source=std::filesystem::path(temp)/"game/dvdroot_ps4/map/mapstudio/map.msb.dcx";
    std::filesystem::create_directories(source.parent_path());
    {std::ofstream f(source,std::ios::binary);f.write(reinterpret_cast<const char*>(packed.data()),packed.size());}
    auto guarded=Document::Open(source);
    Reject([&]{guarded.Export(std::filesystem::path(temp)/"game/mod","m24_01_00_00");});
    std::filesystem::create_directory_symlink(std::filesystem::path(temp)/"game",std::filesystem::path(temp)/"alias");
    Reject([&]{guarded.Export(std::filesystem::path(temp)/"alias/mod","m24_01_00_00");});
    assert(ReadFile(source)==packed);std::filesystem::remove_all(temp);
    Transform t;t.position={10,20,30};t.rotation={0,90,0};const float p[3]={1,0,0};auto q=WorldPoint(t,p);assert(std::abs(q[0]-10)<1e-5 && std::abs(q[2]-29)<1e-5);
    for(int i=1;i<argc;++i) {auto real=ReadFile(argv[i]);auto map=Document::Read(real);assert(map.Save()==real);auto before=map.Inflated();
        bool object=false,enemy=false,dummy_enemy=false;
        for(std::size_t n=0;n<map.Parts().size();++n) {
            const auto& p=map.Parts()[n];bool& tested=p.enemy?(p.enemy->dummy?dummy_enemy:enemy):object;if(tested)continue;tested=true;
            auto move=p.transform;move.position[0]+=1;move.rotation[1]+=15;map.Set(n,move);
            auto after=map.Inflated();for(std::size_t at=0;at<before.size();++at)if(at<p.record+0x28 || at>=p.record+0x40)assert(before[at]==after[at]);
            assert(Document::Read(map.Save()).Inflated()==after);assert(map.Undo());assert(map.Save()==real);
        }
        std::cout<<"Real map round-trip: "<<argv[i]<<" ("<<map.Parts().size()<<" placements; Enemy/DummyEnemy checked: "<<enemy<<"/"<<dummy_enemy<<")\n";
    }
    std::cout<<"PASS: unchanged bytes, transform-only writes, DCX, undo/redo, malformed files, exclusive exports\n";
}
