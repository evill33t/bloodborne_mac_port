// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_params.h"
#include "map_document.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace BbMapEditor;
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value){assert(at+sizeof(T)<=b.size());std::memcpy(b.data()+at,&value,sizeof(value));}
void Text(std::vector<std::uint8_t>& b,std::size_t at,const std::u16string& text){for(auto c:text){Put<std::uint16_t>(b,at,c);at+=2;}Put<std::uint16_t>(b,at,0);}
template<class F> void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}assert(rejected);}
std::vector<std::uint8_t> Param(){
    std::vector<std::uint8_t> b(0xc0);Put<std::uint32_t>(b,0,0x90);Put<std::uint16_t>(b,10,2);std::memcpy(b.data()+12,"NPC_PARAM_ST",12);
    b[0x2d]=4;b[0x2e]=7;Put<std::uint64_t>(b,48,0x70);
    for(int i=0;i<2;++i){Put<std::int32_t>(b,0x40+i*24,10+i);Put<std::uint64_t>(b,0x48+i*24,0x70+i*16);Put<std::uint64_t>(b,0x50+i*24,0x90+i*8);}
    Text(b,0x90,u"敵");Text(b,0x98,u"NPC");return b;
}
std::vector<std::uint8_t> Binder(const std::vector<std::uint8_t>& param){
    std::vector<std::uint8_t> b(0xe0);std::memcpy(b.data(),"BND4",4);b[10]=1;b[48]=1;b[49]=0x74;
    Put<std::uint32_t>(b,12,1);Put<std::uint64_t>(b,16,64);Put<std::uint64_t>(b,32,36);Put<std::uint64_t>(b,40,b.size());
    Put<std::uint32_t>(b,64,0x40);Put<std::uint64_t>(b,72,param.size());Put<std::uint64_t>(b,80,param.size());Put<std::uint32_t>(b,88,b.size());Put<std::uint32_t>(b,96,100);
    Text(b,100,u"N:\\param\\NpcParam.param");b.insert(b.end(),param.begin(),param.end());return b;
}
int main(int argc,char** argv){
    auto raw=Param();auto table=ParamTable::Read(raw);assert(table.type=="NPC_PARAM_ST" && table.rows.size()==2);
    assert(table.Find(10)->name=="敵" && table.Find(11)->name=="NPC" && !table.Find(12));assert(table.Data(*table.Find(10)).size()==16);
    auto binder=Binder(raw);auto catalog=GameParams::Read(binder);assert(catalog.Table("NpcParam")->Find(11));assert(!catalog.Table("Absent"));
    EnemyData before;before.npc_param=123;before.think_param=456;before.chara_init=-1;before.talk_id=999;
    auto after=before;after.npc_param=11;ValidateCharacterReferences(before,after,catalog,{});
    after.npc_param=12;Reject([&]{ValidateCharacterReferences(before,after,catalog,{});});after=before;after.talk_id=1234;
    Reject([&]{ValidateCharacterReferences(before,after,catalog,{});});ValidateCharacterReferences(before,after,catalog,{{1234,"t1234.esd"}});
    after=before;after.init_anim=-2;Reject([&]{ValidateCharacterReferences(before,after,catalog,{});});
    for(std::size_t n:{std::size_t(0),std::size_t(20),std::size_t(0x9d)})Reject([&]{ParamTable::Read(std::span(raw).first(n));});
    auto bad=raw;Put<std::uint64_t>(bad,0x48,0xffffffffffffffffull);Reject([&]{ParamTable::Read(bad);});
    bad=raw;Put<std::int32_t>(bad,0x58,10);assert(ParamTable::Read(bad).rows.size()==2); // shipped files contain duplicate IDs
    bad=raw;Put<std::uint64_t>(bad,0x60,0x70);Reject([&]{ParamTable::Read(bad);});
    bad=binder;bad[64]=0x41;Reject([&]{ReadBinder(bad);});
    if(argc>1){
        auto actual=GameParams::Open(argv[1]);assert(actual.Tables().size()==8);
        for(const auto& [name,t]:actual.Tables()){assert(!t.rows.empty());for(const auto& r:t.rows)assert(!t.Data(r).empty());std::cout<<name<<": "<<t.rows.size()<<" rows, "<<t.type<<"\n";}
        assert(actual.Table("NpcParam")->Find(263400));assert(actual.Table("NpcThinkParam")->Find(263400));
        assert(ReadNpcInfo(*actual.Table("NpcParam"),263400)->hp==150);assert(ReadNpcInfo(*actual.Table("NpcParam"),263000)->hp==120);
        const auto talks=GameParams::TalkScripts(argv[1],"m24_01_00_01");assert(!talks.empty());std::cout<<"Central Yharnam talk scripts: "<<talks.size()<<"\n";
        auto npc=before;npc.npc_param=263400;npc.think_param=263400;npc.chara_init=-1;npc.talk_id=talks.begin()->first;ValidateCharacterReferences(before,npc,actual,talks);
        const auto* item_table=actual.Table("ItemLotParam");auto lot=ReadItemLot(*item_table,2410360);assert(lot.Flag()==52410360);
        lot.id=900000000;lot.SetFlag(52410900);actual.SetContents(lot,{4,1000,3});assert(lot.First()==ItemContents({4,1000,3}));
        Reject([&]{actual.SetContents(lot,{4,1000,0});});Reject([&]{actual.SetContents(lot,{8,1000,1});});Reject([&]{actual.SetContents(lot,{4,2147483647,1});});
        auto second=lot;second.id+=10;second.SetFlag(52410901);actual.SetContents(second,{4,1000,5});
        const auto original=ReadFile(std::filesystem::path(argv[1])/"dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx");assert(actual.SaveLots({})==original);
        const auto saved=actual.SaveLots({lot,second});const auto reopened=GameParams::Read(saved);const auto& rows=reopened.Table("ItemLotParam")->rows;
        assert(rows.size()==item_table->rows.size()+2);std::size_t next=0;
        for(const auto& row:rows) {
            if(row.id==lot.id || row.id==second.id)continue;const auto& old=item_table->rows[next++];assert(row.id==old.id && row.name==old.name);
            assert(std::ranges::equal(reopened.Table("ItemLotParam")->Data(row),item_table->Data(old)));
        }
        assert(next==item_table->rows.size());const auto old_files=ReadBinder(original),new_files=ReadBinder(saved);assert(old_files.size()==new_files.size());
        for(std::size_t i=0;i<old_files.size();++i)if(old_files[i].name.find("ItemLotParam.param")==std::string::npos)assert(old_files[i].bytes==new_files[i].bytes && old_files[i].name==new_files[i].name && old_files[i].id==new_files[i].id);
        auto bad_lot=lot;bad_lot.id=2410360;Reject([&]{actual.SaveLots({bad_lot});});
        std::cout<<"PASS: custom item lots, exact stacks, independent flags, all original row payloads and other binder entries preserved\n";
    }
    std::cout<<"PASS: native binder/PARAM lookup, Unicode names, bounded parsing, missing row and ESD validation\n";
}
