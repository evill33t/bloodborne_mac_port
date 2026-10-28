// SPDX-License-Identifier: GPL-2.0-or-later
// Retail BB EMEVD/EMELD layouts follow Smithbox/SoulsFormats (MIT).
// Command argument types and names come from the user's common.emedf.dcx.
#include "event_document.h"
#include "game_params.h"
#include "map_document.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace BbMapEditor {
namespace {
template<class T> T Get(std::span<const std::uint8_t> b,std::size_t at) {
    if(at>b.size() || sizeof(T)>b.size()-at)throw std::runtime_error("Truncated event data");T value;std::memcpy(&value,b.data()+at,sizeof(T));return value;
}
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value) {
    if(at>b.size() || sizeof(T)>b.size()-at)throw std::runtime_error("Invalid event write offset");std::memcpy(b.data()+at,&value,sizeof(T));
}
void Range(std::span<const std::uint8_t> b,std::int64_t at,std::int64_t count,std::size_t stride=1) {
    if(at<0 || count<0 || std::uint64_t(at)>b.size() || std::uint64_t(count)>(b.size()-std::size_t(at))/stride)throw std::runtime_error("Invalid event table bounds");
}
std::string Text(std::span<const std::uint8_t> b,std::size_t at) {
    std::string s;
    for(int n=0;n<8192;++n,at+=2) {
        std::uint32_t c=Get<std::uint16_t>(b,at);if(!c)return s;
        if(c>=0xd800 && c<=0xdbff){auto low=Get<std::uint16_t>(b,at+2);if(low<0xdc00 || low>0xdfff)throw std::runtime_error("Invalid event UTF-16");c=0x10000+((c-0xd800)<<10)+(low-0xdc00);at+=2;++n;}
        else if(c>=0xdc00 && c<=0xdfff)throw std::runtime_error("Invalid event UTF-16");
        if(c<128)s+=char(c);else {if(c>=0x10000){s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));}else if(c>=0x800)s+=char(0xe0|(c>>12));if(c>=0x800)s+=char(0x80|((c>>6)&63));else s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}
    }
    throw std::runtime_error("Unterminated event name");
}
std::size_t Width(int type) {return type==0 || type==3?1:type==1 || type==4?2:type==2 || type==5 || type==6?4:0;}
bool Header(std::span<const std::uint8_t> b,const char* magic) {
    return b.size()>=16 && !std::memcmp(b.data(),magic,4) && !b[4] && b[5]==0xff && !b[6] && !b[7];
}
const char* Alias(int bank,int id) {
    if(bank==2000 && id==0)return "Initialize event";
    if(bank==2003 && id==2)return "Set event flag";
    if(bank==2003 && id==4)return "Award item lot";
    if(bank==3 && id==0)return "Wait/check event flag";
    if(bank==0 && id==0)return "Wait/check condition group";
    return "";
}
}
EventDefinitions EventDefinitions::Read(std::span<const std::uint8_t> file) {
    const auto raw=UnpackAsset(file);const std::span<const std::uint8_t> b=raw;
    if(!Header(b,"EDF\0") || b.size()<168 || Get<std::uint16_t>(b,8)!=101 || Get<std::uint16_t>(b,10)!=204)throw std::runtime_error("Expected Bloodborne EMEDF definitions");
    const auto classes=Get<std::int64_t>(b,16),class_at=Get<std::int64_t>(b,24),instructions=Get<std::int64_t>(b,32),instruction_at=Get<std::int64_t>(b,40);
    const auto args=Get<std::int64_t>(b,48),arg_at=Get<std::int64_t>(b,56),strings=Get<std::int64_t>(b,16+16*8),string_at=Get<std::int64_t>(b,16+17*8);
    Range(b,class_at,classes,32);Range(b,instruction_at,instructions,32);Range(b,arg_at,args,64);Range(b,string_at,strings);
    if(classes>1000 || instructions>10000 || args>100000)throw std::runtime_error("Event definition count exceeds limit");
    auto name=[&](std::int64_t relative){if(relative<0 || relative>=strings)throw std::runtime_error("Invalid event definition string");return Text(b.first(string_at+strings),string_at+relative);};
    const auto enum_count=Get<std::int64_t>(b,16+12*8),enum_at=Get<std::int64_t>(b,16+13*8),value_count=Get<std::int64_t>(b,16+14*8),value_at=Get<std::int64_t>(b,16+15*8);
    Range(b,enum_at,enum_count,24);Range(b,value_at,value_count,16);
    EventDefinitions result;
    for(std::int64_t c=0;c<classes;++c) {
        const auto at=class_at+c*32;const auto bank=Get<std::int64_t>(b,at),count=Get<std::int64_t>(b,at+8),offset=Get<std::int64_t>(b,at+16);
        if(bank<0 || bank>INT32_MAX || offset<0 || count<0 || offset/32>instructions || count>instructions-offset/32 || offset%32)throw std::runtime_error("Invalid command class");
        for(std::int64_t n=0;n<count;++n) {
            const auto p=instruction_at+offset+n*32;const auto id=Get<std::int64_t>(b,p),argc=Get<std::int64_t>(b,p+8),arg_offset=Get<std::int64_t>(b,p+16);
            if(id<0 || id>INT32_MAX || argc<0 || arg_offset<0 || arg_offset%64 || arg_offset/64>args || argc>args-arg_offset/64)throw std::runtime_error("Invalid command definition");
            EventCommand command;command.name=name(Get<std::int64_t>(b,p+24));const std::string alias=Alias(int(bank),int(id));if(!alias.empty())command.name=alias+" / "+command.name;
            std::size_t cursor=0;bool known=true;
            for(std::int64_t a=0;a<argc;++a) {
                const auto field=arg_at+arg_offset+a*64;EventArgument arg;arg.name=name(Get<std::int64_t>(b,field));arg.type=Get<std::int32_t>(b,field+8);arg.width=known?Width(arg.type):0;
                const auto enumeration=Get<std::int32_t>(b,field+16);
                if(enumeration!=-1) {
                    if(enumeration<0 || enumeration%24 || enumeration/24>=enum_count)throw std::runtime_error("Invalid command enumeration");
                    const auto count=Get<std::int64_t>(b,enum_at+enumeration+8),offset=Get<std::int64_t>(b,enum_at+enumeration+16);
                    if(count<0 || offset<0 || offset%16 || offset/16>value_count || count>value_count-offset/16)throw std::runtime_error("Invalid enumeration values");
                    for(std::int64_t v=0;v<count;++v){const auto at=value_at+offset+v*16;arg.choices[Get<std::int64_t>(b,at+8)]=name(Get<std::int64_t>(b,at));}
                }
                if(arg.width){cursor=(cursor+arg.width-1)&~(arg.width-1);arg.offset=cursor;cursor+=arg.width;}else known=false;
                command.arguments.push_back(std::move(arg));
            }
            if(!result.commands.emplace(std::make_pair(int(bank),int(id)),std::move(command)).second)throw std::runtime_error("Duplicate command definition");
        }
    }
    return result;
}
const EventCommand* EventDefinitions::Find(int bank,int id) const {auto found=commands.find({bank,id});return found==commands.end()?nullptr:&found->second;}
bool ValidEventName(const std::string& name) {return name=="common.emevd.dcx" || (name.size()==22 && ValidMapName(name.substr(0,12)) && name.substr(12)==".emevd.dcx");}
EventFile EventFile::Read(std::span<const std::uint8_t> file,std::shared_ptr<const EventDefinitions> defs) {
    EventFile result;result.source.assign(file.begin(),file.end());result.raw=UnpackAsset(file);result.definitions=std::move(defs);const std::span<const std::uint8_t> b=result.raw;
    if(!Header(b,"EVD\0") || b.size()<144 || Get<std::int32_t>(b,8)!=204 || b.size()>64*1024*1024)throw std::runtime_error("Expected Bloodborne EMEVD");
    std::array<std::int64_t,16> h;std::memcpy(h.data(),b.data()+16,sizeof(h));
    for(int i=0;i<16;i+=2)Range(b,h[i+1],h[i],i==0?48:i==2 || i==8?32:i==10?8:1);
    if(h[0]>100000 || h[2]>1000000 || h[4]!=0 || h[8]>1000000)throw std::runtime_error("Unsupported event table counts");
    for(std::int64_t n=0;n<h[0];++n) {
        ScriptEvent event;event.record=h[1]+n*48;event.id=Get<std::int64_t>(b,event.record);
        const auto count=Get<std::int64_t>(b,event.record+8),offset=Get<std::int64_t>(b,event.record+16);
        if(count<0 || count>h[2] || (count && (offset<0 || offset%32 || offset/32>h[2] || count>h[2]-offset/32)))throw std::runtime_error("Invalid event instructions");
        for(std::int64_t i=0;i<count;++i) {
            EventInstruction instruction;instruction.record=h[3]+offset+i*32;instruction.bank=Get<std::int32_t>(b,instruction.record);instruction.id=Get<std::int32_t>(b,instruction.record+4);
            const auto length=Get<std::int64_t>(b,instruction.record+8),arg_offset=Get<std::int64_t>(b,instruction.record+16);
            if(length<0 || length>h[12] || (length && (arg_offset<0 || arg_offset>h[12] || length>h[12]-arg_offset)))throw std::runtime_error("Invalid instruction arguments");
            instruction.length=std::size_t(length);instruction.args=length?std::size_t(h[13]+arg_offset):0;event.instructions.push_back(instruction);
        }
        const auto parameters=Get<std::int64_t>(b,event.record+24);const auto parameter_offset=Get<std::int32_t>(b,event.record+32);
        if(parameters<0 || parameters>h[8] || (parameters && (parameter_offset<0 || parameter_offset%32 || parameter_offset/32>h[8] || parameters>h[8]-parameter_offset/32)))throw std::runtime_error("Invalid event parameters");
        for(std::int64_t i=0;i<parameters;++i) {
            const auto p=h[9]+parameter_offset+i*32;const auto instruction=Get<std::int64_t>(b,p),at=Get<std::int64_t>(b,p+8),source=Get<std::int64_t>(b,p+16);const auto length=Get<std::int32_t>(b,p+24);
            if(instruction<0 || std::uint64_t(instruction)>=event.instructions.size() || at<0 || source<0 || source>INT32_MAX || length<0 || std::uint64_t(at)>event.instructions[instruction].length || std::uint64_t(length)>event.instructions[instruction].length-at)throw std::runtime_error("Invalid parameter destination");
            event.parameters.push_back({std::size_t(instruction),std::size_t(at),std::size_t(length),std::size_t(source)});
        }
        result.events.push_back(std::move(event));
    }
    return result;
}
EventFile EventFile::Open(const std::filesystem::path& path,std::shared_ptr<const EventDefinitions> defs) {
    auto result=Read(ReadFile(path),std::move(defs));auto names=path;names.replace_filename(path.filename().string().substr(0,path.filename().string().size()-10)+".emeld.dcx");
    if(!std::filesystem::exists(names))return result;const auto raw=UnpackAsset(ReadFile(names));const std::span<const std::uint8_t> b=raw;
    if(!Header(b,"ELD\0") || b.size()<80)throw std::runtime_error("Expected Bloodborne EMELD names");
    const auto count=Get<std::int64_t>(b,16),at=Get<std::int64_t>(b,24),length=Get<std::int64_t>(b,64),strings=Get<std::int64_t>(b,72);Range(b,at,count,16);Range(b,strings,length);
    std::map<std::int64_t,std::string> labels;
    for(std::int64_t n=0;n<count;++n){const auto relative=Get<std::int64_t>(b,at+n*16+8);if(relative<0 || relative>=length)throw std::runtime_error("Invalid event name offset");labels[Get<std::int64_t>(b,at+n*16)]=Text(b.first(strings+length),strings+relative);}
    for(auto& event:result.events)if(labels.contains(event.id))event.name=labels.at(event.id);return result;
}
const EventCommand* EventFile::Command(const EventInstruction& instruction) const {return definitions?definitions->Find(instruction.bank,instruction.id):nullptr;}
bool EventFile::Parameterized(std::size_t e,std::size_t i,const EventArgument& arg) const {
    const auto& event=events.at(e);return std::ranges::any_of(event.parameters,[&](const auto& p){return p.instruction==i && p.at<arg.offset+arg.width && arg.offset<p.at+p.length;});
}
ScriptOperand EventFile::Operand(std::size_t e,std::size_t i,std::size_t a) const {
    const auto& instruction=events.at(e).instructions.at(i);const auto* command=Command(instruction);if(!command)throw std::runtime_error("Command definition unavailable");const auto& arg=command->arguments.at(a);
    if(!arg.width || arg.offset>instruction.length || arg.width>instruction.length-arg.offset)throw std::runtime_error("Unsupported argument layout");
    ScriptOperand result;result.at=instruction.args+arg.offset;result.width=arg.width;result.type=arg.type;std::memcpy(&result.original,raw.data()+result.at,result.width);result.value=result.original;return result;
}
double EventFile::Number(const ScriptOperand& arg) {
    if(arg.type==6){float f;std::memcpy(&f,&arg.value,4);return f;}
    if(arg.type==3)return std::int8_t(arg.value);if(arg.type==4)return std::int16_t(arg.value);if(arg.type==5)return std::int32_t(arg.value);return arg.value;
}
ScriptOperand EventFile::Edit(std::size_t e,std::size_t i,std::size_t a,double value) const {
    auto result=Operand(e,i,a);if(Parameterized(e,i,Command(events.at(e).instructions.at(i))->arguments.at(a)))throw std::runtime_error("This argument is supplied by event parameters; edit its caller instead");
    if(!std::isfinite(value))throw std::runtime_error("Event argument must be finite");
    if(result.type==6){if(std::abs(value)>1e9)throw std::runtime_error("Event float exceeds editor limit");const float f=float(value);std::memcpy(&result.value,&f,4);}
    else {
        const bool sign=result.type>=3 && result.type<=5;const double minimum=sign?-std::pow(2.,result.width*8-1):0,maximum=std::pow(2.,result.width*8-(sign?1:0))-1;
        if(value!=std::trunc(value) || value<minimum || value>maximum)throw std::runtime_error("Event integer is outside its encoded type");result.value=std::uint32_t(std::int64_t(value));if(result.width<4)result.value&=(1u<<(result.width*8))-1;
    }
    const auto& choices=Command(events.at(e).instructions.at(i))->arguments.at(a).choices;
    if(result.value!=result.original && !choices.empty() && !choices.contains(std::int64_t(Number(result))))throw std::runtime_error("Choose a value defined by this argument's enumeration");
    return result;
}
std::vector<std::uint8_t> EventFile::Save(const std::vector<ScriptOperand>& edits,const std::vector<PickupQuest>& quests) const {
    if(edits.empty() && quests.empty())return source;auto b=raw;
    for(const auto& edit:edits){if(edit.width!=1 && edit.width!=2 && edit.width!=4)throw std::runtime_error("Invalid event edit width");Range(b,edit.at,edit.width);std::uint32_t original=0;std::memcpy(&original,b.data()+edit.at,edit.width);if(original!=edit.original)throw std::runtime_error("Event edit source mismatch");std::memcpy(b.data()+edit.at,&edit.value,edit.width);}
    if(!quests.empty()) {
        const auto constructor=std::ranges::find(events,std::int64_t(0),&ScriptEvent::id);if(constructor==events.end())throw std::runtime_error("Area constructor event is missing");
        std::set<std::int64_t> ids;for(const auto& event:events)ids.insert(event.id);
        const auto arg_size=Get<std::int64_t>(b,16+12*8),arg_at=Get<std::int64_t>(b,16+13*8);std::vector<std::uint8_t> arguments(b.begin()+arg_at,b.begin()+arg_at+arg_size),instructions;
        struct EventRecord {std::vector<std::uint8_t> bytes;};std::vector<EventRecord> records;
        auto instruction=[&](int bank,int id,std::vector<std::uint8_t> args) {
            const auto at=instructions.size();instructions.resize(at+32);Put<std::int32_t>(instructions,at,bank);Put<std::int32_t>(instructions,at+4,id);Put<std::int64_t>(instructions,at+8,args.size());Put<std::int64_t>(instructions,at+16,arguments.size());Put<std::int32_t>(instructions,at+24,-1);
            arguments.insert(arguments.end(),args.begin(),args.end());arguments.resize((arguments.size()+3)&~std::size_t(3),0);
        };
        for(const auto& event:events) {
            EventRecord record{{b.begin()+event.record,b.begin()+event.record+48}};Put<std::int64_t>(record.bytes,16,event.instructions.empty()?-1:instructions.size());
            for(const auto& i:event.instructions)instructions.insert(instructions.end(),b.begin()+i.record,b.begin()+i.record+32);
            if(event.id==0) {
                Put<std::int64_t>(record.bytes,8,event.instructions.size()+quests.size());if(event.instructions.empty())Put<std::int64_t>(record.bytes,16,instructions.size());
                for(const auto& quest:quests){std::vector<std::uint8_t> args(12);Put(args,4,quest.event_id);instruction(2000,0,std::move(args));}
            }
            records.push_back(std::move(record));
        }
        for(const auto& quest:quests) {
            if(quest.event_id<=0 || !ids.insert(quest.event_id).second || quest.trigger_flag<=0 || quest.completion_flag<=0 || quest.trigger_flag==quest.completion_flag || quest.reward_lot<0)throw std::runtime_error("Invalid pickup quest references");
            EventRecord record{std::vector<std::uint8_t>(48)};Put<std::int64_t>(record.bytes,0,quest.event_id);Put<std::int64_t>(record.bytes,8,4);Put<std::int64_t>(record.bytes,16,instructions.size());Put<std::int32_t>(record.bytes,32,-1);
            auto flag=[&](std::int32_t id,bool on){std::vector<std::uint8_t> args(8);args[1]=on;Put(args,4,id);instruction(3,0,std::move(args));};
            flag(quest.completion_flag,false);flag(quest.trigger_flag,true);std::vector<std::uint8_t> award(4);Put(award,0,quest.reward_lot);instruction(2003,4,std::move(award));
            std::vector<std::uint8_t> completed(8);Put(completed,0,quest.completion_flag);completed[4]=1;instruction(2003,2,std::move(completed));records.push_back(std::move(record));
        }
        b.resize((b.size()+7)&~std::size_t(7),0);Put<std::int64_t>(b,16,records.size());Put<std::int64_t>(b,24,b.size());for(const auto& r:records)b.insert(b.end(),r.bytes.begin(),r.bytes.end());
        Put<std::int64_t>(b,32,instructions.size()/32);Put<std::int64_t>(b,40,b.size());b.insert(b.end(),instructions.begin(),instructions.end());
        Put<std::int64_t>(b,16+12*8,arguments.size());Put<std::int64_t>(b,16+13*8,b.size());b.insert(b.end(),arguments.begin(),arguments.end());Put<std::uint32_t>(b,12,b.size());
    }
    const auto output=PackAsset(source,b);const auto verified=Read(output,definitions);
    if(verified.events.size()!=events.size()+quests.size())throw std::runtime_error("Event export verification failed");return output;
}
} // namespace BbMapEditor
