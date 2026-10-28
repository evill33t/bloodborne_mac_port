// SPDX-License-Identifier: GPL-2.0-or-later
// MSBB layout checked against Smithbox/SoulsFormats MSBB/PartsParam.cs (MIT):
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
    std::set<std::string> names;
    for(auto p:all_parts) {
        auto type=Get<std::uint32_t>(b,p+0x14);
        if(type!=1 && type!=2 && type!=10) continue; // Existing Object, Enemy and DummyEnemy placements.
        if(p>b.size() || b.size()-p<0xd0) throw std::runtime_error("Truncated object header");
        const auto end=end_of(p);
        if(end-p<0xd0) throw std::runtime_error("Overlapping object records");
        const auto name_at=Offset(b,p+8,p);
        if(name_at<p+0xd0 || name_at>=end) throw std::runtime_error("Invalid object name offset");
        auto name=Name(b.first(end),name_at);
        auto model=Get<std::int32_t>(b,p+0x1c);
        if(model<0 || std::size_t(model)>=models.size() || !names.insert(name).second)
            throw std::runtime_error("Invalid or duplicate object reference");
        Part o;o.name=name;o.model=models[model];o.record=p;
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
            e.collision=ref(Get<std::int32_t>(b,data+28),all_parts,5);
            e.patrol=ref(Get<std::int16_t>(b,data+32),patrols);
            for(int i=0;i<8;++i)e.move_points[i]=ref(Get<std::int16_t>(b,data+40+i*2),lists.at("POINT_PARAM_ST"));
            e.init_anim=Get<std::int32_t>(b,data+56);e.damage_anim=Get<std::int32_t>(b,data+60);
            o.enemy=std::move(e);
        }
        d.parts.push_back(std::move(o));
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
        } else if(s.name=="EVENT_PARAM_ST") entity(0x18,8);
        else if(s.name=="POINT_PARAM_ST") entity(0x48,0);
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
            for(const auto& [model,ids]:sibling.reserved_instances)reserved_instances[model].insert(ids.begin(),ids.end());
        }
    }
    reserved_siblings=true;
}
bool Document::Changed() const {
    return std::ranges::any_of(parts,[](const Part& o){return o.added || o.transform!=o.original;});
}
bool Document::Set(std::size_t i,const Transform& t) {
    Check(t);auto& o=parts.at(i);
    if(o.kind==PartKind::Enemy && t.scale!=o.original.scale) throw std::runtime_error("Enemy scale is preserved; edit position and rotation only");
    if(o.transform==t) return false;
    auto after=o;after.transform=t;
    history.resize(cursor);history.push_back({i,o,after});++cursor;o=std::move(after);return true;
}
std::size_t Document::Duplicate(std::size_t i) {
    Part copy=parts.at(i);ReserveSiblings();
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
    const auto type=Get<std::uint32_t>(raw,copy.record+0x14);
    copy.type_id=next(reserved_types[type]);
    int block=copy.entity_id>0?copy.entity_id/10000:0;
    const auto map=source_map.filename().string();
    if(!block && map.size()>=12 && ValidMapName(map.substr(0,12)))
        block=std::stoi(map.substr(1,2))*10+std::stoi(map.substr(4,2));
    if(!block && !reserved_entities.empty())block=*reserved_entities.begin()/10000;
    const std::int64_t first=std::int64_t(block)*10000+9000;
    if(first+999>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("No free entity identifier range");
    copy.entity_id=std::int32_t(first);
    while(reserved_entities.contains(copy.entity_id) && copy.entity_id<first+1000)++copy.entity_id;
    if(copy.entity_id>=first+1000)throw std::runtime_error("The area's 9000-9999 entity identifier range is full");
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
bool Document::Remove(std::size_t i) {
    if(!parts.at(i).added)throw std::runtime_error("Only copies created in this session can be removed");
    history.resize(cursor);history.push_back({i,parts[i],std::nullopt});++cursor;
    parts.erase(parts.begin()+std::ptrdiff_t(i));return true;
}
void Document::Apply(const Edit& e,bool forward) {
    const auto& from=forward?e.before:e.after;const auto& to=forward?e.after:e.before;
    if(from && to)parts.at(e.index)=*to;
    else if(to)parts.insert(parts.begin()+std::ptrdiff_t(e.index),*to);
    else parts.erase(parts.begin()+std::ptrdiff_t(e.index));
}
bool Document::Undo() {if(!cursor)return false;Apply(history[--cursor],false);return true;}
bool Document::Redo() {if(cursor==history.size())return false;Apply(history[cursor++],true);return true;}
std::vector<std::uint8_t> Document::Inflated() const {
    auto b=raw;
    for(const auto& o:parts) if(!o.added) {
        Put(b,o.record+0x28,o.transform.position);Put(b,o.record+0x34,o.transform.rotation);Put(b,o.record+0x40,o.transform.scale);
    }
    const auto count=std::ranges::count(parts,true,&Part::added);if(!count)return b;
    const auto& table=*std::ranges::find(sections,std::string("PARTS_PARAM_ST"),&Section::name);
    if(table.entries.size()+count>=100000)throw std::runtime_error("Too many map placements");
    struct Record { std::size_t source; const Part* copy=nullptr; };
    std::vector<Record> ordered;
    for(auto p:table.entries)ordered.push_back({p});
    for(const auto& o:parts)if(o.added)ordered.push_back({o.record,&o});
    // Retail loading needs contiguous part type groups. Keep each original group's
    // order, append its copies, and remap all full-part indices to their new targets.
    std::stable_sort(ordered.begin(),ordered.end(),[&](const auto& a,const auto& b){
        return Get<std::uint32_t>(raw,a.source+0x14)<Get<std::uint32_t>(raw,b.source+0x14);
    });
    std::map<std::size_t,std::int32_t> new_indices;
    std::map<int,int> added_models;
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
    for(const auto& s:sections) {
        if(s.name!="PARTS_PARAM_ST" && s.name!="MODEL_PARAM_ST" && s.name!="EVENT_PARAM_ST" && s.name!="POINT_PARAM_ST")
            throw std::runtime_error("Cannot duplicate maps with an unknown section");
        std::vector<Record> records;
        if(s.name=="PARTS_PARAM_ST")records=ordered;
        else for(auto p:s.entries)records.push_back({p});
        const auto at=result.size();if(previous_next)Put<std::int64_t>(result,previous_next,at);
        result.resize(at+16+(records.size()+1)*8,0);
        Put<std::uint32_t>(result,at,3);Put<std::uint32_t>(result,at+4,records.size()+1);
        Put<std::int64_t>(result,at+8,result.size());
        for(unsigned char c:s.name){result.push_back(c);result.push_back(0);}result.push_back(0);result.push_back(0);
        result.resize((result.size()+7)&~std::size_t(7),0);
        for(std::size_t n=0;n<records.size();++n) {
            const auto& record=records[n];const auto p=record.source,end=record_ends.at(p);
            const auto& bytes=record.copy?raw:b;
            std::vector<std::uint8_t> data(bytes.begin()+std::ptrdiff_t(p),bytes.begin()+std::ptrdiff_t(end));
            if(record.copy) {
                const auto& o=*record.copy;Put<std::int64_t>(data,8,data.size());
                for(unsigned char c:o.name){data.push_back(c);data.push_back(0);}data.push_back(0);data.push_back(0);
                Put(data,0x10,o.instance_id);Put(data,0x18,o.type_id);Put(data,local(data,0xb0,16),o.entity_id);
                Put(data,0x28,o.transform.position);Put(data,0x34,o.transform.rotation);Put(data,0x40,o.transform.scale);
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
                if(type==4)remap(data,local(data,0x20,12)+8); // Treasure
                else if(type==5) {const auto t=local(data,0x20,0xd0);for(int i=0;i<32;++i)remap(data,t+0x50+i*4);}
                else if(type==7)remap(data,local(data,0x20,8)+4); // ObjAct
                else if(type==16) {const auto t=local(data,0x20,0x90);for(int i=0;i<32;++i)remap(data,t+0x10+i*4);}
                else if(type!=1 && type!=2 && type!=6 && type!=8 && type!=9 && type!=10 && type!=11 && type!=13 && type!=14 && type!=15 && type!=17 && type!=0xffffffff)
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
    auto b=Inflated();if(!dcx) return b;
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
    if(!folder.parent_path().empty()) fs::create_directories(folder.parent_path());
    if(!fs::create_directory(folder)) throw std::runtime_error("Choose a new export folder; an existing folder will not be replaced");
    try {
        auto relative=fs::path("dvdroot_ps4/map/MapStudio")/(map+(dcx?".msb.dcx":".msb"));
        fs::create_directories((folder/relative).parent_path());WriteExclusive(folder/relative,bytes);
        const std::string note="Bloodborne map editor mod\nMap: "+map+"\nEnable this folder in Mods & Cheats. Reload the area to apply it.\n";
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
