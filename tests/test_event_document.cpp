// SPDX-License-Identifier: GPL-2.0-or-later
#include "event_document.h"
#include "game_params.h"
#include "map_document.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace BbMapEditor;
template<class F> void Reject(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}assert(rejected);}
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value){assert(at+sizeof(T)<=b.size());std::memcpy(b.data()+at,&value,sizeof(T));}
std::vector<std::uint8_t> DefinitionsFixture() {
    std::vector<std::uint8_t> b(416);std::memcpy(b.data(),"EDF\0",4);b[5]=0xff;Put<std::uint16_t>(b,8,101);Put<std::uint16_t>(b,10,204);
    const std::int64_t header[]={1,168,1,200,2,232,0,360,0,360,0,360,1,360,2,384,0,416,0};std::memcpy(b.data()+16,header,sizeof(header));
    Put<std::int64_t>(b,168,2003);Put<std::int64_t>(b,176,1);Put<std::int64_t>(b,200,2);Put<std::int64_t>(b,208,2);
    auto text=[&](const std::u16string& s){const auto at=b.size()-416;for(auto c:s){b.push_back(std::uint8_t(c));b.push_back(std::uint8_t(c>>8));}b.push_back(0);b.push_back(0);return std::int64_t(at);};
    Put<std::int64_t>(b,224,text(u"Set flag"));Put<std::int64_t>(b,232,text(u"Flag"));Put<std::int32_t>(b,240,5);Put<std::int32_t>(b,248,-1);
    Put<std::int64_t>(b,296,text(u"State"));Put<std::int64_t>(b,360,text(u"State enum"));Put<std::int64_t>(b,368,2);
    Put<std::int64_t>(b,384,text(u"Off"));Put<std::int64_t>(b,400,text(u"On"));Put<std::int64_t>(b,408,1);
    Put<std::int64_t>(b,16+16*8,b.size()-416);Put<std::uint32_t>(b,12,b.size());return b;
}
std::vector<std::uint8_t> EventFixture() {
    std::vector<std::uint8_t> b(0x118);std::memcpy(b.data(),"EVD\0",4);b[5]=0xff;Put<std::int32_t>(b,8,204);Put<std::uint32_t>(b,12,b.size());
    const std::int64_t header[]={1,0x90,1,0xc0,0,0xe0,0,0x100,1,0xe0,0,0x110,8,0x110,0,0x118};std::memcpy(b.data()+16,header,sizeof(header));
    Put<std::int64_t>(b,0x98,1);Put<std::int64_t>(b,0xa8,1);Put<std::int32_t>(b,0xc0,2003);Put<std::int32_t>(b,0xc4,2);Put<std::int64_t>(b,0xc8,8);Put<std::int32_t>(b,0xd8,-1);
    Put<std::int32_t>(b,0xf8,4);Put<std::int32_t>(b,0x110,1000);return b;
}
int main(int argc,char** argv) {
    Reject([]{EventFile::Read({});});Reject([]{EventDefinitions::Read({});});
    assert(ValidEventName("m21_00_00_00.emevd.dcx") && ValidEventName("common.emevd.dcx"));assert(!ValidEventName("../common.emevd.dcx"));
    {
        const auto input=EventFixture();auto defs=std::make_shared<const EventDefinitions>(EventDefinitions::Read(DefinitionsFixture()));auto file=EventFile::Read(input,defs);
        assert(file.Save({},{})==input && file.Events().size()==1);assert(file.Parameterized(0,0,defs->Find(2003,2)->arguments[0]));
        Reject([&]{file.Edit(0,0,0,1001);});Reject([&]{file.Edit(0,0,1,2);});Reject([&]{file.Edit(0,0,1,256);});
        auto edit=file.Edit(0,0,1,1);auto changed=EventFile::Read(file.Save({edit},{}),defs);assert(EventFile::Number(changed.Operand(0,0,1))==1);
        PickupQuest q{"m21_00_00_00.emevd.dcx","Pickup",12109000,52100900,52100901,900000010};auto quest=EventFile::Read(file.Save({}, {q}),defs);
        assert(quest.Events().size()==2 && quest.Events()[0].instructions.size()==2 && quest.Events()[1].instructions.size()==4);
        auto bad=input;Put<std::int64_t>(bad,0xc8,100000);Reject([&]{EventFile::Read(bad,defs);});
        bad=input;Put<std::int64_t>(bad,0xe8,8);Reject([&]{EventFile::Read(bad,defs);});
    }
    if(argc>1) {
        const auto root=std::filesystem::path(argv[1])/"dvdroot_ps4/event";
        auto defs=std::make_shared<const EventDefinitions>(EventDefinitions::Read(ReadFile(root/"common.emedf.dcx")));
        assert(defs->Count()==287);const auto* flag=defs->Find(3,0);assert(flag && flag->arguments.size()==4);
        assert(flag->arguments[3].offset==4 && flag->arguments[3].width==4 && flag->arguments[1].choices.size()==3);
        std::size_t files=0,protected_fields=0,editable_fields=0;
        for(const auto& entry:std::filesystem::directory_iterator(root)) {
            if(!entry.path().filename().string().ends_with(".emevd.dcx"))continue;
            const auto input=ReadFile(entry.path());auto file=EventFile::Open(entry.path(),defs);assert(file.Save({},{})==input);++files;
            bool checked=false;
            for(std::size_t e=0;e<file.Events().size();++e)for(std::size_t i=0;i<file.Events()[e].instructions.size();++i) {
                const auto& instruction=file.Events()[e].instructions[i];const auto* command=file.Command(instruction);if(!command)continue;
                for(std::size_t a=0;a<command->arguments.size();++a) {
                    const auto& arg=command->arguments[a];if(!arg.width || arg.offset>instruction.length || arg.width>instruction.length-arg.offset)continue;
                    const auto operand=file.Operand(e,i,a);const auto number=EventFile::Number(operand);
                    if(file.Parameterized(e,i,arg)){++protected_fields;if(protected_fields==1)Reject([&]{file.Edit(e,i,a,number);});continue;}
                    ++editable_fields;
                    if(!checked && arg.type!=6 && arg.choices.empty() && number>=0 && number<100000000) {
                        auto edit=file.Edit(e,i,a,number+1);auto output=file.Save({edit},{});const auto before=UnpackAsset(input),after=UnpackAsset(output);
                        assert(before.size()==after.size());for(std::size_t p=0;p<before.size();++p)if(p<edit.at || p>=edit.at+edit.width)assert(before[p]==after[p]);
                        assert(EventFile::Read(output,defs).Events().size()==file.Events().size());checked=true;
                    }
                }
            }
        }
        assert(files>20 && protected_fields>0 && editable_fields>0);
        const auto input=ReadFile(root/"m21_00_00_00.emevd.dcx");auto dream=EventFile::Open(root/"m21_00_00_00.emevd.dcx",defs);
        PickupQuest quest{"m21_00_00_00.emevd.dcx","Pickup",12109999,52100900,52100901,900000010};
        while(std::ranges::find(dream.Events(),std::int64_t(quest.event_id),&ScriptEvent::id)!=dream.Events().end())--quest.event_id;
        const auto output=dream.Save({}, {quest});auto reopened=EventFile::Read(output,defs);assert(reopened.Events().size()==dream.Events().size()+1);
        const auto old=UnpackAsset(input),now=UnpackAsset(output);
        for(std::size_t e=0;e<dream.Events().size();++e) {
            const auto& a=dream.Events()[e];const auto& b=reopened.Events()[e];assert(a.id==b.id && a.parameters.size()==b.parameters.size());
            assert(b.instructions.size()==a.instructions.size()+(a.id==0?1:0));
            for(std::size_t i=0;i<a.instructions.size();++i){const auto& x=a.instructions[i];const auto& y=b.instructions[i];assert(x.bank==y.bank && x.id==y.id && x.length==y.length);assert(std::equal(old.begin()+x.args,old.begin()+x.args+x.length,now.begin()+y.args));}
        }
        const auto& added=reopened.Events().back();assert(added.id==quest.event_id && added.instructions.size()==4);
        assert(added.instructions[0].bank==3 && added.instructions[2].bank==2003 && added.instructions[2].id==4);
        auto bad=quest;bad.completion_flag=bad.trigger_flag;Reject([&]{dream.Save({}, {bad});});
        const auto game=std::filesystem::path(argv[1]);const auto map_path=game/"dvdroot_ps4/map/MapStudio/m21_00_00_00.msb.dcx";
        auto map=Document::Open(map_path);map.AttachParameters(std::make_shared<const GameParams>(GameParams::Open(game)));
        map.AttachEvents("m21_00_00_00.emevd.dcx",std::make_shared<const EventFile>(dream));
        const auto delay=std::ranges::find(dream.Events(),std::int64_t(12100000),&ScriptEvent::id);const auto e=std::size_t(delay-dream.Events().begin());
        assert(EventFile::Number(map.EventArgumentValue("m21_00_00_00.emevd.dcx",e,12,0))==3);
        map.SetEventArgument("m21_00_00_00.emevd.dcx",e,12,0,4);assert(map.Changed() && map.Save()==ReadFile(map_path));
        assert(map.Undo() && !map.Changed());assert(map.Redo());assert(map.Undo());
        const auto pickup=std::ranges::find(map.Treasures(),std::string("宝死体_01"),&Treasure::name);const auto copy=map.DuplicateTreasure(std::size_t(pickup-map.Treasures().begin()));map.SetItemContents(copy,{4,1000,3});
        const auto lots=map.ItemLots().size();const auto q=map.AddPickupQuest(copy,{4,1000,5},"m21_00_00_00.emevd.dcx");assert(map.Quests().size()==1 && map.ItemLots().size()==lots+1);
        assert(map.Quests()[q].trigger_flag!=map.Quests()[q].completion_flag && map.ItemLots().back().Flag()==-1);
        Reject([&]{map.RemoveTreasure(copy);});assert(map.Undo() && map.Quests().empty() && map.ItemLots().size()==lots);assert(map.Redo());
        map.RemoveQuest(q);assert(map.Quests().empty() && map.ItemLots().size()==lots);assert(map.Undo());
        while(map.Undo()){}assert(!map.Changed() && map.Save()==ReadFile(map_path));
        if(argc>2){std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(output.data()),output.size());}
        std::cout<<"Checked "<<files<<" real event files; "<<editable_fields<<" typed constants; "<<protected_fields<<" parameter-fed fields protected\n";
    }
    std::cout<<"PASS: event metadata/names, exact round trips, bounded typed edits, parameter protection and preserved instructions with pickup quest\n";
}
