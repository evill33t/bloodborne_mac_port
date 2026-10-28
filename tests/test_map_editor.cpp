// SPDX-License-Identifier: GPL-2.0-or-later
#include "map_document.h"
#include "miniz.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace BbMapEditor;
#include "map_editor_fixture.h"
template<class T> T Get(const std::vector<std::uint8_t>& b,std::size_t at) {T v;assert(at+sizeof(v)<=b.size());std::memcpy(&v,b.data()+at,sizeof(v));return v;}
struct Table {std::string name;std::size_t at,name_at,next;std::vector<std::size_t> entries;};
std::vector<Table> Tables(const std::vector<std::uint8_t>& b) {
    std::vector<Table> result;
    for(std::size_t at=16;at;) {
        Table t;t.at=at;t.name_at=Get<std::int64_t>(b,at+8);
        for(auto p=t.name_at;Get<std::uint16_t>(b,p);p+=2)t.name+=char(Get<std::uint16_t>(b,p));
        const auto count=Get<std::uint32_t>(b,at+4);
        for(std::size_t n=0;n+1<count;++n)t.entries.push_back(Get<std::int64_t>(b,at+16+n*8));
        t.next=Get<std::int64_t>(b,at+16+(count-1)*8);at=t.next;result.push_back(t);
    }return result;
}
std::u16string RecordName(const std::vector<std::uint8_t>& b,std::size_t p,bool part) {
    auto at=p+Get<std::int64_t>(b,p+(part?8:0));std::u16string name;
    for(;Get<std::uint16_t>(b,at);at+=2)name+=char16_t(Get<std::uint16_t>(b,at));return name;
}
// Compare every opaque record byte after normalizing reference indices by target
// name. This independently checks that type grouping never redirects old references.
void PreservedRecords(const std::vector<std::uint8_t>& before,const std::vector<std::uint8_t>& after,std::size_t added,std::size_t regions=0,std::size_t events=0) {
    const auto old=Tables(before),now=Tables(after);assert(old.size()==now.size());
    const auto& old_parts=*std::ranges::find(old,std::string("PARTS_PARAM_ST"),&Table::name);
    const auto& new_parts=*std::ranges::find(now,std::string("PARTS_PARAM_ST"),&Table::name);
    std::map<std::u16string,std::size_t> target_indices;
    for(std::size_t i=0;i<new_parts.entries.size();++i)target_indices[RecordName(after,new_parts.entries[i],true)]=i;
    for(std::size_t i=1;i<new_parts.entries.size();++i)assert(Get<std::uint32_t>(after,new_parts.entries[i-1]+0x14)<=Get<std::uint32_t>(after,new_parts.entries[i]+0x14));
    std::set<std::size_t> bounds{before.size()};
    for(const auto& t:old){bounds.insert(t.at);bounds.insert(t.name_at);bounds.insert(t.entries.begin(),t.entries.end());}
    for(std::size_t s=0;s<old.size();++s) {
        assert(old[s].name==now[s].name);
        assert(now[s].entries.size()==old[s].entries.size()+(old[s].name=="PARTS_PARAM_ST"?added:old[s].name=="POINT_PARAM_ST"?regions:old[s].name=="EVENT_PARAM_ST"?events:0));
        std::map<std::uint32_t,std::size_t> event_occurrences;
        for(std::size_t i=0;i<old[s].entries.size();++i) {
            const auto a=old[s].entries[i],length=*bounds.upper_bound(a)-a;
            auto b=old[s].name=="PARTS_PARAM_ST"?new_parts.entries[target_indices.at(RecordName(before,a,true))]:now[s].entries[i];
            if(old[s].name=="EVENT_PARAM_ST") {
                const auto type=Get<std::uint32_t>(before,a+12);auto occurrence=event_occurrences[type]++;
                for(const auto p:now[s].entries)if(Get<std::uint32_t>(after,p+12)==type && occurrence--==0){b=p;break;}
            }
            std::vector<std::uint8_t> expected(before.begin()+a,before.begin()+a+length);
            auto reference=[&](std::size_t field){const auto index=Get<std::int32_t>(expected,field);if(index==-1)return;assert(index>=0 && std::size_t(index)<old_parts.entries.size());Put<std::int32_t>(expected,field,target_indices.at(RecordName(before,old_parts.entries[index],true)));};
            if(old[s].name=="PARTS_PARAM_ST") {
                const auto type=Get<std::uint32_t>(expected,0x14);const auto data=Get<std::int64_t>(expected,0xb8);
                if((type==1 || type==9) && data)reference(data+8);
                if(type==2 || type==10)reference(data+28);
            } else if(old[s].name=="EVENT_PARAM_ST") {
                const auto entity=Get<std::int64_t>(expected,0x18),data=Get<std::int64_t>(expected,0x20);const auto type=Get<std::uint32_t>(expected,12);
                if(entity)reference(entity);
                if(type==4)reference(data+8);
                if(type==5)for(int k=0;k<32;++k)reference(data+0x50+k*4);
                if(type==7)reference(data+4);
                if(type==16)for(int k=0;k<32;++k)reference(data+0x10+k*4);
            }
            for(std::size_t n=0;n<length;++n)if(old[s].name!="MODEL_PARAM_ST" || n<0x18 || n>=0x1c)assert(expected[n]==after[b+n]);
        }
    }
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
    {
        auto input=EnemyFixture();Put<std::uint32_t>(input,0x308,1);Put<std::uint32_t>(input,0x388,2);Put<std::int32_t>(input,0x398,1);
        // A following collision model must retain its target when the new enemy
        // model is inserted into the earlier, contiguous enemy group.
        Put<std::uint32_t>(input,0x14,4);Put<std::int64_t>(input,0x30,0xf00);Put<std::int64_t>(input,0x38,0x50);
        Put<std::int64_t>(input,0xf00,40);Put<std::uint32_t>(input,0xf08,5);Text(input,0xf28,"h0000");Put<std::int32_t>(input,0x91c,2);
        auto d=Document::Read(input);auto t=d.Parts()[1].transform;t.position[2]+=2;auto a=CharacterFixture();const auto copy=d.ImportCharacter(1,t,a);
        assert(d.Parts()[copy].model=="c6900" && d.Parts()[1].model=="c3030" && d.Parts()[copy].enemy->collision.name=="h0000");
        const auto saved=d.Save();const auto reopened=Document::Read(saved);assert(reopened.Parts().back().model=="c6900");
        const auto tables=Tables(d.Inflated());const auto& models=*std::ranges::find(tables,std::string("MODEL_PARAM_ST"),&Table::name);assert(models.entries.size()==4);
        const auto bytes=d.Inflated();for(std::size_t n=1;n<models.entries.size();++n)assert(Get<std::uint32_t>(bytes,models.entries[n-1]+8)<=Get<std::uint32_t>(bytes,models.entries[n]+8));
        const auto& pt=*std::ranges::find(tables,std::string("PARTS_PARAM_ST"),&Table::name);for(const auto p:pt.entries)if(RecordName(bytes,p,true)==u"h0000")assert(Get<std::int32_t>(bytes,p+0x1c)==3);
        assert(Get<std::int32_t>(bytes,models.entries[1]+0x18)==1 && Get<std::int32_t>(bytes,models.entries[2]+0x18)==1);
        assert(d.Undo() && d.Save()==input && !d.Changed());assert(d.CharacterImports().size()==1 && d.Redo() && d.Save()==saved);
        d.Remove(copy);assert(d.Save()==input && d.Undo() && d.Save()==saved);const auto again=d.Duplicate(copy);assert(d.Parts()[again].model=="c6900");assert(Document::Read(d.Save()).Parts().back().model=="c6900");
        Reject([&]{d.ImportCharacter(1,t,a);});auto bad=a;bad.model="../../bad";Reject([&]{Document::ValidateCharacterImport(bad);});bad=a;bad.files["../notes"]={1};Reject([&]{Document::ValidateCharacterImport(bad);});Reject([&]{d.ImportCharacter(0,t,CharacterFixture("c6901"));});
        std::cout<<"PASS: independent character model groups, original model references and loading collision, imported duplication, grouped undo/redo and package guards\n";
    }
    invalid_enemy=enemies;Put<std::int64_t>(invalid_enemy,0x6b0,0x28);Reject([&]{Document::Read(invalid_enemy);});
    auto unicode=enemies;Put<std::uint16_t>(unicode,0xd80,0x65e5);Put<std::uint16_t>(unicode,0xd82,0);assert(Document::Read(unicode).Parts()[1].enemy->move_points[0].name=="\xe6\x97\xa5");
    for(auto input:{raw,packed,enemies,Pack(enemies),dummy,Pack(dummy)}) {
        auto d=Document::Read(input);const auto before=d.Inflated();const auto original=d.Parts();
        const auto source=original.size()-1;const auto one=d.Duplicate(source);assert(one==original.size());
        const auto first=d.Parts()[one];assert(first.added && first.transform==original[source].transform);
        assert(first.name!=original[source].name && first.entity_id!=original[source].entity_id && first.instance_id!=original[source].instance_id);
        if(original.size()>1)assert(first.entity_id==2409003); // reserves collision, region and event IDs too
        const auto two=d.Duplicate(one);const auto second=d.Parts()[two];
        assert(second.name!=first.name && second.entity_id!=first.entity_id && second.instance_id!=first.instance_id);
        auto inflated=d.Inflated();PreservedRecords(before,inflated,2);
        auto reopened=Document::Read(d.Save());assert(reopened.Parts().size()==original.size()+2);
        assert(reopened.Parts()[one].name==first.name && reopened.Parts()[one].entity_id==first.entity_id);
        if(first.enemy)assert(reopened.Parts()[one].enemy->collision.name=="h0000" && reopened.Parts()[one].enemy->patrol.name=="Patrol");
        auto move=first.transform;move.position[0]+=2;move.rotation[1]+=15;d.Set(one,move);
        assert(Document::Read(d.Save()).Parts()[one].transform==move);
        d.Remove(one);assert(d.Parts()[one].name==second.name);assert(d.Undo());assert(d.Parts()[one].transform==move);
        assert(d.Redo());assert(d.Undo());assert(d.Undo());assert(d.Parts()[one].transform==first.transform);
        assert(d.Undo());assert(d.Parts().size()==original.size()+1);assert(d.Undo());assert(!d.Changed() && d.Save()==input);
        assert(d.Redo());assert(d.Parts().back().name==first.name);assert(d.Undo());
        const auto branched=d.Duplicate(source);assert(d.Parts()[branched].name!=first.name && !d.Redo());
        d.Remove(branched);assert(!d.Changed() && d.Save()==input);
        Reject([&]{d.Remove(0);});Reject([&]{reopened.Remove(one);});
        const auto tables=Tables(inflated);const auto& model_table=*std::ranges::find(tables,std::string("MODEL_PARAM_ST"),&Table::name);
        assert(Get<std::int32_t>(inflated,model_table.entries.at(original[source].enemy?1:0)+0x18)==3);
    }
    {
        const auto input=Pack(enemies);auto d=Document::Read(input);
        assert(d.Regions().size()==1 && d.Routes().size()==1 && d.Routes()[0].data.points[0].name=="Region");
        auto t=d.Regions()[0].transform;t.position={20,30,40};d.SetRegion(0,t);
        const auto edited=d.Inflated();for(std::size_t n=0;n<enemies.size();++n)if(n<0xd14 || n>=0xd20)assert(edited[n]==enemies[n]);
        assert(Document::Read(d.Save()).Regions()[0].transform==t);assert(d.Undo() && d.Save()==input);
        auto bad=t;bad.scale[0]=2;Reject([&]{d.SetRegion(0,bad);});
        const auto a=d.AddWaypoint({1,2,3}),b=d.AddWaypoint({4,5,6});const auto new_route=d.AddRoute();
        assert(d.Regions()[a].key>9); // unresolved legacy move point 9 must not become a new key
        auto route=d.Routes()[new_route].data;route.type=1;
        route.points[0]={d.Regions()[a].key,{}};route.points[1]={d.Regions()[b].key,{}};d.SetRoute(new_route,route);
        const auto character=d.Duplicate(1);auto npc=*d.Parts()[character].enemy;
        npc.patrol={d.Routes()[new_route].key,{}};npc.patrol_type=1;npc.npc_param+=1;npc.think_param+=2;
        npc.move_points[0]={d.Regions()[a].key,{}};d.SetEnemy(character,npc);
        auto reopened=Document::Read(d.Save());const auto& saved=*std::ranges::find(reopened.Parts(),d.Parts()[character].name,&Part::name);
        assert(saved.enemy->patrol.name==d.Routes()[new_route].name && saved.enemy->patrol_type==1);
        assert(saved.enemy->move_points[0].name==d.Regions()[a].name && saved.enemy->npc_param==303002 && saved.enemy->think_param==303002);
        const auto& saved_route=*std::ranges::find(reopened.Routes(),d.Routes()[new_route].name,&Route::name);
        assert(saved_route.data.points[1].name==d.Regions()[b].name && saved_route.data.type==1);
        assert(reopened.Regions()[a].transform.position==BbMapArt::Vec3({1,2,3}));
        Reject([&]{d.RemoveRoute(new_route);});Reject([&]{d.RemoveRegion(a);});
        auto invalid=npc;invalid.entity_id=123;Reject([&]{d.SetEnemy(character,invalid);});
        invalid=*d.Parts()[character].enemy;invalid.collision.index=1;Reject([&]{d.SetEnemy(character,invalid);});
        auto invalid_route=route;invalid_route.points[0].index=123456;Reject([&]{d.SetRoute(new_route,invalid_route);});
        d.Remove(character);d.RemoveRoute(new_route);d.RemoveRegion(a);
        auto original_route=d.Routes()[0].data;original_route.points[1]={d.Regions()[a].key,{}};d.SetRoute(0,original_route);
        reopened=Document::Read(d.Save());assert(reopened.Routes()[0].data.points[1].name==d.Regions()[a].name);
        assert(reopened.Routes()[0].data.points[1].index==1); // key survives removal of an earlier new point
        Reject([&]{d.RemoveRegion(0);});Reject([&]{d.RemoveRoute(0);});
        while(d.Undo()){}assert(!d.Changed() && d.Save()==input);
        while(d.Redo()){}assert(Document::Read(d.Save()).Regions().size()==2);
    }
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
    char area_temp[]="/tmp/bb-map-area-XXXXXX";assert(mkdtemp(area_temp));
    const auto area=std::filesystem::path(area_temp);
    auto write=[&](const auto& p,const auto& bytes){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());};
    auto sibling=Document::Read(enemies);const auto sibling_copy=sibling.Duplicate(1);const auto reserved=sibling.Parts()[sibling_copy];
    write(area/"m24_01_00_00.msb.dcx",sibling.Save());write(area/"m24_01_00_01.msb.dcx",Pack(enemies));
    auto variant=Document::Open(area/"m24_01_00_01.msb.dcx");const auto added=variant.Duplicate(1);const auto allocated=variant.Parts()[added];
    assert(allocated.name!=reserved.name && allocated.entity_id!=reserved.entity_id && allocated.instance_id!=reserved.instance_id);
    assert(ReadFile(area/"m24_01_00_00.msb.dcx")==sibling.Save());
    std::filesystem::remove_all(area);
    Transform t;t.position={10,20,30};t.rotation={0,90,0};const float p[3]={1,0,0};auto q=WorldPoint(t,p);assert(std::abs(q[0]-10)<1e-5 && std::abs(q[2]-29)<1e-5);
    std::shared_ptr<const GameParams> params;
    if(argc>1)params=std::make_shared<const GameParams>(GameParams::Open(std::filesystem::path(argv[1]).parent_path().parent_path().parent_path().parent_path()));
    for(int i=1;i<argc;++i) {auto real=ReadFile(argv[i]);auto map=Document::Read(real);map.AttachParameters(params);assert(map.Save()==real);auto before=map.Inflated();
        bool object=false,enemy=false,dummy_enemy=false;
        for(std::size_t n=0;n<map.Parts().size();++n) {
            const auto& p=map.Parts()[n];bool& tested=p.enemy?(p.enemy->dummy?dummy_enemy:enemy):object;if(tested)continue;tested=true;
            auto move=p.transform;move.position[0]+=1;move.rotation[1]+=15;map.Set(n,move);
            auto after=map.Inflated();for(std::size_t at=0;at<before.size();++at)if(at<p.record+0x28 || at>=p.record+0x40)assert(before[at]==after[at]);
            assert(Document::Read(map.Save()).Inflated()==after);assert(map.Undo());assert(map.Save()==real);
        }
        object=enemy=dummy_enemy=false;
        const auto count=map.Parts().size();
        for(std::size_t n=0;n<count;++n) {
            const auto p=map.Parts()[n];bool& tested=p.enemy?(p.enemy->dummy?dummy_enemy:enemy):object;if(tested)continue;tested=true;
            const auto copy=map.Duplicate(n),again=map.Duplicate(copy);auto saved=map.Save();
            auto reopened=Document::Read(saved);assert(reopened.Parts().size()==count+2);
            const auto& reread=*std::ranges::find(reopened.Parts(),map.Parts()[copy].name,&Part::name);
            assert(reread.entity_id==map.Parts()[copy].entity_id);
            assert(reread.name!=map.Parts()[again].name);
            PreservedRecords(before,reopened.Inflated(),2);
            if(p.enemy)assert(reread.enemy->collision.name==p.enemy->collision.name);
            auto move=map.Parts()[copy].transform;move.position[0]+=1;map.Set(copy,move);
            const auto moved=Document::Read(map.Save());assert(std::ranges::find(moved.Parts(),map.Parts()[copy].name,&Part::name)->transform==move);assert(map.Undo());
            map.Remove(copy);assert(Document::Read(map.Save()).Parts().size()==count+1);assert(map.Undo());
            assert(map.Undo());assert(map.Undo());assert(map.Save()==real);
        }
        const auto region_count=map.Regions().size(),route_count=map.Routes().size();
        auto character=std::ranges::find(map.Parts(),PartKind::Enemy,&Part::kind);
        const auto character_index=std::size_t(character-map.Parts().begin());
        const auto position=character!=map.Parts().end()?character->transform.position:BbMapArt::Vec3{1,2,3};
        const auto copy=character_index<count?map.Duplicate(character_index):count;
        const auto a=map.AddWaypoint(position),b=map.AddWaypoint({position[0]+1,position[1],position[2]}),r=map.AddRoute();
        auto patrol=map.Routes()[r].data;patrol.type=1;
        patrol.points[0]={map.Regions()[a].key,{}};patrol.points[1]={map.Regions()[b].key,{}};map.SetRoute(r,patrol);
        if(character_index<count) {auto metadata=*map.Parts()[copy].enemy;metadata.patrol={map.Routes()[r].key,{}};metadata.patrol_type=1;for(auto& point:metadata.move_points)point={};map.SetEnemy(copy,metadata);}
        const auto with_patrol=Document::Read(map.Save());
        assert(with_patrol.Regions().size()==region_count+2 && with_patrol.Routes().size()==route_count+1);
        assert(with_patrol.Routes().back().data.points[1].name==map.Regions()[b].name);
        if(character_index<count)assert(std::ranges::find(with_patrol.Parts(),map.Parts()[copy].name,&Part::name)->enemy->patrol.name==map.Routes()[r].name);
        PreservedRecords(before,with_patrol.Inflated(),character_index<count?1:0,2,1);
        while(map.Undo()){}assert(!map.Changed() && map.Save()==real);
        const auto treasure_count=map.Treasures().size();
        for(std::size_t item=0;item<treasure_count;++item) {
            const auto t=map.Treasures()[item];const auto* table=params->Table("ItemLotParam");
            if(t.data.in_chest || t.data.lots[1]!=-1 || t.data.lots[2]!=-1 || !table->Find(t.data.lots[0]))continue;
            if(ReadItemLot(*table,t.data.lots[0]).Flag()<=0)continue;
            const auto part=std::ranges::find(map.Parts(),t.data.part.name,&Part::name);if(part==map.Parts().end() || part->kind!=PartKind::Object)continue;
            const auto copy=map.DuplicateTreasure(item);assert(map.Treasures().size()==treasure_count+1 && map.Parts().size()==count+1 && map.ItemLots().size()==1);
            assert(map.ItemLots()[0].Flag()!=ReadItemLot(*table,t.data.lots[0]).Flag());map.SetItemContents(copy,{4,1000,3});
            auto reopened=Document::Read(map.Save());assert(reopened.Treasures().size()==treasure_count+1);
            assert(reopened.Treasures().back().data.part.name==map.Treasures()[copy].data.part.name);PreservedRecords(before,reopened.Inflated(),1,0,1);
            const auto support=std::ranges::find(map.Parts(),map.Treasures()[copy].data.part.name,&Part::name);
            assert(support->type==1); // dormant object templates become active pickup copies
            Reject([&]{map.Remove(std::size_t(support-map.Parts().begin()));});
            const auto second=map.DuplicateTreasure(copy);assert(map.ItemLots().size()==2 && map.ItemLots()[0].id!=map.ItemLots()[1].id && map.ItemLots()[0].Flag()!=map.ItemLots()[1].Flag());
            map.RemoveTreasure(second);assert(map.Undo());assert(map.Undo());assert(map.ItemLots().size()==1);
            map.RemoveTreasure(copy);assert(map.ItemLots().empty() && map.Parts().size()==count && map.Save()==real);
            assert(map.Undo());assert(map.Treasures().size()==treasure_count+1 && map.ItemLots().size()==1);
            assert(map.Undo());assert(map.Undo());assert(!map.Changed() && map.Save()==real); // content edit, then grouped creation
            assert(map.Redo());assert(map.Treasures().size()==treasure_count+1 && map.Parts().size()==count+1 && map.ItemLots().size()==1);
            while(map.Undo()){}assert(map.Save()==real);break;
        }
        std::cout<<"Real map round-trip: "<<argv[i]<<" ("<<map.Parts().size()<<" placements; Enemy/DummyEnemy checked: "<<enemy<<"/"<<dummy_enemy<<")\n";
    }
    std::cout<<"PASS: unchanged bytes, prop/enemy duplication, region transforms, new waypoints/patrols, character settings, preserved references, identifier allocation, DCX, undo/remove/redo, malformed files, exclusive exports\n";
}
