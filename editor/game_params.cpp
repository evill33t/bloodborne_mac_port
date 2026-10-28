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
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value) {
    if(at>b.size() || sizeof(T)>b.size()-at)throw std::runtime_error("Invalid parameter write offset");std::memcpy(b.data()+at,&value,sizeof(T));
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
    if(b.size()<0x4c || BE(b,4)!=0x10000 || !((BE(b,0x10)==0x44 && BE(b,0x14)==0x4c) || (BE(b,0x10)==0x24 && BE(b,0x14)==0x2c)) ||
       std::memcmp(b.data()+0x18,"DCS\0",4) || std::memcmp(b.data()+0x24,"DCP\0DFLT",8) ||
       std::memcmp(b.data()+0x44,"DCA\0",4) || BE(b,0x48)!=8 || BE(b,0x20)!=b.size()-0x4c ||
       !BE(b,0x1c) || BE(b,0x1c)>256*1024*1024)throw std::runtime_error("Unsupported parameter DCX envelope");
    std::vector<std::uint8_t> raw(BE(b,0x1c));mz_ulong size=raw.size();
    if(mz_uncompress(raw.data(),&size,b.data()+0x4c,b.size()-0x4c)!=MZ_OK || size!=raw.size())throw std::runtime_error("Parameter decompression failed");
    return raw;
}
std::vector<std::uint8_t> PackAsset(std::span<const std::uint8_t> envelope,std::span<const std::uint8_t> raw) {
    if(raw.size()>256*1024*1024)throw std::runtime_error("Asset exceeds compression limit");
    if(envelope.size()<4 || std::memcmp(envelope.data(),"DCX\0",4))return {raw.begin(),raw.end()};
    if(envelope.size()<0x4c)throw std::runtime_error("Truncated DCX envelope");
    std::vector<std::uint8_t> result(envelope.begin(),envelope.begin()+0x4c);mz_ulong size=mz_compressBound(raw.size());std::vector<std::uint8_t> compressed(size);
    if(mz_compress2(compressed.data(),&size,raw.data(),raw.size(),9)!=MZ_OK)throw std::runtime_error("Asset compression failed");
    for(int i=0;i<4;++i){result[0x1c+i]=std::uint8_t(raw.size()>>(24-i*8));result[0x20+i]=std::uint8_t(size>>(24-i*8));}
    result.insert(result.end(),compressed.begin(),compressed.begin()+size);return result;
}
std::vector<BinderFile> ReadBinder(std::span<const std::uint8_t> packed) {
    const auto raw=UnpackAsset(packed);const std::span<const std::uint8_t> b=raw;
    if(b.size()<64 || std::memcmp(b.data(),"BND4",4) || b[9] || b[10]!=1 || b[48]!=1 || b[49]!=0x74 ||
       Get<std::uint64_t>(b,16)!=64 || Get<std::uint64_t>(b,32)!=36)throw std::runtime_error("Unsupported Bloodborne binder");
    const auto count=Get<std::uint32_t>(b,12);if(count>100000 || count>(b.size()-64)/36)throw std::runtime_error("Invalid binder count");
    const auto data_start=Get<std::uint64_t>(b,40);if(data_start<64+std::size_t(count)*36 || data_start>b.size())throw std::runtime_error("Invalid binder data start");
    std::vector<BinderFile> files;std::uint64_t total=0;
    for(std::size_t i=0;i<count;++i) {
        const auto at=64+i*36;const auto size=Get<std::uint64_t>(b,at+8),unpacked=Get<std::uint64_t>(b,at+16);
        const auto offset=Get<std::uint32_t>(b,at+24),name=Get<std::uint32_t>(b,at+32);
        if(Get<std::uint32_t>(b,at)!=0x40 || size!=unpacked || offset<data_start || offset>b.size() || size>b.size()-offset || name<64+std::size_t(count)*36 || name>=data_start)
            throw std::runtime_error("Unsupported or invalid binder entry");
        if(size>256*1024*1024-total)throw std::runtime_error("Binder entry data exceeds limit");total+=size;
        files.push_back({Utf16(b.first(data_start),name),Get<std::int32_t>(b,at+28),at,{b.begin()+offset,b.begin()+offset+size}});
    }
    return files;
}
std::map<std::int32_t,std::string> ReadFmg(std::span<const std::uint8_t> b) {
    if(b.size()<40 || b[0] || b[1] || b[2]!=2 || b[3] || b[8]!=1 || b[9] || b[10] || b[11] || Get<std::uint32_t>(b,20)!=0xff)
        throw std::runtime_error("Unsupported Bloodborne message format");
    const auto groups=Get<std::uint32_t>(b,12),strings=Get<std::uint32_t>(b,16);const auto offsets=Get<std::uint64_t>(b,24);
    if(groups>(b.size()-40)/16 || offsets<40+std::uint64_t(groups)*16 || offsets>b.size() || strings>(b.size()-offsets)/8)throw std::runtime_error("Invalid message tables");
    std::map<std::int32_t,std::string> result;
    for(std::size_t n=0;n<groups;++n) {
        const auto at=40+n*16;const auto index=Get<std::int32_t>(b,at),first=Get<std::int32_t>(b,at+4),last=Get<std::int32_t>(b,at+8);
        const auto count=std::int64_t(last)-first+1;if(index<0 || count<0 || std::uint64_t(index)>strings || std::uint64_t(count)>strings-std::uint64_t(index))throw std::runtime_error("Invalid message group");
        for(std::int64_t i=0;i<count;++i) {
            const auto offset=Get<std::uint64_t>(b,offsets+(index+i)*8);if(!offset)continue;
            if(offset<offsets+std::uint64_t(strings)*8 || offset>=b.size())throw std::runtime_error("Invalid message string");
            result[std::int32_t(first+i)]=Utf16(b,offset);
        }
    }
    return result;
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
    GameParams result;result.source.assign(binder.begin(),binder.end());
    const std::set<std::string> wanted={"NpcParam","NpcThinkParam","CharaInitParam","ItemLotParam","EquipParamWeapon","EquipParamProtector","EquipParamAccessory","EquipParamGoods"};
    for(auto& file:ReadBinder(binder)) {
        const auto name=Basename(file.name);if(!name.ends_with(".param"))continue;const auto key=name.substr(0,name.size()-6);
        if(wanted.contains(key) && !result.tables.emplace(key,ParamTable::Read(file.bytes)).second)throw std::runtime_error("Duplicate parameter table");
    }
    return result;
}
GameParams GameParams::Open(const std::filesystem::path& game,std::span<const std::uint8_t> baseline) {
    auto result=baseline.empty()?Read(ReadFile(game/"dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx")):Read(baseline);
    try {
        std::filesystem::path messages;for(const auto* language:{"enggb","engus"}){const auto p=game/"dvdroot_ps4/msg"/language/"item.msgbnd.dcx";if(std::filesystem::exists(p)){messages=p;break;}}
        if(!messages.empty())for(const auto& file:ReadBinder(ReadFile(messages))) {
            const char* table=file.id==10?"EquipParamGoods":file.id==11?"EquipParamWeapon":file.id==12?"EquipParamProtector":file.id==13?"EquipParamAccessory":nullptr;
            if(!table || !result.tables.contains(table))continue;const auto names=ReadFmg(file.bytes);
            for(auto& row:result.tables[table].rows)if(const auto found=names.find(row.id);found!=names.end())row.display_name=found->second;
        }
    }catch(const std::exception& e){result.text_notice=e.what();}
    return result;
}
const ParamTable* GameParams::Table(const std::string& name) const {auto p=tables.find(name);return p==tables.end()?nullptr:&p->second;}
ItemLot ReadItemLot(const ParamTable& table,std::int32_t id) {
    const auto* row=table.Find(id);
    if(!row || table.type!="ITEMLOT_PARAM_ST" || Get<std::int16_t>(table.bytes,8)!=2 || row->size!=148)throw std::runtime_error("Unsupported or missing item lot");
    const auto data=table.Data(*row);return {id,id,row->name,{data.begin(),data.end()}};
}
std::int32_t ItemLot::Flag() const {return Get<std::int32_t>(bytes,128);}
void ItemLot::SetFlag(std::int32_t flag) {Put(bytes,128,flag);}
ItemContents ItemLot::First() const {return {Get<std::int32_t>(bytes,32),Get<std::int32_t>(bytes,0),Get<std::uint8_t>(bytes,138)};}
void GameParams::SetContents(ItemLot& lot,const ItemContents& contents) const {
    const char* name=contents.category==0?"EquipParamWeapon":contents.category==1?"EquipParamProtector":contents.category==4?"EquipParamGoods":nullptr;
    if(!name)throw std::runtime_error("Choose a weapon, armor or consumable item");
    const auto* table=Table(name);if(!table || !table->Find(contents.item))throw std::runtime_error("Item does not exist in this game's equipment table");
    if(contents.count<1 || contents.count>99 || (contents.category!=4 && contents.count!=1))throw std::runtime_error("Choose 1-99 consumables, or one equipment item");
    if(lot.bytes.size()!=148)throw std::runtime_error("Unsupported item lot layout");
    // A custom pickup gives exactly one selected item stack, without luck/cumulative rolls.
    const auto flag=lot.Flag();lot.bytes.assign(148,0);Put(lot.bytes,0,contents.item);Put(lot.bytes,32,contents.category);
    Put<std::uint16_t>(lot.bytes,64,1000);Put(lot.bytes,128,flag);Put<std::int32_t>(lot.bytes,132,-1);lot.bytes[138]=std::uint8_t(contents.count);
}
std::vector<std::int32_t> GameParams::UsedFlags(std::int32_t group) const {
    if(group<0 || group>std::numeric_limits<std::int32_t>::max()-1000)throw std::runtime_error("Invalid pickup flag group");
    const auto raw=UnpackAsset(source);std::set<std::int32_t> used;
    for(std::size_t i=0;i+4<=raw.size();++i){const auto flag=Get<std::int32_t>(raw,i);if(flag>=group && flag<group+1000)used.insert(flag);}
    return {used.begin(),used.end()};
}
std::vector<std::uint8_t> GameParams::SaveLots(const std::vector<ItemLot>& lots) const {
    if(lots.empty())return source;
    const auto* table=Table("ItemLotParam");if(!table)throw std::runtime_error("ItemLotParam is unavailable");
    const auto& old=table->bytes;const auto start=Get<std::uint64_t>(old,48);const auto strings=Get<std::uint32_t>(old,0);
    if(table->rows.size()+lots.size()>65535 || strings-start!=table->rows.size()*148)throw std::runtime_error("Unsupported item lot data layout");
    struct Row {const ParamRow* old=nullptr;const ItemLot* lot=nullptr;};std::vector<Row> ordered;
    std::set<std::int32_t> ids;for(const auto& r:table->rows){if(r.size!=148)throw std::runtime_error("Unsupported item lot row size");ordered.push_back({&r});ids.insert(r.id);}
    for(const auto& lot:lots) {
        if(lot.bytes.size()!=148 || !ids.insert(lot.id).second)throw std::runtime_error("Conflicting or invalid custom item lot");
        auto pos=std::ranges::find_if(ordered,[&](const Row& r){return (r.old?r.old->id:r.lot->id)>lot.id;});ordered.insert(pos,{nullptr,&lot});
    }
    const auto padding=start-(64+table->rows.size()*24);const auto data_start=64+ordered.size()*24+padding;
    std::vector<std::uint8_t> param(old.begin(),old.begin()+64);param.resize(data_start,0);
    std::copy(old.begin()+64+table->rows.size()*24,old.begin()+start,param.begin()+64+ordered.size()*24);
    Put<std::uint16_t>(param,10,ordered.size());Put<std::uint64_t>(param,48,data_start);
    for(std::size_t i=0;i<ordered.size();++i) {
        const auto& row=ordered[i];const auto at=64+i*24;
        if(row.old){const auto index=std::size_t(row.old-table->rows.data());std::copy_n(old.begin()+64+index*24,24,param.begin()+at);}
        else Put(param,at,row.lot->id);
        Put<std::uint64_t>(param,at+8,param.size());const auto bytes=row.old?table->Data(*row.old):std::span<const std::uint8_t>(row.lot->bytes);
        param.insert(param.end(),bytes.begin(),bytes.end());
    }
    const auto new_strings=param.size();Put<std::uint32_t>(param,0,new_strings);param.insert(param.end(),old.begin()+strings,old.end());
    for(std::size_t i=0;i<ordered.size();++i) {
        const auto& row=ordered[i];const auto at=64+i*24;
        if(row.old){const auto name=Get<std::uint64_t>(old,64+std::size_t(row.old-table->rows.data())*24+16);Put<std::uint64_t>(param,at+16,name?new_strings+name-strings:0);}
        else {Put<std::uint64_t>(param,at+16,param.size());const auto name="Editor item "+std::to_string(row.lot->id);for(unsigned char c:name){param.push_back(c);param.push_back(0);}param.push_back(0);param.push_back(0);}
    }
    auto binder=UnpackAsset(source);const auto files=ReadBinder(source);const auto entry=std::ranges::find_if(files,[](const auto& f){return Basename(f.name)=="ItemLotParam.param";});
    if(entry==files.end())throw std::runtime_error("Item lot binder entry missing");
    binder.resize((binder.size()+15)&~std::size_t(15),0);if(binder.size()+param.size()>256*1024*1024)throw std::runtime_error("Parameter binder exceeds limit");
    Put<std::uint64_t>(binder,entry->header+8,param.size());Put<std::uint64_t>(binder,entry->header+16,param.size());Put<std::uint32_t>(binder,entry->header+24,binder.size());binder.insert(binder.end(),param.begin(),param.end());
    auto result=PackAsset(source,binder);
    const auto verified=Read(result);const auto* saved=verified.Table("ItemLotParam");
    if(!saved || saved->rows.size()!=table->rows.size()+lots.size())throw std::runtime_error("Parameter export verification failed");
    for(const auto& lot:lots){const auto check=ReadItemLot(*saved,lot.id);if(check.bytes!=lot.bytes)throw std::runtime_error("Custom item lot verification failed");}
    return result;
}
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
