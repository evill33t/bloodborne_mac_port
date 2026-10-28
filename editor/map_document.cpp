// SPDX-License-Identifier: GPL-2.0-or-later
// MSBB layout checked against Smithbox/SoulsFormats MSBB Parts/Point/EventParam (MIT):
// https://github.com/vawser/Smithbox/blob/main/src/Andre/SoulsFormats/SoulsFormats/Formats/MSB/MSBB/PartsParam.cs
#include "map_document.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace BbMapEditor {
namespace {
template<class T> T Get(std::span<const std::uint8_t> b, std::size_t at) {
    if(at > b.size() || sizeof(T) > b.size()-at) throw std::runtime_error("Truncated map record");
    T v; std::memcpy(&v,b.data()+at,sizeof(v)); return v;
}
template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T v) {
    if(at>b.size() || sizeof(T)>b.size()-at) throw std::runtime_error("Invalid map write offset");
    std::memcpy(b.data()+at,&v,sizeof(v));
}
std::size_t Offset(std::span<const std::uint8_t> b, std::size_t at, std::size_t base=0) {
    auto p=Get<std::int64_t>(b,at);
    if(p<0 || base>b.size() || std::uint64_t(p)>=b.size()-base) throw std::runtime_error("Invalid map offset");
    return base+std::size_t(p);
}
std::string Name(std::span<const std::uint8_t> b,std::size_t at) {
    std::string s;
    for(int i=0;i<512;++i,at+=2) {
        auto c=Get<std::uint16_t>(b,at); if(!c) return s;
        std::uint32_t cp=c;
        if(c>=0xd800 && c<=0xdbff) {
            auto low=Get<std::uint16_t>(b,at+2);
            if(low<0xdc00 || low>0xdfff) throw std::runtime_error("Invalid UTF-16 map name");
            cp=0x10000+((c-0xd800)<<10)+(low-0xdc00);at+=2;++i;
        } else if(c>=0xdc00 && c<=0xdfff) throw std::runtime_error("Invalid UTF-16 map name");
        if(cp<0x80) s+=char(cp);
        else {
            if(cp>=0x10000) {s+=char(0xf0|(cp>>18));s+=char(0x80|((cp>>12)&63));}
            else if(cp>=0x800) s+=char(0xe0|(cp>>12));
            if(cp>=0x800) s+=char(0x80|((cp>>6)&63));
            else s+=char(0xc0|(cp>>6));
            s+=char(0x80|(cp&63));
        }
    }
    throw std::runtime_error("Unterminated map identifier");
}
std::uint32_t BE(std::span<const std::uint8_t> b,std::size_t at) {
    return std::uint32_t(Get<std::uint8_t>(b,at))<<24 | std::uint32_t(Get<std::uint8_t>(b,at+1))<<16 |
        std::uint32_t(Get<std::uint8_t>(b,at+2))<<8 | Get<std::uint8_t>(b,at+3);
}
void PutBE(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v) {
    for(int i=0;i<4;++i) b[at+i]=std::uint8_t(v>>(24-8*i));
}
void Check(const Transform& t) {
    for(const auto& v:{t.position,t.rotation,t.scale}) for(float f:v)
        if(!std::isfinite(f) || std::abs(f)>1e6f) throw std::runtime_error("Transform must contain finite, bounded numbers");
    for(float f:t.scale) if(std::abs(f)<.0001f || std::abs(f)>1000) throw std::runtime_error("Scale must be nonzero and within 1000");
}
std::string FreshName(const char* prefix,const std::set<std::string>& used) {
    for(int i=0;i<100000;++i) {auto n=std::to_string(i);auto name=prefix+std::string(n.size()<4?4-n.size():0,'0')+n;if(!used.contains(name))return name;}
    throw std::runtime_error("No free record name");
}
void AppendName(std::vector<std::uint8_t>& bytes,const std::string& name) {
    // Generated identifiers are ASCII; existing UTF-16 strings stay in opaque records.
    for(unsigned char c:name){if(c>=128)throw std::runtime_error("Expected a generated ASCII identifier");bytes.push_back(c);bytes.push_back(0);}
    bytes.push_back(0);bytes.push_back(0);
}
void Align(std::vector<std::uint8_t>& bytes,std::size_t alignment) {bytes.resize((bytes.size()+alignment-1)&~(alignment-1),0);}
std::vector<std::uint8_t> PointRecord(const Region& r,int id) {
    std::vector<std::uint8_t> b(0x50);Put<std::int64_t>(b,0,0x50);Put<std::int32_t>(b,12,id);
    AppendName(b,r.name);Align(b,4);
    Put<std::int64_t>(b,0x30,b.size());b.resize(b.size()+2);Align(b,4);
    Put<std::int64_t>(b,0x38,b.size());b.resize(b.size()+2);Align(b,8);
    Put<std::int64_t>(b,0x48,b.size());b.resize(b.size()+4);Put(b,b.size()-4,r.entity_id);Align(b,8);
    Put(b,0x14,r.transform.position);Put(b,0x20,r.transform.rotation);return b;
}
std::vector<std::uint8_t> PatrolRecord(const Route& r,int id) {
    std::vector<std::uint8_t> b(0x28);Put<std::int64_t>(b,0,0x28);Put(b,8,r.event_id);
    Put<std::uint32_t>(b,12,14);Put<std::int32_t>(b,16,id);AppendName(b,r.name);Align(b,8);
    Put<std::int64_t>(b,0x18,b.size());const auto entity=b.size();b.resize(entity+16);
    Put<std::int32_t>(b,entity,-1);Put<std::int32_t>(b,entity+4,-1);Put(b,entity+8,r.entity_id);
    Put<std::int64_t>(b,0x20,b.size());const auto data=b.size();b.resize(data+80);
    Put(b,data,r.data.type);for(int i=0;i<32;++i)Put<std::int16_t>(b,data+16+i*2,-1);return b;
}
void WriteExclusive(const std::filesystem::path& path,std::span<const std::uint8_t> b) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0644);
    if(fd<0) throw std::runtime_error("Cannot create export file: "+path.string());
    std::size_t at=0;
    while(at<b.size()) {
        auto n=write(fd,b.data()+at,b.size()-at);
        if(n<=0) {close(fd);throw std::runtime_error("Export write failed");}
        at+=std::size_t(n);
    }
    const int synced=fsync(fd),closed=close(fd);
    if(synced || closed) throw std::runtime_error("Export could not be flushed");
}
}
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f) throw std::runtime_error("Cannot read "+path.string());
    auto length=f.tellg();
    if(length<0 || length>512*1024*1024) throw std::runtime_error("Asset exceeds 512 MiB limit");
    std::vector<std::uint8_t> b(static_cast<std::size_t>(length)); f.seekg(0);
    if(!b.empty() && !f.read(reinterpret_cast<char*>(b.data()),b.size())) throw std::runtime_error("Short asset read");
    return b;
}
bool ValidMapName(const std::string& s) {
    if(s.size()!=12 || s[0]!='m') return false;
    for(std::size_t i=1;i<s.size();++i) {
        if(i==3 || i==6 || i==9) {if(s[i]!='_') return false;}
        else if(s[i]<'0' || s[i]>'9') return false;
    }
    return true;
}
Document Document::Open(const std::filesystem::path& p) {
    auto d=Read(ReadFile(p));const auto path=std::filesystem::canonical(p);
    d.source_map=path;
    auto lower=[](const auto& p) {auto name=p.filename().string();for(char& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));return name;};
    if(lower(path.parent_path())=="mapstudio" && lower(path.parent_path().parent_path())=="map" &&
       lower(path.parent_path().parent_path().parent_path())=="dvdroot_ps4")
        d.source_game=path.parent_path().parent_path().parent_path().parent_path();
    return d;
}
Document Document::Read(std::span<const std::uint8_t> file) {
    Document d;d.source.assign(file.begin(),file.end());
    d.dcx=file.size()>=4 && !std::memcmp(file.data(),"DCX\0",4);
    if(d.dcx) {
        // Bloodborne's DFLT_10000_24_9 envelope; refuse other compression variants.
        if(file.size()<0x4c || std::memcmp(file.data()+0x18,"DCS\0",4) ||
           std::memcmp(file.data()+0x24,"DCP\0DFLT",8) || std::memcmp(file.data()+0x44,"DCA\0",4) ||
           BE(file,4)!=0x10000 || BE(file,0x10)!=0x44 || BE(file,0x14)!=0x4c || BE(file,0x48)!=8 ||
           BE(file,0x1c)>64*1024*1024 || !BE(file,0x1c) || BE(file,0x20)!=file.size()-0x4c)
            throw std::runtime_error("Unsupported or invalid Bloodborne DCX envelope");
        d.raw.resize(BE(file,0x1c));mz_ulong length=d.raw.size();
        if(mz_uncompress(d.raw.data(),&length,file.data()+0x4c,file.size()-0x4c)!=MZ_OK || length!=d.raw.size())
            throw std::runtime_error("Map decompression failed");
    } else d.raw=d.source;
    const std::span<const std::uint8_t> b=d.raw;
    constexpr std::uint8_t header[]={0x4d,0x53,0x42,0x20,1,0,0,0,0x10,0,0,0,0,0,1,0xff};
    if(b.size()<sizeof(header) || std::memcmp(b.data(),header,sizeof(header))) throw std::runtime_error("Expected a Bloodborne MSBB map");
    std::map<std::string,std::vector<std::size_t>> lists;
    std::set<std::size_t> visited,record_offsets,boundaries{b.size()};
    std::size_t at=16;
    while(at) {
        if(!visited.insert(at).second || visited.size()>16) throw std::runtime_error("Cyclic map sections");
        const auto count=Get<std::uint32_t>(b,at+4);
        if(!count || count>100000 || at>b.size() || b.size()-at<16 || count>(b.size()-at-16)/8)
            throw std::runtime_error("Invalid map section count");
        const auto name_at=Offset(b,at+8);auto name=Name(b,name_at);
        boundaries.insert(at);boundaries.insert(name_at);
        if(lists.contains(name)) throw std::runtime_error("Duplicate map section");
        auto& entries=lists[name];
        for(std::uint32_t i=0;i+1<count;++i) {
            auto p=Offset(b,at+16+i*8);
            if(!record_offsets.insert(p).second) throw std::runtime_error("Aliased map records");
            entries.push_back(p);
            boundaries.insert(p);
        }
        if(Get<std::uint32_t>(b,at)!=3) throw std::runtime_error("Unsupported map section version");
        const auto next=Get<std::int64_t>(b,at+16+(count-1)*8);
        if(next<0 || std::uint64_t(next)>=b.size()) throw std::runtime_error("Invalid next section");
        d.sections.push_back({name,at,name_at,std::size_t(next),entries});
        at=std::size_t(next);
    }
    for(auto name:{"MODEL_PARAM_ST","PARTS_PARAM_ST","EVENT_PARAM_ST","POINT_PARAM_ST"})
        if(!lists.contains(name)) throw std::runtime_error("Incomplete map sections");
    std::vector<std::string> models;
    for(auto p:lists.at("MODEL_PARAM_ST")) models.push_back(Name(b,Offset(b,p,p)));
    for(auto p:record_offsets) d.record_ends[p]=*boundaries.upper_bound(p);
    auto end_of=[&](std::size_t p) {return d.record_ends.at(p);};
    auto local=[&](std::size_t p,std::size_t field,std::size_t length) {
        auto at=Offset(b,p+field,p),end=end_of(p);
        if(at<p+0xd0 || at>end || length>end-at) throw std::runtime_error("Invalid enemy data offset");
        return at;
    };
    const auto& all_parts=lists.at("PARTS_PARAM_ST");
    auto ref=[&](int index,const std::vector<std::size_t>& entries,int required_type=-1) {
        Reference r;r.index=index;
        if(index>=0 && std::size_t(index)<entries.size()) {
            auto p=entries[index];
            if(required_type<0 || Get<std::uint32_t>(b,p+0x14)==std::uint32_t(required_type))
                r.name=Name(b.first(end_of(p)),Offset(b,p+(required_type>=0?8:0),p));
        }
        return r;
    };
    std::vector<std::size_t> patrols;
    for(auto p:lists.at("EVENT_PARAM_ST")) if(Get<std::uint32_t>(b,p+12)==14) patrols.push_back(p);
    for(auto p:lists.at("POINT_PARAM_ST")) {
        if(end_of(p)-p<0x50)throw std::runtime_error("Truncated region header");
        Region r;r.record=p;r.key=int(d.regions.size());r.name=Name(b.first(end_of(p)),Offset(b,p,p));
        r.shape=Get<std::uint32_t>(b,p+0x10);
        r.original.position=Get<BbMapArt::Vec3>(b,p+0x14);r.original.rotation=Get<BbMapArt::Vec3>(b,p+0x20);
        Check(r.original);r.transform=r.original;
        if(Get<std::int64_t>(b,p+0x48))r.entity_id=Get<std::int32_t>(b,Offset(b,p+0x48,p));
        d.regions.push_back(std::move(r));
    }
    for(auto p:patrols) {
        Route r;r.record=p;r.key=int(d.routes.size());r.name=Name(b.first(end_of(p)),Offset(b,p,p));
        r.event_id=Get<std::int32_t>(b,p+8);
        const auto data=Offset(b,p+0x20,p);
        if(data<p+0x28 || data>end_of(p) || end_of(p)-data<80)throw std::runtime_error("Invalid patrol data offset");
        if(Get<std::int64_t>(b,p+0x18))r.entity_id=Get<std::int32_t>(b,Offset(b,p+0x18,p)+8);
        r.original.type=Get<std::int32_t>(b,data);
        for(int i=0;i<32;++i)r.original.points[i]=ref(Get<std::int16_t>(b,data+16+i*2),lists.at("POINT_PARAM_ST"));
        r.data=r.original;d.routes.push_back(std::move(r));
    }
    for(auto p:lists.at("EVENT_PARAM_ST"))if(Get<std::uint32_t>(b,p+12)==4) {
        Treasure t;t.record=p;t.name=Name(b.first(end_of(p)),Offset(b,p,p));t.event_id=Get<std::int32_t>(b,p+8);
        const auto data=Offset(b,p+0x20,p);if(data<p+0x28 || data>end_of(p) || end_of(p)-data<80)throw std::runtime_error("Invalid treasure data offset");
        if(Get<std::int64_t>(b,p+0x18))t.entity_id=Get<std::int32_t>(b,Offset(b,p+0x18,p)+8);
        t.data.part.index=Get<std::int32_t>(b,data+8);
        if(t.data.part.index>=0 && std::size_t(t.data.part.index)<all_parts.size()) {
            const auto part=all_parts[t.data.part.index];t.data.part.name=Name(b.first(end_of(part)),Offset(b,part+8,part));
        }
        for(int i=0;i<3;++i)t.data.lots[i]=Get<std::int32_t>(b,data+0x10+i*4);
        t.data.action_button=Get<std::int32_t>(b,data+0x38);t.data.in_chest=Get<std::uint8_t>(b,data+0x40)!=0;t.data.disabled=Get<std::uint8_t>(b,data+0x41)!=0;
        t.original=t.data;d.treasures.push_back(std::move(t));
    }
    std::set<std::string> names;
    for(auto p:all_parts) {
        auto type=Get<std::uint32_t>(b,p+0x14);
        if(type!=1 && type!=2 && type!=9 && type!=10) continue; // Object/Enemy and their dormant variants.
        if(p>b.size() || b.size()-p<0xd0) throw std::runtime_error("Truncated object header");
        const auto end=end_of(p);
        if(end-p<0xd0) throw std::runtime_error("Overlapping object records");
        const auto name_at=Offset(b,p+8,p);
        if(name_at<p+0xd0 || name_at>=end) throw std::runtime_error("Invalid object name offset");
        auto name=Name(b.first(end),name_at);
        auto model=Get<std::int32_t>(b,p+0x1c);
        if(model<0 || std::size_t(model)>=models.size() || !names.insert(name).second)
            throw std::runtime_error("Invalid or duplicate object reference");
        Part o;o.name=name;o.model=models[model];o.record=p;o.type=type;
        o.instance_id=Get<std::int32_t>(b,p+0x10);o.type_id=Get<std::int32_t>(b,p+0x18);
        if(Get<std::int64_t>(b,p+0xb0))o.entity_id=Get<std::int32_t>(b,local(p,0xb0,16));
        o.original.position=Get<BbMapArt::Vec3>(b,p+0x28);
        o.original.rotation=Get<BbMapArt::Vec3>(b,p+0x34);
        o.original.scale=Get<BbMapArt::Vec3>(b,p+0x40);Check(o.original);o.transform=o.original;
        if(type==2 || type==10) {
            o.kind=PartKind::Enemy;EnemyData e;e.dummy=type==10;
            const auto entity=local(p,0xb0,16),data=local(p,0xb8,64);
            e.entity_id=Get<std::int32_t>(b,entity);e.instance_id=o.instance_id;
            e.think_param=Get<std::int32_t>(b,data+8);e.npc_param=Get<std::int32_t>(b,data+12);
            e.talk_id=Get<std::int32_t>(b,data+16);e.chara_init=Get<std::int32_t>(b,data+24);
            e.patrol_type=Get<std::int16_t>(b,data+20);e.fallback_platoon=Get<std::int16_t>(b,data+22);
            e.collision=ref(Get<std::int32_t>(b,data+28),all_parts,5);
            e.patrol=ref(Get<std::int16_t>(b,data+32),patrols);
            for(int i=0;i<8;++i)e.move_points[i]=ref(Get<std::int16_t>(b,data+40+i*2),lists.at("POINT_PARAM_ST"));
            e.init_anim=Get<std::int32_t>(b,data+56);e.damage_anim=Get<std::int32_t>(b,data+60);
            o.enemy=std::move(e);
            o.original_enemy=o.enemy;
        }
        d.parts.push_back(std::move(o));
    }
    d.next_region_key=int(d.regions.size());d.next_route_key=int(d.routes.size());
    for(const auto& r:d.routes)for(const auto& point:r.data.points)d.next_region_key=std::max(d.next_region_key,point.index+1);
    for(const auto& p:d.parts)if(p.enemy) {
        d.next_route_key=std::max(d.next_route_key,p.enemy->patrol.index+1);
        for(const auto& point:p.enemy->move_points)d.next_region_key=std::max(d.next_region_key,point.index+1);
    }
    d.ReserveIdentifiers();
    return d;
}
void Document::ReserveIdentifiers() {
    const auto& b=raw;
    std::vector<std::string> models;
    for(const auto& s:sections) if(s.name=="MODEL_PARAM_ST")
        for(auto p:s.entries)models.push_back(Name(b,Offset(b,p,p)));
    for(const auto& s:sections) for(auto p:s.entries) {
        // Optional zero offsets accommodate records with no entity data; nonzero offsets
        // must stay within that record, including on parts the viewport does not display.
        auto entity=[&](std::size_t field,std::size_t extra) {
            if(p+field+8>record_ends.at(p))throw std::runtime_error("Truncated identifier header");
            if(!Get<std::int64_t>(b,p+field))return;
            const auto at=Offset(b,p+field,p),end=record_ends.at(p);
            if(at<p+field+8 || at>end || extra+4>end-at)throw std::runtime_error("Invalid entity identifier offset");
            const auto id=Get<std::int32_t>(b,at+extra);if(id>0)reserved_entities.insert(id);
        };
        if(s.name=="PARTS_PARAM_ST") {
            reserved_names.insert(Name(b,Offset(b,p+8,p)));
            auto model=Get<std::int32_t>(b,p+0x1c);
            if(model>=0 && std::size_t(model)<models.size())
                reserved_instances[models[model]].insert(Get<std::int32_t>(b,p+0x10));
            reserved_types[Get<std::uint32_t>(b,p+0x14)].insert(Get<std::int32_t>(b,p+0x18));
            entity(0xb0,0);
        } else if(s.name=="EVENT_PARAM_ST") {
            if(Get<std::uint32_t>(b,p+12)==14)reserved_route_names.insert(Name(b,Offset(b,p,p)));
            reserved_treasure_names.insert(Name(b,Offset(b,p,p)));
            entity(0x18,8);
        } else if(s.name=="POINT_PARAM_ST") {reserved_region_names.insert(Name(b,Offset(b,p,p)));entity(0x48,0);}
    }
}
void Document::ReserveSiblings() {
    if(reserved_siblings)return;
    // Variants may be loaded together. Reserve identifiers from every sibling layout
    // of this area before allocating anything; a failed read must not silently collide.
    if(!source_map.empty()) {
        const auto prefix=source_map.filename().string().substr(0,6)+"_";
        for(const auto& f:std::filesystem::directory_iterator(source_map.parent_path())) {
            const auto name=f.path().filename().string();
            if(f.path()==source_map || !name.starts_with(prefix) ||
               !ValidMapName(name.substr(0,12)) ||
               !(name.substr(12)==".msb.dcx" || name.substr(12)==".msb"))continue;
            const auto sibling=Read(ReadFile(f.path()));
            reserved_names.insert(sibling.reserved_names.begin(),sibling.reserved_names.end());
            reserved_entities.insert(sibling.reserved_entities.begin(),sibling.reserved_entities.end());
            reserved_region_names.insert(sibling.reserved_region_names.begin(),sibling.reserved_region_names.end());
            reserved_route_names.insert(sibling.reserved_route_names.begin(),sibling.reserved_route_names.end());
            reserved_treasure_names.insert(sibling.reserved_treasure_names.begin(),sibling.reserved_treasure_names.end());
            for(const auto& [model,ids]:sibling.reserved_instances)reserved_instances[model].insert(ids.begin(),ids.end());
        }
    }
    reserved_siblings=true;
}
bool Document::Changed() const {
    return std::ranges::any_of(parts,[](const Part& o){return o.added || o.transform!=o.original || o.enemy!=o.original_enemy;}) ||
        std::ranges::any_of(regions,[](const Region& r){return r.added || r.transform!=r.original;}) ||
        std::ranges::any_of(routes,[](const Route& r){return r.added || r.data!=r.original;}) ||
        std::ranges::any_of(treasures,[](const Treasure& t){return t.added || t.data!=t.original;}) || !item_lots.empty() || !event_edits.empty() || !quests.empty();
}
bool Document::Set(std::size_t i,const Transform& t) {
    Check(t);auto& o=parts.at(i);
    if(o.kind==PartKind::Enemy && t.scale!=o.original.scale) throw std::runtime_error("Enemy scale is preserved; edit position and rotation only");
    if(o.transform==t) return false;
    auto after=o;after.transform=t;
    history.resize(cursor);history.push_back({i,o,after});++cursor;o=std::move(after);return true;
}
bool Document::SetEnemy(std::size_t i,const EnemyData& value) {
    auto& part=parts.at(i);if(!part.enemy)throw std::runtime_error("Select a character first");
    auto data=value;const auto& before=*part.enemy;
    if(data.dummy!=before.dummy || data.entity_id!=before.entity_id || data.instance_id!=before.instance_id)
        throw std::runtime_error("Character type and identifiers are read only");
    if(data.patrol_type!=before.patrol_type && (data.patrol_type<0 || data.patrol_type>6))throw std::runtime_error("Unsupported patrol behavior");
    auto resolve=[&](Reference& ref,const Reference& old,const auto& list) {
        if(ref==old)return;
        if(ref.index==-1){ref.name.clear();return;}
        auto found=std::ranges::find(list,ref.index,[](const auto& record){return record.key;});
        if(found==list.end())throw std::runtime_error("Reference does not exist in this map");ref.name=found->name;
    };
    resolve(data.patrol,before.patrol,routes);
    for(int n=0;n<8;++n)resolve(data.move_points[n],before.move_points[n],regions);
    if(data.collision!=before.collision) {
        if(data.collision.index==-1)data.collision.name.clear();
        else {
            const auto& table=*std::ranges::find(sections,std::string("PARTS_PARAM_ST"),&Section::name);
            if(data.collision.index<0 || std::size_t(data.collision.index)>=table.entries.size() ||
               Get<std::uint32_t>(raw,table.entries[data.collision.index]+0x14)!=5)throw std::runtime_error("Loading collision must reference a collision part");
            const auto p=table.entries[data.collision.index];data.collision.name=Name(raw,Offset(raw,p+8,p));
        }
    }
    if(data==before)return false;
    auto after=part;after.enemy=data;history.resize(cursor);history.push_back({i,part,after});++cursor;part=std::move(after);return true;
}
bool Document::SetRegion(std::size_t i,const Transform& t) {
    Check(t);auto& region=regions.at(i);
    if(t.scale!=region.original.scale)throw std::runtime_error("Region dimensions are preserved; edit position and rotation");
    if(t==region.transform)return false;
    auto after=region;after.transform=t;history.resize(cursor);history.push_back({i,region,after});++cursor;region=std::move(after);return true;
}
bool Document::SetRoute(std::size_t i,const RouteData& value) {
    auto& route=routes.at(i);auto data=value;
    if(data.type!=route.data.type && (data.type<0 || data.type>6))throw std::runtime_error("Unsupported patrol behavior");
    for(int n=0;n<32;++n) {
        auto& ref=data.points[n];if(ref==route.data.points[n])continue;
        if(ref.index==-1){ref.name.clear();continue;}
        auto found=std::ranges::find(regions,ref.index,&Region::key);
        if(found==regions.end())throw std::runtime_error("Waypoint must reference a region in this map");ref.name=found->name;
    }
    if(data==route.data)return false;
    auto after=route;after.data=std::move(data);history.resize(cursor);history.push_back({i,route,after});++cursor;route=std::move(after);return true;
}
std::size_t Document::AddWaypoint(const BbMapArt::Vec3& position) {
    if(regions.size()>=32767)throw std::runtime_error("Patrol region index limit reached");
    Region r;r.transform.position=position;Check(r.transform);r.original=r.transform;
    r.entity_id=NewEntityID();r.name=FreshName("Waypoint_",reserved_region_names);reserved_region_names.insert(r.name);
    r.key=next_region_key++;r.added=true;const auto index=regions.size();
    history.resize(cursor);history.push_back({index,std::nullopt,r});++cursor;regions.push_back(r);return index;
}
std::size_t Document::AddRoute() {
    if(routes.size()>=32767)throw std::runtime_error("Patrol route index limit reached");
    Route r;r.entity_id=NewEntityID();r.name=FreshName("Patrol_",reserved_route_names);reserved_route_names.insert(r.name);
    r.event_id=NewEventID();r.key=next_route_key++;r.added=true;const auto index=routes.size();
    history.resize(cursor);history.push_back({index,std::nullopt,r});++cursor;routes.push_back(r);return index;
}
std::int32_t Document::NewEventID() const {
    std::int32_t last=-1;
    for(const auto& s:sections)if(s.name=="EVENT_PARAM_ST")for(auto p:s.entries)last=std::max(last,Get<std::int32_t>(raw,p+8));
    for(const auto& route:routes)last=std::max(last,route.event_id);
    for(const auto& treasure:treasures)last=std::max(last,treasure.event_id);
    if(last==std::numeric_limits<std::int32_t>::max())throw std::runtime_error("No free event identifier");
    return last+1;
}
ItemLot Document::NewLot(std::int32_t source_id,bool independent) {
    if(!parameters || !parameters->Table("ItemLotParam"))throw std::runtime_error("Load the game's item parameters first");
    const auto& table=*parameters->Table("ItemLotParam");const auto current=std::ranges::find(item_lots,source_id,&ItemLot::id);
    auto lot=current==item_lots.end()?ReadItemLot(table,source_id):*current;
    for(lot.id=900000000;lot.id<1000000000;lot.id+=10)if(!table.Find(lot.id) && !reserved_lot_ids.contains(lot.id))break;
    if(lot.id>=1000000000)throw std::runtime_error("Custom item lot range is full");
    if(independent) {
        const auto flag=NewPickupFlag(lot.Flag());
        // Per-slot flags must also be independent. Zero means inherit the common flag.
        for(int n=0;n<8;++n)Put<std::int32_t>(lot.bytes,96+n*4,0);
        lot.SetFlag(flag);
    }
    reserved_lot_ids.insert(lot.id);lot.name="Editor item "+std::to_string(lot.id);return lot;
}
std::int32_t Document::NewPickupFlag(std::int32_t original) {
        if(original<=0 || !parameters)throw std::runtime_error("Choose a pickup with a persistent collection flag");
        const auto group=original/1000*1000;
        if(!reserved_pickup_flags.contains(group)) {
            const auto used=parameters->UsedFlags(group);std::set<std::int32_t> flags(used.begin(),used.end());
            auto reserve=[&](std::span<const std::uint8_t> bytes){for(std::size_t at=0;at+4<=bytes.size();++at){const auto value=Get<std::int32_t>(bytes,at);if(value>=group && value<group+1000)flags.insert(value);}};
            if(!source_game.empty()) {
                for(const auto& relative:{"event","script/talk"}) {
                    const auto folder=source_game/"dvdroot_ps4"/relative;if(!std::filesystem::exists(folder))continue;
                    for(const auto& f:std::filesystem::directory_iterator(folder))if(f.is_regular_file() && f.path().extension()==".dcx")reserve(UnpackAsset(ReadFile(f.path())));
                }
            }
            reserved_pickup_flags.emplace(group,std::move(flags));
        }
        auto& flags=reserved_pickup_flags[group];
        for(const auto& edit:event_edits)if(edit.value>=std::uint32_t(group) && edit.value<std::uint32_t(group+1000))flags.insert(std::int32_t(edit.value));
        auto flag=group+900;while(flag<group+1000 && flags.contains(flag))++flag;
        if(flag>=group+1000)throw std::runtime_error("No unused pickup flag in this pickup's retail flag group");
        flags.insert(flag);return flag;
}
void Document::AttachEvents(const std::string& name,std::shared_ptr<const EventFile> file) {
    if(!ValidEventName(name) || !file || Changed())throw std::runtime_error("Attach event files before editing");
    for(const auto& event:file->Events())if(event.id>=0 && event.id<=INT32_MAX)reserved_script_ids.insert(std::int32_t(event.id));scripts[name]=std::move(file);
}
ScriptOperand Document::EventArgumentValue(const std::string& name,std::size_t event,std::size_t instruction,std::size_t argument) const {
    const auto found=scripts.find(name);if(found==scripts.end())throw std::runtime_error("Event script is unavailable");
    auto operand=found->second->Operand(event,instruction,argument);operand.script=name;
    for(const auto& edit:event_edits)if(edit.script==name && edit.at==operand.at)return edit;return operand;
}
bool Document::SetEventArgument(const std::string& name,std::size_t event,std::size_t instruction,std::size_t argument,double value) {
    const auto script=scripts.find(name);if(script==scripts.end())throw std::runtime_error("Event script is unavailable");
    auto after=script->second->Edit(event,instruction,argument,value);after.script=name;const auto& command=script->second->Events().at(event).instructions.at(instruction);
    if(EventArgumentValue(name,event,instruction,argument).value==after.value)return false;
    if(command.bank==2003 && command.id==4 && argument==0) {
        const auto id=std::int32_t(after.value);if(std::ranges::find(item_lots,id,&ItemLot::id)==item_lots.end() &&
           (!parameters || !parameters->Table("ItemLotParam") || !parameters->Table("ItemLotParam")->Find(id)))throw std::runtime_error("Reward item lot does not exist");
    }
    if(command.bank==2000 && command.id==0 && argument==1) {
        bool present=std::ranges::any_of(quests,[&](const auto& q){return q.event_id==std::int32_t(after.value);});
        for(const auto& [_,file]:scripts) {
            const auto target=std::ranges::find(file->Events(),std::int64_t(after.value),&ScriptEvent::id);if(target==file->Events().end())continue;present=true;
            std::size_t required=0;for(const auto& parameter:target->parameters)required=std::max(required,parameter.source+parameter.length);
            if(command.length<8 || required>command.length-8)throw std::runtime_error("Called event requires more parameter bytes than this instruction supplies");
        }
        if(!present)throw std::runtime_error("Called event ID is not present in the area or common script");
    }
    auto existing=std::ranges::find_if(event_edits,[&](const auto& e){return e.script==name && e.at==after.at;});
    if(existing==event_edits.end()) {
        if(after.value==after.original)return false;history.resize(cursor);history.push_back({event_edits.size(),std::nullopt,after});++cursor;event_edits.push_back(after);
    } else {
        if(*existing==after)return false;const auto index=std::size_t(existing-event_edits.begin());history.resize(cursor);
        if(after.value==after.original){history.push_back({index,*existing,std::nullopt});++cursor;event_edits.erase(existing);}
        else {history.push_back({index,*existing,after});++cursor;*existing=after;}
    }
    return true;
}
std::size_t Document::AddPickupQuest(std::size_t treasure,const ItemContents& reward,const std::string& name) {
    const auto script=scripts.find(name);if(script==scripts.end() || name=="common.emevd.dcx" || !ValidEventName(name))throw std::runtime_error("Load an area event script first");
    if(std::ranges::find(script->second->Events(),std::int64_t(0),&ScriptEvent::id)==script->second->Events().end())throw std::runtime_error("Area constructor event is missing");
    if(!parameters)throw std::runtime_error("Item parameters are unavailable");
    ItemLot checked;checked.bytes.resize(148);checked.SetFlag(-1);parameters->SetContents(checked,reward);
    const auto& t=treasures.at(treasure);auto lot=NewLot(t.data.lots[0],false);const auto trigger=lot.Flag();
    const auto completed=NewPickupFlag(trigger);parameters->SetContents(lot,reward);lot.SetFlag(-1);
    const auto block=std::stoi(name.substr(1,2))*10+std::stoi(name.substr(4,2));const auto first_id=10000000+block*10000+9000;
    auto id=first_id;while(id<first_id+1000 && reserved_script_ids.contains(id))++id;if(id>=first_id+1000)throw std::runtime_error("Area custom-event ID range is full");
    PickupQuest quest{name,t.name,id,trigger,completed,lot.id};const auto first=cursor;history.resize(cursor);
    history.push_back({item_lots.size(),std::nullopt,lot});++cursor;item_lots.push_back(lot);
    const auto index=quests.size();history.push_back({index,std::nullopt,quest});++cursor;quests.push_back(quest);reserved_script_ids.insert(id);Group(first);return index;
}
bool Document::RemoveQuest(std::size_t index) {
    const auto quest=quests.at(index);const auto first=cursor;history.resize(cursor);history.push_back({index,quest,std::nullopt});++cursor;quests.erase(quests.begin()+std::ptrdiff_t(index));
    const auto lot=std::ranges::find(item_lots,quest.reward_lot,&ItemLot::id);
    if(lot!=item_lots.end() && std::ranges::none_of(quests,[&](const auto& q){return q.reward_lot==lot->id;}) && std::ranges::none_of(event_edits,[&](const auto& e){return e.value==std::uint32_t(lot->id);}) && std::ranges::none_of(treasures,[&](const auto& t){return std::ranges::find(t.data.lots,lot->id)!=t.data.lots.end();})) {
        const auto at=std::size_t(lot-item_lots.begin());history.push_back({at,*lot,std::nullopt});++cursor;item_lots.erase(lot);
    }
    Group(first);return true;
}
bool Document::SetTreasure(std::size_t i,const TreasureData& value) {
    auto& t=treasures.at(i);auto data=value;
    if(data.part!=t.data.part) {
        const auto found=std::ranges::find(parts,data.part.name,&Part::name);
        if(found==parts.end() || found->kind!=PartKind::Object)throw std::runtime_error("Treasure must reference an object in this map");
    }
    for(int n=0;n<3;++n)if(data.lots[n]!=t.data.lots[n] && data.lots[n]!=-1) {
        if(std::ranges::find(item_lots,data.lots[n],&ItemLot::id)!=item_lots.end())continue;
        if(!parameters || !parameters->Table("ItemLotParam") || !parameters->Table("ItemLotParam")->Find(data.lots[n]))throw std::runtime_error("Item lot not found");
    }
    if(data==t.data)return false;auto after=t;after.data=data;
    history.resize(cursor);history.push_back({i,t,after});++cursor;t=std::move(after);return true;
}
bool Document::SetItemContents(std::size_t i,const ItemContents& contents) {
    if(!parameters)throw std::runtime_error("Item parameter lookup is unavailable");
    const auto first=cursor;const auto& t=treasures.at(i);
    const auto found=std::ranges::find(item_lots,t.data.lots[0],&ItemLot::id);
    if(found!=item_lots.end()) {
        const auto index=std::size_t(found-item_lots.begin());auto after=*found;parameters->SetContents(after,contents);if(after==*found)return false;
        history.resize(cursor);history.push_back({index,*found,after});++cursor;*found=std::move(after);return true;
    }
    auto lot=NewLot(t.data.lots[0],false);parameters->SetContents(lot,contents);auto data=t.data;data.lots={lot.id,-1,-1};
    history.resize(cursor);history.push_back({item_lots.size(),std::nullopt,lot});++cursor;item_lots.push_back(lot);
    SetTreasure(i,data);Group(first);return true;
}
std::size_t Document::DuplicateTreasure(std::size_t i) {
    Treasure copy=treasures.at(i);
    if(copy.data.in_chest || copy.data.lots[1]!=-1 || copy.data.lots[2]!=-1)throw std::runtime_error("Choose a ground pickup with one item lot");
    const auto part=std::ranges::find(parts,copy.data.part.name,&Part::name);
    if(part==parts.end() || part->kind!=PartKind::Object)throw std::runtime_error("Pickup's supporting object is unavailable");
    const auto part_index=std::size_t(part-parts.begin());auto lot=NewLot(copy.data.lots[0],true);
    copy.event_id=NewEventID();copy.name=FreshName("Treasure_",reserved_treasure_names);copy.entity_id=NewEntityID();copy.added=true;
    const auto first=cursor;
    try {
        const auto new_part=Duplicate(part_index,true);copy.data.part={-1,parts[new_part].name};copy.data.lots={lot.id,-1,-1};copy.data.disabled=false;copy.original=copy.data;
        history.resize(cursor);history.push_back({item_lots.size(),std::nullopt,lot});++cursor;item_lots.push_back(lot);
        const auto index=treasures.size();history.push_back({index,std::nullopt,copy});++cursor;treasures.push_back(copy);
        reserved_treasure_names.insert(copy.name);Group(first);return index;
    }catch(...){while(cursor>first)Undo();history.resize(cursor);throw;}
}
bool Document::RemoveTreasure(std::size_t i) {
    const auto t=treasures.at(i);if(!t.added)throw std::runtime_error("Only newly created pickups can be removed");
    if(std::ranges::any_of(quests,[&](const auto& q){return q.treasure==t.name;}))throw std::runtime_error("Remove this pickup's reward quest first");
    const auto first=cursor;history.resize(cursor);history.push_back({i,t,std::nullopt});++cursor;treasures.erase(treasures.begin()+std::ptrdiff_t(i));
    const auto part=std::ranges::find(parts,t.data.part.name,&Part::name);
    if(part!=parts.end() && part->added && std::ranges::none_of(treasures,[&](const auto& other){return other.data.part.name==part->name;}))Remove(std::size_t(part-parts.begin()));
    for(const auto id:t.data.lots) {
        const auto lot=std::ranges::find(item_lots,id,&ItemLot::id);if(lot==item_lots.end())continue;
        if(std::ranges::any_of(treasures,[&](const auto& other){return std::ranges::find(other.data.lots,id)!=other.data.lots.end();}))continue;
        if(std::ranges::any_of(quests,[&](const auto& q){return q.reward_lot==id;}) || std::ranges::any_of(event_edits,[&](const auto& e){return e.value==std::uint32_t(id);}))continue;
        const auto index=std::size_t(lot-item_lots.begin());history.push_back({index,*lot,std::nullopt});++cursor;item_lots.erase(lot);
    }
    Group(first);return true;
}
bool Document::RemoveRegion(std::size_t i) {
    const auto& region=regions.at(i);if(!region.added)throw std::runtime_error("Only new waypoints can be removed");
    for(const auto& route:routes)for(const auto& point:route.data.points)if(point.index==region.key)throw std::runtime_error("Remove this waypoint from its routes first");
    for(const auto& p:parts)if(p.enemy)for(const auto& point:p.enemy->move_points)if(point.index==region.key)throw std::runtime_error("Remove this waypoint from character move points first");
    history.resize(cursor);history.push_back({i,region,std::nullopt});++cursor;regions.erase(regions.begin()+std::ptrdiff_t(i));return true;
}
bool Document::RemoveRoute(std::size_t i) {
    const auto& route=routes.at(i);if(!route.added)throw std::runtime_error("Only new routes can be removed");
    for(const auto& p:parts)if(p.enemy && p.enemy->patrol.index==route.key)throw std::runtime_error("Unassign this route from its characters first");
    history.resize(cursor);history.push_back({i,route,std::nullopt});++cursor;routes.erase(routes.begin()+std::ptrdiff_t(i));return true;
}
std::int32_t Document::NewEntityID(std::int32_t source_id) {
    ReserveSiblings();
    int block=source_id>0?source_id/10000:0;
    const auto map=source_map.filename().string();
    if(!block && map.size()>=12 && ValidMapName(map.substr(0,12)))
        block=std::stoi(map.substr(1,2))*10+std::stoi(map.substr(4,2));
    if(!block && !reserved_entities.empty())block=*reserved_entities.begin()/10000;
    const std::int64_t first=std::int64_t(block)*10000+9000;
    if(first+999>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("No free entity identifier range");
    auto allocated=std::int32_t(first);
    while(reserved_entities.contains(allocated) && allocated<first+1000)++allocated;
    if(allocated>=first+1000)throw std::runtime_error("The area's 9000-9999 entity identifier range is full");
    reserved_entities.insert(allocated);return allocated;
}
std::size_t Document::Duplicate(std::size_t i,bool activate_object) {
    Part copy=parts.at(i);ReserveSiblings();
    if(activate_object && copy.type==9)copy.type=1;
    const char prefix=copy.kind==PartKind::Enemy?'c':'o';
    if(copy.model.size()<2 || copy.model[0]!=prefix || copy.model.find_first_not_of("0123456789",1)!=std::string::npos)
        throw std::runtime_error("Cannot duplicate this model identifier");
    // The standard model_NNNN naming convention is also understood by the game.
    for(int n=0;;++n) {
        if(n>=100000)throw std::runtime_error("No free placement name");
        const auto number=std::to_string(n);
        copy.name=copy.model+"_"+std::string(number.size()<4?4-number.size():0,'0')+number;
        if(!reserved_names.contains(copy.name))break;
    }
    auto next=[](const auto& used) {
        if(!used.empty() && *used.rbegin()==std::numeric_limits<std::int32_t>::max())throw std::runtime_error("No free placement identifier");
        return used.empty()?0:std::max(0,*used.rbegin()+1);
    };
    copy.instance_id=next(reserved_instances[copy.model]);
    const auto type=copy.type;
    copy.type_id=next(reserved_types[type]);
    copy.entity_id=NewEntityID(copy.entity_id);
    // Fail before creating a copy when the source cannot be relocated as one record.
    const auto end=record_ends.at(copy.record);
    if(Get<std::int64_t>(raw,copy.record+0xb0)<=0 ||
       Offset(raw,copy.record+0xb0,copy.record)+16>end)throw std::runtime_error("Source placement has no valid entity data");
    if(copy.enemy) {copy.enemy->entity_id=copy.entity_id;copy.enemy->instance_id=copy.instance_id;}
    copy.added=true;copy.original=copy.transform;
    const auto index=parts.size();
    history.resize(cursor);history.push_back({index,std::nullopt,copy});++cursor;parts.push_back(copy);
    reserved_names.insert(copy.name);reserved_entities.insert(copy.entity_id);
    reserved_instances[copy.model].insert(copy.instance_id);reserved_types[type].insert(copy.type_id);
    return index;
}
std::size_t Document::DuplicateAt(std::size_t i,const Transform& transform) {
    Check(transform);const auto& p=parts.at(i);if(p.kind==PartKind::Enemy && transform.scale!=p.original.scale)throw std::runtime_error("Enemy scale is preserved; edit position and rotation only");
    const auto first=cursor;const auto copy=Duplicate(i);Set(copy,transform);Group(first);return copy;
}
bool Document::Remove(std::size_t i) {
    if(!parts.at(i).added)throw std::runtime_error("Only copies created in this session can be removed");
    for(const auto& t:treasures)if(t.data.part.name==parts[i].name)throw std::runtime_error("Remove the attached treasure before its part");
    history.resize(cursor);history.push_back({i,parts[i],std::nullopt});++cursor;
    parts.erase(parts.begin()+std::ptrdiff_t(i));return true;
}
void Document::Apply(const Edit& e,bool forward) {
    const auto& from=forward?e.before:e.after;const auto& to=forward?e.after:e.before;
    std::visit([&](const auto& entry){
        using T=std::decay_t<decltype(entry)>;
        auto& list=[&]() -> auto& {if constexpr(std::is_same_v<T,Part>)return parts;else if constexpr(std::is_same_v<T,Region>)return regions;else if constexpr(std::is_same_v<T,Route>)return routes;else if constexpr(std::is_same_v<T,Treasure>)return treasures;else if constexpr(std::is_same_v<T,ItemLot>)return item_lots;else if constexpr(std::is_same_v<T,ScriptOperand>)return event_edits;else return quests;}();
        if(from && to)list.at(e.index)=std::get<T>(*to);
        else if(to)list.insert(list.begin()+std::ptrdiff_t(e.index),std::get<T>(*to));
        else list.erase(list.begin()+std::ptrdiff_t(e.index));
    },from?*from:*to);
}
void Document::Group(std::size_t first) {const auto group=++next_group;for(auto i=first;i<cursor;++i)history[i].group=group;}
bool Document::Undo() {
    if(!cursor)return false;const auto group=history[cursor-1].group;
    do{Apply(history[--cursor],false);}while(group && cursor && history[cursor-1].group==group);return true;
}
bool Document::Redo() {
    if(cursor==history.size())return false;const auto group=history[cursor].group;
    do{Apply(history[cursor++],true);}while(group && cursor<history.size() && history[cursor].group==group);return true;
}
std::vector<std::uint8_t> Document::Inflated() const {
    auto b=raw;
    const auto structural=std::ranges::any_of(parts,&Part::added) || std::ranges::any_of(regions,&Region::added) ||
        std::ranges::any_of(routes,&Route::added) || std::ranges::any_of(treasures,&Treasure::added);
    const auto& table=*std::ranges::find(sections,std::string("PARTS_PARAM_ST"),&Section::name);
    struct FullPart {std::uint32_t type;std::string name;};std::vector<FullPart> full_parts;
    for(const auto p:table.entries)full_parts.push_back({Get<std::uint32_t>(raw,p+0x14),Name(raw,Offset(raw,p+8,p))});
    for(const auto& p:parts)if(p.added)full_parts.push_back({p.type,p.name});
    if(structural)std::stable_sort(full_parts.begin(),full_parts.end(),[](const auto& a,const auto& b){return a.type<b.type;});
    std::map<std::string,int> part_indices;for(std::size_t i=0;i<full_parts.size();++i)part_indices[full_parts[i].name]=int(i);
    std::map<int,int> region_indices,route_indices;
    for(std::size_t n=0;n<regions.size();++n)region_indices[regions[n].key]=int(n);
    for(std::size_t n=0;n<routes.size();++n)route_indices[routes[n].key]=int(n);
    auto index=[&](const Reference& ref,const auto& indices) {
        if(auto found=indices.find(ref.index);found!=indices.end())return found->second;
        if(ref.index>=0 && std::size_t(ref.index)<indices.size())throw std::runtime_error("An unresolved reference would target a new record");
        return ref.index;
    };
    auto patch_part=[&](auto& bytes,std::size_t p,const Part& part) {
        Put(bytes,p+0x28,part.transform.position);Put(bytes,p+0x34,part.transform.rotation);Put(bytes,p+0x40,part.transform.scale);
        if(!part.enemy)return;const auto& e=*part.enemy;const auto data=p+Get<std::int64_t>(bytes,p+0xb8);
        Put(bytes,data+8,e.think_param);Put(bytes,data+12,e.npc_param);Put(bytes,data+16,e.talk_id);
        Put(bytes,data+20,e.patrol_type);Put(bytes,data+22,e.fallback_platoon);Put(bytes,data+24,e.chara_init);
        Put<std::int32_t>(bytes,data+28,e.collision.index);Put<std::int16_t>(bytes,data+32,index(e.patrol,route_indices));
        for(int n=0;n<8;++n)Put<std::int16_t>(bytes,data+40+n*2,index(e.move_points[n],region_indices));
        Put(bytes,data+56,e.init_anim);Put(bytes,data+60,e.damage_anim);
    };
    auto patch_route=[&](auto& bytes,std::size_t p,const Route& r) {
        const auto data=p+Get<std::int64_t>(bytes,p+0x20);Put(bytes,data,r.data.type);
        for(int n=0;n<32;++n)Put<std::int16_t>(bytes,data+16+n*2,index(r.data.points[n],region_indices));
    };
    auto patch_treasure=[&](auto& bytes,std::size_t p,const Treasure& t) {
        const auto data=p+Get<std::int64_t>(bytes,p+0x20);
        const auto found=part_indices.find(t.data.part.name);
        if(found!=part_indices.end())Put<std::int32_t>(bytes,data+8,found->second);
        else if(structural && t.data.part.index!=-1)throw std::runtime_error("Treasure part reference is unresolved");
        for(int n=0;n<3;++n)Put(bytes,data+0x10+n*4,t.data.lots[n]);
        Put(bytes,data+0x38,t.data.action_button);Put<std::uint8_t>(bytes,data+0x40,t.data.in_chest);Put<std::uint8_t>(bytes,data+0x41,t.data.disabled);
    };
    for(const auto& o:parts)if(!o.added)patch_part(b,o.record,o);
    for(const auto& r:regions)if(!r.added){Put(b,r.record+0x14,r.transform.position);Put(b,r.record+0x20,r.transform.rotation);}
    for(const auto& r:routes)if(!r.added)patch_route(b,r.record,r);
    for(const auto& t:treasures)if(!t.added)patch_treasure(b,t.record,t);
    const auto count=std::ranges::count(parts,true,&Part::added);
    if(!structural)return b;
    if(table.entries.size()+count>=100000)throw std::runtime_error("Too many map placements");
    struct Record { std::size_t source; const Part* copy=nullptr; const Region* region=nullptr; const Route* route=nullptr; const Treasure* treasure=nullptr; };
    std::vector<Record> ordered;
    for(auto p:table.entries)ordered.push_back({p});
    for(const auto& o:parts)if(o.added)ordered.push_back({o.record,&o});
    // Retail loading needs contiguous part type groups. Keep each original group's
    // order, append its copies, and remap all full-part indices to their new targets.
    std::stable_sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){
        return (a.copy?a.copy->type:Get<std::uint32_t>(raw,a.source+0x14))<(b.copy?b.copy->type:Get<std::uint32_t>(raw,b.source+0x14));
    });
    std::map<std::size_t,std::int32_t> new_indices;
    std::map<int,int> added_models;
    std::map<std::uint32_t,std::int64_t> next_type_ids;
    for(auto p:table.entries) {
        const auto type=Get<std::uint32_t>(raw,p+0x14);
        next_type_ids[type]=std::max(next_type_ids[type],std::int64_t(Get<std::int32_t>(raw,p+0x18))+1);
    }
    for(std::size_t n=0;n<ordered.size();++n) {
        if(!ordered[n].copy)new_indices[ordered[n].source]=std::int32_t(n);
        else ++added_models[Get<std::int32_t>(raw,ordered[n].source+0x1c)];
    }
    auto remap=[&](std::vector<std::uint8_t>& data,std::size_t field) {
        auto index=Get<std::int32_t>(data,field);if(index==-1)return;
        if(index<0 || std::size_t(index)>=table.entries.size())throw std::runtime_error("Cannot duplicate a map with an unresolved part reference");
        Put(data,field,new_indices.at(table.entries[index]));
    };
    auto local=[&](const auto& data,std::size_t field,std::size_t length) {
        const auto offset=Get<std::int64_t>(data,field);
        if(offset<=0 || std::uint64_t(offset)>data.size() || length>data.size()-offset)
            throw std::runtime_error("Invalid data offset while rebuilding map references");
        return std::size_t(offset);
    };
    std::vector<std::uint8_t> result(raw.begin(),raw.begin()+16);
    std::size_t previous_next=0;
    std::int64_t next_treasure_id=0;
    for(const auto& section:sections)if(section.name=="EVENT_PARAM_ST")for(const auto p:section.entries)if(Get<std::uint32_t>(raw,p+12)==4)next_treasure_id=std::max(next_treasure_id,std::int64_t(Get<std::int32_t>(raw,p+16))+1);
    for(const auto& s:sections) {
        if(s.name!="PARTS_PARAM_ST" && s.name!="MODEL_PARAM_ST" && s.name!="EVENT_PARAM_ST" && s.name!="POINT_PARAM_ST")
            throw std::runtime_error("Cannot duplicate maps with an unknown section");
        std::vector<Record> records;
        if(s.name=="PARTS_PARAM_ST")records=ordered;
        else for(auto p:s.entries)records.push_back({p});
        if(s.name=="POINT_PARAM_ST")for(const auto& r:regions)if(r.added)records.push_back({0,nullptr,&r});
        if(s.name=="EVENT_PARAM_ST") {
            for(const auto& r:routes)if(r.added)records.push_back({0,nullptr,nullptr,&r});
            for(const auto& t:treasures)if(t.added)records.push_back({t.record,nullptr,nullptr,nullptr,&t});
            std::stable_sort(records.begin(),records.end(),[&](const auto& a,const auto& b){
                return (a.route?14:a.treasure?4:Get<std::uint32_t>(raw,a.source+12))<(b.route?14:b.treasure?4:Get<std::uint32_t>(raw,b.source+12));
            });
        }
        const auto at=result.size();if(previous_next)Put<std::int64_t>(result,previous_next,at);
        result.resize(at+16+(records.size()+1)*8,0);
        Put<std::uint32_t>(result,at,3);Put<std::uint32_t>(result,at+4,records.size()+1);
        Put<std::int64_t>(result,at+8,result.size());
        for(unsigned char c:s.name){result.push_back(c);result.push_back(0);}result.push_back(0);result.push_back(0);
        result.resize((result.size()+7)&~std::size_t(7),0);
        for(std::size_t n=0;n<records.size();++n) {
            const auto& record=records[n];const auto p=record.source;
            std::vector<std::uint8_t> data;
            if(record.region)data=PointRecord(*record.region,int(n));
            else if(record.route){data=PatrolRecord(*record.route,route_indices.at(record.route->key));patch_route(data,0,*record.route);}
            else {const auto& bytes=record.copy || record.treasure?raw:b;data.assign(bytes.begin()+std::ptrdiff_t(p),bytes.begin()+std::ptrdiff_t(record_ends.at(p)));}
            if(record.treasure) {
                const auto& t=*record.treasure;Put<std::int64_t>(data,0,data.size());AppendName(data,t.name);Put(data,8,t.event_id);
                if(next_treasure_id>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("Treasure type identifier limit reached");
                Put<std::int32_t>(data,16,next_treasure_id++);Put(data,local(data,0x18,12)+8,t.entity_id);patch_treasure(data,0,t);
            }
            if(record.copy) {
                const auto& o=*record.copy;Put<std::int64_t>(data,8,data.size());
                AppendName(data,o.name);Put(data,0x10,o.instance_id);
                const auto type=o.type;Put<std::uint32_t>(data,0x14,type);const auto type_id=next_type_ids[type]++;
                if(type_id>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("Part type identifier limit reached");
                Put<std::int32_t>(data,0x18,type_id);Put(data,local(data,0xb0,16),o.entity_id);patch_part(data,0,o);
            }
            if(s.name=="PARTS_PARAM_ST") {
                const auto type=Get<std::uint32_t>(data,0x14);
                switch(type) {
                    case 1:case 9: if(Get<std::int64_t>(data,0xb8))remap(data,local(data,0xb8,12)+8);break;
                    case 2:case 10:remap(data,local(data,0xb8,32)+28);break;
                    case 0:case 4:case 5:case 8:case 11:case 0xffffffff:break;
                    default:throw std::runtime_error("Unknown part type while rebuilding references");
                }
                // ConnectCollision's index addresses only collision parts; that subtype
                // retains its internal order and is not part of this full-index remap.
            } else if(s.name=="EVENT_PARAM_ST") {
                if(Get<std::int64_t>(data,0x18))remap(data,local(data,0x18,12));
                const auto type=Get<std::uint32_t>(data,12);
                if(type==5) {const auto t=local(data,0x20,0xd0);for(int i=0;i<32;++i)remap(data,t+0x50+i*4);}
                else if(type==7)remap(data,local(data,0x20,8)+4); // ObjAct
                else if(type==16) {const auto t=local(data,0x20,0x90);for(int i=0;i<32;++i)remap(data,t+0x10+i*4);}
                else if(type!=1 && type!=2 && type!=4 && type!=6 && type!=8 && type!=9 && type!=10 && type!=11 && type!=13 && type!=14 && type!=15 && type!=17 && type!=0xffffffff)
                    throw std::runtime_error("Unknown event type while rebuilding references");
            } else if(s.name=="MODEL_PARAM_ST" && added_models.contains(int(n))) {
                int instances=added_models.at(int(n));
                for(auto part:table.entries)if(Get<std::int32_t>(raw,part+0x1c)==int(n))++instances;
                Put<std::int32_t>(data,0x18,instances);
            }
            Put<std::int64_t>(result,at+16+n*8,result.size());result.insert(result.end(),data.begin(),data.end());
            result.resize((result.size()+7)&~std::size_t(7),0);
            if(result.size()>64*1024*1024)throw std::runtime_error("Map exceeds 64 MiB limit");
        }
        previous_next=at+16+records.size()*8;
    }
    return result;
}
std::vector<std::uint8_t> Document::Save() const {
    if(!Changed()) return source;
    auto b=Inflated();if(b==raw)return source;if(!dcx) {if(Document::Read(b).Inflated()!=b)throw std::runtime_error("Export verification failed");return b;}
    mz_ulong length=mz_compressBound(b.size());std::vector<std::uint8_t> packed(length);
    if(mz_compress2(packed.data(),&length,b.data(),b.size(),9)!=MZ_OK) throw std::runtime_error("Map compression failed");
    std::vector<std::uint8_t> result(source.begin(),source.begin()+0x4c);
    PutBE(result,0x1c,std::uint32_t(b.size()));PutBE(result,0x20,std::uint32_t(length));
    result.insert(result.end(),packed.begin(),packed.begin()+std::ptrdiff_t(length));
    if(Document::Read(result).Inflated()!=b) throw std::runtime_error("Export verification failed");
    return result;
}
std::filesystem::path Document::Export(const std::filesystem::path& folder,const std::string& map) const {
    namespace fs=std::filesystem;
    if(!ValidMapName(map)) throw std::runtime_error("Invalid map name");
    if(!source_game.empty()) {
        const auto target=fs::weakly_canonical(fs::absolute(folder));
        const auto result=std::mismatch(source_game.begin(),source_game.end(),target.begin(),target.end());
        if(result.first==source_game.end()) throw std::runtime_error("Export outside the original game folder");
    }
    const auto bytes=Save();
    if(!item_lots.empty() && !parameters)throw std::runtime_error("Item parameters are unavailable");
    const auto parameter_bytes=item_lots.empty()?std::vector<std::uint8_t>{}:parameters->SaveLots(item_lots);
    std::map<std::string,std::vector<std::uint8_t>> event_files;
    for(const auto& [name,file]:scripts) {
        std::vector<ScriptOperand> edits;for(const auto& e:event_edits)if(e.script==name)edits.push_back(e);
        std::vector<PickupQuest> added;for(const auto& q:quests)if(q.script==name)added.push_back(q);
        if(!edits.empty() || !added.empty())event_files[name]=file->Save(edits,added);
    }
    if(!folder.parent_path().empty()) fs::create_directories(folder.parent_path());
    if(!fs::create_directory(folder)) throw std::runtime_error("Choose a new export folder; an existing folder will not be replaced");
    try {
        auto relative=fs::path("dvdroot_ps4/map/MapStudio")/(map+(dcx?".msb.dcx":".msb"));
        fs::create_directories((folder/relative).parent_path());WriteExclusive(folder/relative,bytes);
        if(!parameter_bytes.empty()) {const auto path=folder/"dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx";fs::create_directories(path.parent_path());WriteExclusive(path,parameter_bytes);}
        for(const auto& [name,data]:event_files){const auto path=folder/"dvdroot_ps4/event"/name;fs::create_directories(path.parent_path());WriteExclusive(path,data);}
        const std::string note="Bloodborne map editor mod\nMap: "+map+"\nEnable this folder in Mods & Cheats. Reload the area to apply it.\n"+
            (parameter_bytes.empty()?std::string():"Includes a full gameparam binder with private item-lot rows. Combine parameter changes before using other gameparam mods.\n");
        WriteExclusive(folder/"README.txt",std::span(reinterpret_cast<const std::uint8_t*>(note.data()),note.size()));
        return folder/relative;
    } catch(...) {fs::remove_all(folder);throw;}
}
BbMapArt::Vec3 WorldPoint(const Transform& t,const float* p) {
    constexpr float rad=3.14159265358979323846f/180;
    float x=p[0]*t.scale[0],y=p[1]*t.scale[1],z=p[2]*t.scale[2];
    auto turn=[](float& a,float& b,float angle){const float v=a*std::cos(angle)-b*std::sin(angle);b=a*std::sin(angle)+b*std::cos(angle);a=v;};
    turn(y,z,t.rotation[0]*rad);turn(x,y,t.rotation[2]*rad);turn(z,x,t.rotation[1]*rad);
    return {x+t.position[0],y+t.position[1],z+t.position[2]};
}
} // namespace BbMapEditor
