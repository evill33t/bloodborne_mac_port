// SPDX-License-Identifier: GPL-2.0-or-later
// Layout reference: Smithbox's MIT-licensed SoulsFormats BND4 and PARAM readers.
#include "game_params.h"
#include "map_document.h"
#include "miniz.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace BbMapEditor {
namespace {
template<class T> T Get(std::span<const std::uint8_t> b,std::size_t at) {
    if(at>b.size() || sizeof(T)>b.size()-at)throw std::runtime_error("Truncated parameter asset");
    T v;std::memcpy(&v,b.data()+at,sizeof(T));return v;
}
std::string Utf16(std::span<const std::uint8_t> b,std::size_t at) {
    std::string text;
    for(int n=0;n<8192;++n,at+=2) {
        std::uint32_t c=Get<std::uint16_t>(b,at);if(!c)return text;
        if(c>=0xd800 && c<=0xdbff) {
            const auto low=Get<std::uint16_t>(b,at+2);if(low<0xdc00 || low>0xdfff)throw std::runtime_error("Invalid parameter UTF-16");
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);at+=2;++n;
        } else if(c>=0xdc00 && c<=0xdfff)throw std::runtime_error("Invalid parameter UTF-16");
        if(c<0x80)text+=char(c);
        else {
            if(c>=0x10000){text+=char(0xf0|(c>>18));text+=char(0x80|((c>>12)&63));}
            else if(c>=0x800)text+=char(0xe0|(c>>12));
            if(c>=0x800)text+=char(0x80|((c>>6)&63));else text+=char(0xc0|(c>>6));
            text+=char(0x80|(c&63));
        }
    }
    throw std::runtime_error("Unterminated parameter name");
}
std::uint32_t BE(std::span<const std::uint8_t> b,std::size_t at) {
    std::uint32_t v=0;for(int i=0;i<4;++i)v=v<<8|Get<std::uint8_t>(b,at+i);return v;
}
std::string Basename(const std::string& path) {
    const auto at=path.find_last_of("/\\");return path.substr(at==std::string::npos?0:at+1);
}
}
std::vector<std::uint8_t> UnpackAsset(std::span<const std::uint8_t> b) {
    if(b.size()<4 || std::memcmp(b.data(),"DCX\0",4))return {b.begin(),b.end()};
    if(b.size()<0x4c || BE(b,4)!=0x10000 || BE(b,0x10)!=0x44 || BE(b,0x14)!=0x4c ||
       std::memcmp(b.data()+0x18,"DCS\0",4) || std::memcmp(b.data()+0x24,"DCP\0DFLT",8) ||
       std::memcmp(b.data()+0x44,"DCA\0",4) || BE(b,0x48)!=8 || BE(b,0x20)!=b.size()-0x4c ||
       !BE(b,0x1c) || BE(b,0x1c)>256*1024*1024)throw std::runtime_error("Unsupported parameter DCX envelope");
    std::vector<std::uint8_t> raw(BE(b,0x1c));mz_ulong size=raw.size();
    if(mz_uncompress(raw.data(),&size,b.data()+0x4c,b.size()-0x4c)!=MZ_OK || size!=raw.size())throw std::runtime_error("Parameter decompression failed");
    return raw;
}
std::vector<BinderFile> ReadBinder(std::span<const std::uint8_t> packed) {
    const auto raw=UnpackAsset(packed);const std::span<const std::uint8_t> b=raw;
    if(b.size()<64 || std::memcmp(b.data(),"BND4",4) || b[9] || b[10]!=1 || b[48]!=1 || b[49]!=0x74 ||
       Get<std::uint64_t>(b,16)!=64 || Get<std::uint64_t>(b,32)!=36)throw std::runtime_error("Unsupported Bloodborne binder");
    const auto count=Get<std::uint32_t>(b,12);if(count>100000 || count>(b.size()-64)/36)throw std::runtime_error("Invalid binder count");
    const auto data_start=Get<std::uint64_t>(b,40);if(data_start<64+std::size_t(count)*36 || data_start>b.size())throw std::runtime_error("Invalid binder data start");
    std::vector<BinderFile> files;
    for(std::size_t i=0;i<count;++i) {
        const auto at=64+i*36;const auto size=Get<std::uint64_t>(b,at+8),unpacked=Get<std::uint64_t>(b,at+16);
        const auto offset=Get<std::uint32_t>(b,at+24),name=Get<std::uint32_t>(b,at+32);
        if(Get<std::uint32_t>(b,at)!=0x40 || size!=unpacked || offset<data_start || offset>b.size() || size>b.size()-offset || name<64+std::size_t(count)*36 || name>=data_start)
            throw std::runtime_error("Unsupported or invalid binder entry");
        files.push_back({Utf16(b.first(data_start),name),Get<std::int32_t>(b,at+28),at,{b.begin()+offset,b.begin()+offset+size}});
    }
    return files;
}
ParamTable ParamTable::Read(std::span<const std::uint8_t> b) {
    if(b.size()<64 || b[0x2c] || b[0x2d]!=4 || b[0x2e]!=7)throw std::runtime_error("Unsupported Bloodborne parameter format");
    ParamTable result;result.bytes.assign(b.begin(),b.end());
    const auto type_end=std::find(b.begin()+12,b.begin()+44,0);if(type_end==b.begin()+44)throw std::runtime_error("Invalid parameter type");
    result.type.assign(b.begin()+12,type_end);
    const auto count=Get<std::uint16_t>(b,10);if(count>(b.size()-64)/24)throw std::runtime_error("Invalid parameter row count");
    const auto strings=Get<std::uint32_t>(b,0);const auto start=Get<std::uint64_t>(b,48);
    if(start<64+std::size_t(count)*24 || start>strings || strings>b.size())throw std::runtime_error("Invalid parameter data bounds");
    std::set<std::size_t> offsets{strings};
    for(std::size_t i=0;i<count;++i) {
        const auto at=64+i*24;const auto id=Get<std::int32_t>(b,at);const auto data=Get<std::uint64_t>(b,at+8),name=Get<std::uint64_t>(b,at+16);
        // Retail BB contains duplicate row IDs in CharaInit, NpcThink and ItemLot.
        // Keep all rows in their original order; lookup returns the first matching row.
        if(data<start || data>=strings || (name && (name<strings || name>=b.size())) || !offsets.insert(data).second)throw std::runtime_error("Invalid parameter row");
        result.rows.push_back({id,name?Utf16(b,name):std::string(),std::size_t(data),0});
    }
    for(auto& row:result.rows)row.size=*offsets.upper_bound(row.offset)-row.offset;
    return result;
}
const ParamRow* ParamTable::Find(std::int32_t id) const {
    const auto found=std::ranges::find(rows,id,&ParamRow::id);return found==rows.end()?nullptr:&*found;
}
std::span<const std::uint8_t> ParamTable::Data(const ParamRow& row) const {return std::span(bytes).subspan(row.offset,row.size);}
std::optional<NpcInfo> ReadNpcInfo(const ParamTable& table,std::int32_t id) {
    const auto* row=table.Find(id);
    // BB NPC_PARAM_ST data version 5, 388-byte retail layout; other layouts stay opaque.
    if(!row || table.type!="NPC_PARAM_ST" || Get<std::int16_t>(table.bytes,8)!=5 || row->size!=388)return {};
    const auto data=table.Data(*row);return NpcInfo{Get<std::uint32_t>(data,32),data[302],data[303]};
}
GameParams GameParams::Read(std::span<const std::uint8_t> binder) {
    GameParams result;
    const std::set<std::string> wanted={"NpcParam","NpcThinkParam","CharaInitParam","ItemLotParam","EquipParamWeapon","EquipParamProtector","EquipParamAccessory","EquipParamGoods"};
    for(auto& file:ReadBinder(binder)) {
        const auto name=Basename(file.name);if(!name.ends_with(".param"))continue;const auto key=name.substr(0,name.size()-6);
        if(wanted.contains(key) && !result.tables.emplace(key,ParamTable::Read(file.bytes)).second)throw std::runtime_error("Duplicate parameter table");
    }
    return result;
}
GameParams GameParams::Open(const std::filesystem::path& game) {return Read(ReadFile(game/"dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx"));}
const ParamTable* GameParams::Table(const std::string& name) const {auto p=tables.find(name);return p==tables.end()?nullptr:&p->second;}
std::map<std::int32_t,std::string> GameParams::TalkScripts(const std::filesystem::path& game,const std::string& map) {
    if(!ValidMapName(map))throw std::runtime_error("Invalid talk map name");
    std::map<std::int32_t,std::string> result;const auto path=game/"dvdroot_ps4/script/talk"/(map.substr(0,6)+"_00_00.talkesdbnd.dcx");
    if(!std::filesystem::exists(path))return result;
    for(const auto& file:ReadBinder(ReadFile(path))) {
        const auto name=Basename(file.name);if(name.size()<6 || name[0]!='t' || !name.ends_with(".esd"))continue;
        const auto digits=name.substr(1,name.size()-5);if(digits.empty() || digits.find_first_not_of("0123456789")!=std::string::npos)continue;
        const auto value=std::stoll(digits);if(value<=std::numeric_limits<std::int32_t>::max())result.emplace(std::int32_t(value),name);
    }
    return result;
}
void ValidateCharacterReferences(const EnemyData& before,const EnemyData& after,const GameParams& params,
                                 const std::map<std::int32_t,std::string>& talks) {
    auto check=[&](const char* name,int old,int value) {
        if(old==value || value==-1)return;const auto* table=params.Table(name);
        if(!table || !table->Find(value))throw std::runtime_error(std::string(name)+" row not found: "+std::to_string(value));
    };
    check("NpcParam",before.npc_param,after.npc_param);check("NpcThinkParam",before.think_param,after.think_param);
    check("CharaInitParam",before.chara_init,after.chara_init);
    if(after.talk_id!=before.talk_id && after.talk_id!=0 && after.talk_id!=-1 && !talks.contains(after.talk_id))
        throw std::runtime_error("Talk ESD not found in this area's binder: "+std::to_string(after.talk_id));
    if((after.init_anim!=before.init_anim && after.init_anim<-1) || (after.damage_anim!=before.damage_anim && after.damage_anim<-1))
        throw std::runtime_error("Animation IDs must be -1 or nonnegative");
}
} // namespace BbMapEditor
