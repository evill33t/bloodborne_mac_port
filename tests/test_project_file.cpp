// SPDX-License-Identifier: GPL-2.0-or-later
#include "project_file.h"
#include "map_editor_fixture.h"
#include "miniz.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace BbMapEditor;
namespace fs=std::filesystem;
template<class F>void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}assert(rejected);}
void Write(const fs::path& path,const std::vector<std::uint8_t>& bytes){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());assert(f.good());}
Document Loaded(const Project& p){
    auto d=p.OriginalDocument();const auto params=p.Source("param/gameparam/gameparam.parambnd.dcx");if(!params.empty())d.AttachParameters(std::make_shared<const GameParams>(GameParams::Open(p.game,params)));
    const auto edf=p.Source("event/common.emedf.dcx");if(!edf.empty()){
        auto defs=std::make_shared<const EventDefinitions>(EventDefinitions::Read(edf));
        for(const auto& name:p.EventNames())d.AttachEvents(name,std::make_shared<const EventFile>(EventFile::Open(p.game/"dvdroot_ps4/event"/name,defs,p.Source("event/"+name))));
    }
    p.Restore(d);return d;
}
void Same(const Document& a,const Document& b){assert(a.Save()==b.Save() && a.ItemLots()==b.ItemLots() && a.EventEdits()==b.EventEdits() && a.Quests()==b.Quests() && a.Changed()==b.Changed());}
void History(Document a,Document b){
    Same(a,b);while(a.Undo()){assert(b.Undo());Same(a,b);}assert(!b.Undo());
    while(a.Redo()){assert(b.Redo());Same(a,b);}assert(!b.Redo());
}
void Checksum(std::vector<std::uint8_t>& b){Put<std::uint32_t>(b,b.size()-4,mz_crc32(0,b.data(),b.size()-4));}
int main(int argc,char** argv){
    const auto root=fs::temp_directory_path()/("bb-project-test-"+std::to_string(getpid()));assert(fs::create_directory(root));
    try{
        const auto game=root/"game",studio=game/"dvdroot_ps4/map/MapStudio";fs::create_directories(studio);const std::string map="m24_01_00_00";const auto path=studio/(map+".msb.dcx");Write(path,EnemyFixture());
        auto d=Document::Open(path);auto t=d.Parts()[0].transform;t.position[0]+=2;d.Set(0,t);
        const auto copy=d.DuplicateAt(1,Transform{{5,7,9},{0,90,0},{1,1,1}});const auto copy_name=d.Parts()[copy].name;
        const auto region=d.AddWaypoint({10,6,7});const auto route=d.AddRoute();auto r=d.Routes()[route].data;r.points[0]={d.Regions()[region].key,d.Regions()[region].name};d.SetRoute(route,r);
        auto enemy=*d.Parts()[copy].enemy;enemy.patrol={d.Routes()[route].key,d.Routes()[route].name};d.SetEnemy(copy,enemy);
        const auto discarded=d.Duplicate(0);const auto discarded_name=d.Parts()[discarded].name;d.Remove(discarded);assert(d.Undo()); // redo tail includes deletion
        auto project=Project::Capture(game,map,d);project.selected_kind=1;project.selected_name=copy_name;project.camera={5,7,9,45,.5f,.8f};const auto file=root/"session.bbmapproject";project.Save(file);
        auto reopened=Project::Open(file);assert(reopened.selected_name==copy_name && reopened.camera==project.camera);auto loaded=Loaded(reopened);Same(d,loaded);History(d,loaded);
        assert(loaded.Redo() && loaded.Parts().size()==d.Parts().size()-1);assert(loaded.Undo());
        const auto old=d.Parts().size();assert(loaded.Remove(copy) && loaded.Parts().size()==old-1);assert(loaded.Undo());
        const auto fresh=loaded.Duplicate(0);assert(loaded.Parts()[fresh].name!=discarded_name);assert(loaded.Undo());
        while(loaded.Undo()){}assert(loaded.Save()==ReadFile(path) && !loaded.Changed());
        // Source edits after capture cannot change a saved session's baseline.
        Write(path,Fixture());Same(d,Loaded(reopened));Write(path,EnemyFixture());
        project.Save(file);assert(Project::Open(file).Bytes()==project.Bytes());
        const auto other=root/"notes.txt";Write(other,{1,2,3});Reject([&]{project.Save(other);});assert(ReadFile(other)==std::vector<std::uint8_t>({1,2,3}));
        Reject([&]{project.Save(game/"session.bbmapproject");});
        fs::create_directory_symlink(game,root/"game-link");Reject([&]{project.Save(root/"game-link/session.bbmapproject");});
        fs::create_symlink(file,root/"project-link");Reject([&]{project.Save(root/"project-link");});assert(Project::Open(file).Bytes()==project.Bytes());
        const auto corrupt=root/"corrupt.bbmapproject";auto damaged=project.Bytes();damaged.back()^=1;Write(corrupt,damaged);Reject([&]{project.Save(corrupt);});assert(ReadFile(corrupt)==damaged);
        const auto bytes=project.Bytes();for(std::size_t n:{std::size_t(0),std::size_t(8),bytes.size()/2,bytes.size()-1})Reject([&]{Project::Read(std::span(bytes).first(n));});
        auto bad=bytes;bad[bytes.size()/2]^=1;Reject([&]{Project::Read(bad);});
        bad=bytes;Put<std::uint32_t>(bad,12,0xffffffff);Checksum(bad);Reject([&]{Project::Read(bad);});
        // Corrupt a history record offset while retaining a valid outer checksum.
        std::vector<std::uint8_t> needle(4);Put<std::uint32_t>(needle,0,5);for(char c:std::string("c3030"))needle.push_back(c);
        auto match=bytes.begin();bool found=false;
        while((match=std::search(match,bytes.end(),needle.begin(),needle.end()))!=bytes.end()){
            const auto at=std::size_t(match-bytes.begin())+needle.size();std::uint64_t record=0;if(at+8<bytes.size())std::memcpy(&record,bytes.data()+at,8);
            if(record==0x600){bad=bytes;Put<std::uint64_t>(bad,at,UINT64_MAX);found=true;break;}++match;
        }
        assert(found);Checksum(bad);
        Reject([&]{auto p=Project::Read(bad);Loaded(p);});
        auto invalid_cursor=bytes;Put<std::uint64_t>(invalid_cursor,invalid_cursor.size()-12,2);Checksum(invalid_cursor);Reject([&]{Loaded(Project::Read(invalid_cursor));}); // inside a grouped duplicate+move
        // Failure leaves the caller's document intact.
        auto original=Document::Open(path);const auto untouched=original.Save();Reject([&]{Project::Read(bad).Restore(original);});assert(original.Save()==untouched && !original.Changed());
        std::cout<<"PASS: project ownership, grouped undo/redo and redo tail, baseline independence, allocator reservations, atomic replacement and malformed/path guards\n";
        if(argc>1){
            const auto actual=fs::path(argv[1]);const auto name=std::string("m21_00_00_00");const auto source=actual/"dvdroot_ps4/map/MapStudio"/(name+".msb.dcx");auto real=Document::Open(source);
            auto params=std::make_shared<const GameParams>(GameParams::Open(actual));real.AttachParameters(params);
            auto defs=std::make_shared<const EventDefinitions>(EventDefinitions::Read(ReadFile(actual/"dvdroot_ps4/event/common.emedf.dcx")));
            for(const auto& ev:{std::string("common.emevd.dcx"),name+".emevd.dcx"})real.AttachEvents(ev,std::make_shared<const EventFile>(EventFile::Open(actual/"dvdroot_ps4/event"/ev,defs)));
            const auto pickup=std::ranges::find(real.Treasures(),std::string("宝死体_01"),&Treasure::name);assert(pickup!=real.Treasures().end());const auto treasure=real.DuplicateTreasure(std::size_t(pickup-real.Treasures().begin()));real.SetItemContents(treasure,{4,1000,3});
            real.AddPickupQuest(treasure,{4,1000,5},name+".emevd.dcx");
            const auto& events=real.Scripts().at(name+".emevd.dcx")->Events();const auto event=std::ranges::find(events,std::int64_t(12100000),&ScriptEvent::id);assert(event!=events.end());
            real.SetEventArgument(name+".emevd.dcx",std::size_t(event-events.begin()),12,0,4);assert(real.Undo());
            auto p=Project::Read(Project::Capture(actual,name,real).Bytes());auto r=Loaded(p);Same(real,r);History(real,r);assert(r.Redo());
            assert(r.Undo());real.Export(root/"before",name);r.Export(root/"after",name);
            for(const auto& entry:fs::recursive_directory_iterator(root/"before"))if(entry.is_regular_file()){
                const auto relative=fs::relative(entry.path(),root/"before");assert(ReadFile(entry.path())==ReadFile(root/"after"/relative));
            }
            r.RemoveQuest(0);r.RemoveTreasure(treasure);assert(r.Quests().empty() && r.ItemLots().empty());
            while(r.Undo()){}assert(r.Save()==ReadFile(source) && !r.Changed());
            std::cout<<"PASS: real pickup/item/quest/event project history, retained new-copy ownership and byte-identical mod exports\n";
        }
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
