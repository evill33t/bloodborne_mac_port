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
    std::set<std::size_t> visited,record_offsets;
    std::size_t at=16;
    while(at) {
        if(!visited.insert(at).second || visited.size()>16) throw std::runtime_error("Cyclic map sections");
        const auto count=Get<std::uint32_t>(b,at+4);
        if(!count || count>100000 || at>b.size() || b.size()-at<16 || count>(b.size()-at-16)/8)
            throw std::runtime_error("Invalid map section count");
        auto name=Name(b,Offset(b,at+8));
        if(lists.contains(name)) throw std::runtime_error("Duplicate map section");
        auto& entries=lists[name];
        for(std::uint32_t i=0;i+1<count;++i) {
            auto p=Offset(b,at+16+i*8);
            if(!record_offsets.insert(p).second) throw std::runtime_error("Aliased map records");
            entries.push_back(p);
        }
        if(Get<std::uint32_t>(b,at)!=3) throw std::runtime_error("Unsupported map section version");
        const auto next=Get<std::int64_t>(b,at+16+(count-1)*8);
        if(next<0 || std::uint64_t(next)>=b.size()) throw std::runtime_error("Invalid next section");
        at=std::size_t(next);
    }
    for(auto name:{"MODEL_PARAM_ST","PARTS_PARAM_ST","EVENT_PARAM_ST","POINT_PARAM_ST"})
        if(!lists.contains(name)) throw std::runtime_error("Incomplete map sections");
    std::vector<std::string> models;
    for(auto p:lists.at("MODEL_PARAM_ST")) models.push_back(Name(b,Offset(b,p,p)));
    auto end_of=[&](std::size_t p) {auto next=record_offsets.upper_bound(p);return next==record_offsets.end()?b.size():*next;};
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
        auto next=record_offsets.upper_bound(p);
        const auto end=next==record_offsets.end()?b.size():*next;
        if(end-p<0xd0) throw std::runtime_error("Overlapping object records");
        const auto name_at=Offset(b,p+8,p);
        if(name_at<p+0xd0 || name_at>=end) throw std::runtime_error("Invalid object name offset");
        auto name=Name(b.first(end),name_at);
        auto model=Get<std::int32_t>(b,p+0x1c);
        if(model<0 || std::size_t(model)>=models.size() || !names.insert(name).second)
            throw std::runtime_error("Invalid or duplicate object reference");
        Part o;o.name=name;o.model=models[model];o.record=p;
        o.original.position=Get<BbMapArt::Vec3>(b,p+0x28);
        o.original.rotation=Get<BbMapArt::Vec3>(b,p+0x34);
        o.original.scale=Get<BbMapArt::Vec3>(b,p+0x40);Check(o.original);o.transform=o.original;
        if(type==2 || type==10) {
            o.kind=PartKind::Enemy;EnemyData e;e.dummy=type==10;
            const auto entity=local(p,0xb0,16),data=local(p,0xb8,64);
            e.entity_id=Get<std::int32_t>(b,entity);e.instance_id=Get<std::int32_t>(b,p+0x10);
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
    return d;
}
bool Document::Changed() const {
    return std::ranges::any_of(parts,[](const Part& o){return o.transform!=o.original;});
}
bool Document::Set(std::size_t i,const Transform& t) {
    Check(t);auto& o=parts.at(i);
    if(o.kind==PartKind::Enemy && t.scale!=o.original.scale) throw std::runtime_error("Enemy scale is preserved; edit position and rotation only");
    if(o.transform==t) return false;
    history.resize(cursor);history.push_back({i,o.transform,t});++cursor;o.transform=t;return true;
}
bool Document::Undo() {if(!cursor) return false;auto& e=history[--cursor];parts[e.index].transform=e.before;return true;}
bool Document::Redo() {if(cursor==history.size()) return false;auto& e=history[cursor++];parts[e.index].transform=e.after;return true;}
std::vector<std::uint8_t> Document::Inflated() const {
    auto b=raw;
    for(const auto& o:parts) {
        std::memcpy(b.data()+o.record+0x28,o.transform.position.data(),12);
        std::memcpy(b.data()+o.record+0x34,o.transform.rotation.data(),12);
        std::memcpy(b.data()+o.record+0x40,o.transform.scale.data(),12);
    }
    return b;
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
