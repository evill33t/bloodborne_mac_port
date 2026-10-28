// SPDX-License-Identifier: GPL-2.0-or-later
#include "project_file.h"
#include "miniz.h"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace BbMapEditor {
namespace {
constexpr std::size_t Limit=512*1024*1024,CountLimit=1000000;
constexpr char Magic[]="BBPROJ01";
constexpr const char* Params="param/gameparam/gameparam.parambnd.dcx";
constexpr const char* Definitions="event/common.emedf.dcx";
[[noreturn]] void Fail(const char* reason){throw std::runtime_error(std::string("Invalid editor project: ")+reason);}
std::string MapSource(const std::string& map){return "map/MapStudio/"+map+".msb.dcx";}
template<class T>struct Vector:std::false_type{};
template<class T,class A>struct Vector<std::vector<T,A>>:std::true_type{};
template<class T>struct Array:std::false_type{};
template<class T,std::size_t N>struct Array<std::array<T,N>>:std::true_type{};
template<class T>struct Set:std::false_type{};
template<class K,class C,class A>struct Set<std::set<K,C,A>>:std::true_type{};
template<class T>struct Map:std::false_type{};
template<class K,class V,class C,class A>struct Map<std::map<K,V,C,A>>:std::true_type{};
template<class T>struct Optional:std::false_type{};
template<class V>struct Optional<std::optional<V>>:std::true_type{};
template<class T>struct Variant:std::false_type{};
template<class...V>struct Variant<std::variant<V...>>:std::true_type{};
#define F(Type, ...) template<class I,class T> requires std::is_same_v<std::remove_cv_t<T>,Type> void Fields(I& io,T& v){io(__VA_ARGS__);}
F(Transform,v.position,v.rotation,v.scale)
F(Reference,v.index,v.name)
F(EnemyData,v.dummy,v.entity_id,v.instance_id,v.npc_param,v.think_param,v.talk_id,v.chara_init,v.init_anim,v.damage_anim,v.patrol_type,v.fallback_platoon,v.collision,v.patrol,v.move_points)
F(Part,v.name,v.model,v.record,v.entity_id,v.instance_id,v.type_id,v.type,v.added,v.original,v.transform,v.kind,v.enemy,v.original_enemy)
F(Region,v.name,v.record,v.key,v.entity_id,v.shape,v.added,v.original,v.transform)
F(RouteData,v.type,v.points)
F(Route,v.name,v.record,v.key,v.entity_id,v.event_id,v.added,v.original,v.data)
F(TreasureData,v.part,v.lots,v.action_button,v.in_chest,v.disabled)
F(Treasure,v.name,v.record,v.event_id,v.entity_id,v.added,v.original,v.data)
F(ItemLot,v.id,v.source_id,v.name,v.bytes)
F(ScriptOperand,v.script,v.at,v.width,v.type,v.original,v.value)
F(PickupQuest,v.script,v.treasure,v.event_id,v.trigger_flag,v.completion_flag,v.reward_lot)
F(CharacterImport,v.model,v.native_model,v.source_game,v.source_model,v.report,v.files,v.preview)
#undef F
// Explicit fields only: never serialize C++ padding, pointers, or string internals.
// Version 1 is little endian and uses 64-bit sizes on the native macOS target.
template<bool Reading> struct Archive {
    std::span<const std::uint8_t> input;
    std::vector<std::uint8_t> bytes;
    std::size_t at=0;
    explicit Archive(std::span<const std::uint8_t> b={}):input(b){}
    template<class...T>void operator()(T&...v){(One(v),...);}
    void Raw(void* p,std::size_t n){
        if constexpr(Reading){if(at>input.size() || n>input.size()-at)Fail("truncated data");std::memcpy(p,input.data()+at,n);at+=n;}
        else {if(n>Limit-bytes.size())Fail("file size limit");const auto* b=static_cast<const std::uint8_t*>(p);bytes.insert(bytes.end(),b,b+n);}
    }
    std::uint32_t Count(std::size_t size){
        if(size>CountLimit)Fail("container limit");std::uint32_t n=std::uint32_t(size);One(n);
        if(n>CountLimit || (Reading && n>input.size()-at))Fail("invalid container size");return n;
    }
    template<std::size_t N=0,class V>void Alternative(V& v,std::uint8_t tag){
        if constexpr(N<std::variant_size_v<V>){if(tag==N){v.template emplace<N>();One(std::get<N>(v));}else Alternative<N+1>(v,tag);}
        else Fail("unknown history entry");
    }
    template<class T>void One(T& value){
        using V=std::remove_cv_t<T>;
        if constexpr(std::is_same_v<V,bool>){std::uint8_t b=value?1:0;Raw(&b,1);if(b>1)Fail("invalid boolean");if constexpr(Reading)value=b!=0;}
        else if constexpr(std::is_arithmetic_v<V>){
            static_assert(sizeof(std::size_t)==8 && std::endian::native==std::endian::little);
            Raw(const_cast<V*>(&value),sizeof(V));
            if constexpr(std::is_floating_point_v<V>)if(!std::isfinite(value))Fail("non-finite number");
        }else if constexpr(std::is_enum_v<V>){std::underlying_type_t<V> n=static_cast<std::underlying_type_t<V>>(value);One(n);if constexpr(Reading)value=static_cast<V>(n);}
        else if constexpr(std::is_same_v<V,std::string>){
            const auto n=Count(value.size());if constexpr(Reading)value.resize(n);Raw(const_cast<char*>(value.data()),n);
            if(value.find('\0')!=std::string::npos)Fail("embedded null in text");
        }else if constexpr(Vector<V>::value){
            if constexpr(std::is_same_v<typename V::value_type,std::uint8_t>){
                std::uint64_t n=value.size();One(n);if(n>Limit || (Reading && n>input.size()-at))Fail("invalid byte blob");
                if constexpr(Reading)value.resize(n);Raw(const_cast<std::uint8_t*>(value.data()),n);
            }else {const auto n=Count(value.size());if constexpr(Reading){value.clear();for(std::uint32_t i=0;i<n;++i){typename V::value_type v{};One(v);value.push_back(std::move(v));}}else for(auto& v:value)One(v);}
        }else if constexpr(Array<V>::value){for(auto& v:value)One(v);}
        else if constexpr(Set<V>::value){const auto n=Count(value.size());if constexpr(Reading){value.clear();for(std::uint32_t i=0;i<n;++i){typename V::value_type v{};One(v);if(!value.insert(std::move(v)).second)Fail("duplicate set entry");}}else for(auto& v:value)One(v);}
        else if constexpr(Map<V>::value){const auto n=Count(value.size());if constexpr(Reading){value.clear();for(std::uint32_t i=0;i<n;++i){typename V::key_type k{};typename V::mapped_type v{};One(k);One(v);if(!value.emplace(std::move(k),std::move(v)).second)Fail("duplicate map entry");}}else for(auto& [k,v]:value){One(k);One(v);}}
        else if constexpr(Optional<V>::value){bool present=value.has_value();One(present);if constexpr(Reading){if(present)value.emplace();else value.reset();}if(present)One(*value);}
        else if constexpr(Variant<V>::value){std::uint8_t tag=std::uint8_t(value.index());One(tag);if constexpr(Reading)Alternative(value,tag);else std::visit([&](auto& v){One(v);},value);}
        else Fields(*this,value);
    }
    void End(){if(at!=input.size())Fail("trailing data");}
};
template<class T>auto Encoded(const T& value){Archive<false> writer;writer(value);return writer.bytes;}
void CheckTransform(const Transform& t){
    for(const auto* v:{&t.position,&t.rotation,&t.scale})for(float f:*v)if(!std::isfinite(f) || std::abs(f)>1000000)Fail("transform outside limits");
    for(float f:t.scale)if(std::abs(f)<.0001f || std::abs(f)>1000)Fail("scale outside limits");
}
void CheckName(const std::string& name){if(name.empty() || name.size()>1024 || name.find('\0')!=std::string::npos)Fail("invalid record name");}
bool Inside(const std::filesystem::path& path,const std::filesystem::path& root){
    auto fold=[](std::string text){for(char& c:text)if(static_cast<unsigned char>(c)<128)c=char(std::tolower(static_cast<unsigned char>(c)));return text;};
    auto p=path.begin();for(auto r=root.begin();r!=root.end();++r,++p)if(p==path.end() || fold(p->string())!=fold(r->string()))return false;return true;
}
void SourceKeys(const std::map<std::string,std::vector<std::uint8_t>>& sources,const std::string& map){
    if(!ValidMapName(map) || !sources.contains(MapSource(map)))Fail("map baseline missing");
    for(const auto& [name,bytes]:sources){
        if(bytes.empty() || (name!=MapSource(map) && name!=Params && name!=Definitions && !(name.starts_with("event/") && ValidEventName(name.substr(6)))))Fail("unknown or empty source baseline");
    }
}
}

Project Project::Capture(const std::filesystem::path& root,const std::string& name,const Document& d){
    Project result;result.game=std::filesystem::weakly_canonical(std::filesystem::absolute(root));result.map=name;
    result.character_imports=d.character_imports;
    result.sources[MapSource(name)]=d.source;
    if(d.parameters){const auto b=d.parameters->OriginalSource();result.sources[Params]={b.begin(),b.end()};}
    for(const auto& [key,file]:d.scripts){
        auto b=file->OriginalSource();result.sources["event/"+key]={b.begin(),b.end()};
        if(file->Definitions()){b=file->Definitions()->OriginalSource();auto& stored=result.sources[Definitions];if(!stored.empty() && !std::equal(stored.begin(),stored.end(),b.begin(),b.end()))Fail("inconsistent event definitions");stored.assign(b.begin(),b.end());}
    }
    SourceKeys(result.sources,name);Archive<false> io;
    io(d.reserved_names,d.reserved_region_names,d.reserved_route_names,d.reserved_treasure_names,d.reserved_lot_ids,d.reserved_pickup_flags,d.reserved_entities,d.reserved_instances,d.reserved_types,d.reserved_script_ids,d.next_region_key,d.next_route_key,d.next_group);
    auto count=io.Count(d.history.size());(void)count;
    for(const auto& edit:d.history)io(edit.index,edit.before,edit.after,edit.group);
    io(d.cursor);result.session=std::move(io.bytes);return result;
}
Document Project::OriginalDocument()const{
    SourceKeys(sources,map);auto d=Document::Read(Source(MapSource(map)));d.source_game=game;d.source_map=game/"dvdroot_ps4"/MapSource(map);return d;
}
std::span<const std::uint8_t> Project::Source(const std::string& relative)const{auto it=sources.find(relative);return it==sources.end()?std::span<const std::uint8_t>{}:it->second;}
std::vector<std::string> Project::EventNames()const{std::vector<std::string> names;for(const auto& [name,_]:sources)if(name.starts_with("event/") && ValidEventName(name.substr(6)))names.push_back(name.substr(6));return names;}

void Project::Restore(Document& document)const{
    auto d=OriginalDocument();d.parameters=document.parameters;d.scripts=document.scripts;
    std::size_t import_size=0;
    for(const auto& [name,assets]:character_imports){Document::ValidateCharacterImport(assets);if(name!=assets.model || !d.CanImportCharacterModel(name))Fail("import conflicts with an original model");import_size+=assets.preview.size();for(const auto& [_,b]:assets.files)import_size+=b.size();}
    if(import_size>256*1024*1024)Fail("character import asset budget");
    d.character_imports=character_imports;
    // Contexts must match the embedded baseline before any history can be applied.
    if(sources.contains(Params) && (!d.parameters || !std::ranges::equal(d.parameters->OriginalSource(),Source(Params))))Fail("item parameter baseline unavailable");
    for(const auto& name:EventNames())if(!d.scripts.contains(name) || !std::ranges::equal(d.scripts.at(name)->OriginalSource(),Source("event/"+name)))Fail("event baseline unavailable");
    // Only editable, typed instruction operands may be restored from history.
    std::map<std::string,std::map<std::size_t,std::vector<std::array<std::size_t,3>>>> operands;
    auto checked_operand=[&](const ScriptOperand& value){
        const auto script=d.scripts.find(value.script);if(script==d.scripts.end())Fail("unknown event script");const auto& file=*script->second;
        if(!operands.contains(value.script)){
            auto& positions=operands[value.script];
            for(std::size_t e=0;e<file.Events().size();++e)for(std::size_t i=0;i<file.Events()[e].instructions.size();++i){
                const auto& instruction=file.Events()[e].instructions[i];const auto* command=file.Command(instruction);if(!command)continue;
                for(std::size_t a=0;a<command->arguments.size();++a){const auto& arg=command->arguments[a];
                    if(!arg.width || arg.offset>instruction.length || arg.width>instruction.length-arg.offset || file.Parameterized(e,i,arg))continue;
                    positions[instruction.args+arg.offset].push_back({e,i,a});
                }
            }
        }
        const auto found=operands[value.script].find(value.at);if(found==operands[value.script].end())Fail("event operand is not editable");
        for(const auto& position:found->second){
            try{auto candidate=file.Edit(position[0],position[1],position[2],EventFile::Number(value));candidate.script=value.script;if(candidate==value)return;}catch(const std::exception&){}
        }
        Fail("event operand type or value mismatch");
    };
    const auto original=d;Archive<true> io(session);
    io(d.reserved_names,d.reserved_region_names,d.reserved_route_names,d.reserved_treasure_names,d.reserved_lot_ids,d.reserved_pickup_flags,d.reserved_entities,d.reserved_instances,d.reserved_types,d.reserved_script_ids,d.next_region_key,d.next_route_key,d.next_group);
    if(d.next_region_key<original.next_region_key || d.next_region_key>=INT32_MAX || d.next_route_key<original.next_route_key || d.next_route_key>=INT32_MAX || d.next_group==SIZE_MAX)Fail("invalid region allocator");
    auto contains=[](const auto& all,const auto& required){for(const auto& v:required)if(!all.contains(v))Fail("missing identifier reservation");};
    contains(d.reserved_names,original.reserved_names);contains(d.reserved_region_names,original.reserved_region_names);contains(d.reserved_route_names,original.reserved_route_names);contains(d.reserved_treasure_names,original.reserved_treasure_names);contains(d.reserved_entities,original.reserved_entities);
    const auto count=io.Count(0);std::size_t cursor=0;
    std::set<std::size_t> groups;std::size_t last_group=0;
    for(std::uint32_t i=0;i<count;++i){
        Document::Edit edit{};io(edit.index,edit.before,edit.after,edit.group);
        if((!edit.before && !edit.after) || (edit.before && edit.after && edit.before->index()!=edit.after->index()))Fail("invalid edit types");
        if(edit.group>d.next_group || (edit.group && edit.group!=last_group && !groups.insert(edit.group).second))Fail("invalid edit grouping");last_group=edit.group;
        std::visit([&](const auto& value){
            using T=std::decay_t<decltype(value)>;
            auto& list=[&]() -> auto& {if constexpr(std::is_same_v<T,Part>)return d.parts;else if constexpr(std::is_same_v<T,Region>)return d.regions;else if constexpr(std::is_same_v<T,Route>)return d.routes;else if constexpr(std::is_same_v<T,Treasure>)return d.treasures;else if constexpr(std::is_same_v<T,ItemLot>)return d.item_lots;else if constexpr(std::is_same_v<T,ScriptOperand>)return d.event_edits;else return d.quests;}();
            if(edit.index>list.size() || (edit.before && (edit.index==list.size() || Encoded(list[edit.index])!=Encoded(std::get<T>(*edit.before)))))Fail("history does not match its baseline");
            auto validate=[&](const T& v){
                if constexpr(std::is_same_v<T,Part>){
                    CheckName(v.name);CheckTransform(v.original);CheckTransform(v.transform);
                    const auto base=std::ranges::find(original.parts,v.record,&Part::record);
                    if(base==original.parts.end())Fail("invalid placement source");
                    const bool imported=v.added && bool(v.enemy) && character_imports.contains(v.model) && character_imports.at(v.model).native_model==base->model;
                    if((v.model!=base->model && !imported) || v.kind!=base->kind || !(v.type==base->type || (v.added && base->type==9 && v.type==1)) || v.original_enemy!=base->original_enemy || bool(v.enemy)!=bool(base->enemy))Fail("invalid placement source");
                    if(!v.added && (v.name!=base->name || v.entity_id!=base->entity_id || v.instance_id!=base->instance_id || v.type_id!=base->type_id || v.original!=base->original))Fail("changed original placement identity");
                    if(v.enemy && (v.enemy->entity_id!=v.entity_id || v.enemy->instance_id!=v.instance_id || v.enemy->dummy!=base->enemy->dummy || v.transform.scale!=v.original.scale))Fail("invalid character identity");
                    if(!d.reserved_names.contains(v.name) || (v.entity_id>0 && !d.reserved_entities.contains(v.entity_id)))Fail("unreserved placement identifier");
                    if(v.added && (!d.reserved_instances[v.model].contains(v.instance_id) || !d.reserved_types[v.type].contains(v.type_id)))Fail("unreserved placement instance");
                }else if constexpr(std::is_same_v<T,Region>){
                    CheckName(v.name);CheckTransform(v.original);CheckTransform(v.transform);
                    const auto base=std::ranges::find(original.regions,v.record,&Region::record);
                    if((v.added && (v.record || v.shape)) || (!v.added && (base==original.regions.end() || v.name!=base->name || v.key!=base->key || v.entity_id!=base->entity_id || v.shape!=base->shape || v.original!=base->original)) || v.key<0 || v.key>=d.next_region_key || !d.reserved_region_names.contains(v.name) || v.original.scale!=v.transform.scale)Fail("invalid waypoint source");
                }else if constexpr(std::is_same_v<T,Route>){
                    CheckName(v.name);const auto base=std::ranges::find(original.routes,v.record,&Route::record);
                    if((v.added && v.record) || (!v.added && (base==original.routes.end() || v.name!=base->name || v.key!=base->key || v.entity_id!=base->entity_id || v.event_id!=base->event_id || v.original!=base->original)) || v.key<0 || v.key>=d.next_route_key || !d.reserved_route_names.contains(v.name))Fail("invalid patrol source");
                }else if constexpr(std::is_same_v<T,Treasure>){
                    CheckName(v.name);const auto base=std::ranges::find(original.treasures,v.record,&Treasure::record);
                    if(base==original.treasures.end() || (!v.added && (v.name!=base->name || v.entity_id!=base->entity_id || v.event_id!=base->event_id || v.original!=base->original)) || !d.reserved_treasure_names.contains(v.name))Fail("invalid pickup source");
                }else if constexpr(std::is_same_v<T,ItemLot>){if(v.bytes.size()!=148 || !d.parameters || !d.reserved_lot_ids.contains(v.id) || !d.parameters->Table("ItemLotParam") || !d.parameters->Table("ItemLotParam")->Find(v.source_id))Fail("invalid private item lot");}
                else if constexpr(std::is_same_v<T,ScriptOperand>)checked_operand(v);
                else if constexpr(std::is_same_v<T,PickupQuest>){if(!d.scripts.contains(v.script) || !d.reserved_script_ids.contains(v.event_id))Fail("invalid reward quest");}
            };
            if(edit.before)validate(std::get<T>(*edit.before));if(edit.after)validate(std::get<T>(*edit.after));
            if constexpr(std::is_same_v<T,Part> || std::is_same_v<T,Region> || std::is_same_v<T,Route> || std::is_same_v<T,Treasure>){if((!edit.before && !std::get<T>(*edit.after).added) || (!edit.after && !std::get<T>(*edit.before).added))Fail("original records cannot be added or removed");}
            if(edit.before && edit.after){
                auto allowed=std::get<T>(*edit.before);const auto& after=std::get<T>(*edit.after);
                if constexpr(std::is_same_v<T,Part>){allowed.transform=after.transform;allowed.enemy=after.enemy;}
                else if constexpr(std::is_same_v<T,Region>)allowed.transform=after.transform;
                else if constexpr(std::is_same_v<T,Route> || std::is_same_v<T,Treasure>)allowed.data=after.data;
                else if constexpr(std::is_same_v<T,ItemLot>)allowed.bytes=after.bytes;
                else if constexpr(std::is_same_v<T,ScriptOperand>)allowed.value=after.value;
                if(Encoded(allowed)!=Encoded(after))Fail("edit changes record ownership");
            }
        },edit.before?*edit.before:*edit.after);
        d.Apply(edit,true);d.history.push_back(std::move(edit));++d.cursor;
    }
    io(cursor);io.End();if(cursor>d.cursor || (cursor && cursor<d.cursor && d.history[cursor].group && d.history[cursor].group==d.history[cursor-1].group))Fail("invalid undo cursor");
    auto validate=[&]{d.Save();if(!d.item_lots.empty())d.parameters->SaveLots(d.item_lots);for(const auto& [name,file]:d.scripts){std::vector<ScriptOperand> edits;std::vector<PickupQuest> quests;for(const auto& v:d.event_edits)if(v.script==name)edits.push_back(v);for(const auto& v:d.quests)if(v.script==name)quests.push_back(v);file->Save(edits,quests);}};
    validate();while(d.cursor>cursor)d.Undo();if(d.cursor!=cursor)Fail("invalid grouped undo cursor");validate();d.reserved_siblings=false;document=std::move(d);
}

std::vector<std::uint8_t> Project::Bytes()const{
    SourceKeys(sources,map);Archive<false> io;const auto path=game.string();const std::uint32_t version=2;
    io.Raw(const_cast<char*>(Magic),8);io(version,path,map,selected_name,selected_kind,route_key,camera,exported,exported_msb,exported_lots,exported_events,exported_quests,sources,session);
    io(character_imports);
    const auto crc=std::uint32_t(mz_crc32(0,io.bytes.data(),io.bytes.size()));io(crc);return std::move(io.bytes);
}
Project Project::Read(std::span<const std::uint8_t> bytes){
    if(bytes.size()<16 || bytes.size()>Limit || std::memcmp(bytes.data(),Magic,8))Fail("file header");std::uint32_t crc;std::memcpy(&crc,bytes.data()+bytes.size()-4,4);
    if(mz_crc32(0,bytes.data(),bytes.size()-4)!=crc)Fail("checksum mismatch");Archive<true> io(bytes.subspan(8,bytes.size()-12));Project p;std::uint32_t version;std::string game;
    io(version,game,p.map,p.selected_name,p.selected_kind,p.route_key,p.camera,p.exported,p.exported_msb,p.exported_lots,p.exported_events,p.exported_quests,p.sources,p.session);
    if(version==2)io(p.character_imports);io.End();
    p.game=game;if((version!=1 && version!=2) || !p.game.is_absolute() || p.selected_kind<0 || p.selected_kind>3 || p.route_key< -1 || p.camera[3]<=0 || p.camera[3]>10000)Fail("unsupported metadata");SourceKeys(p.sources,p.map);if(!p.exported_msb.empty())Document::Read(p.exported_msb);return p;
}
Project Project::Open(const std::filesystem::path& path){std::error_code ec;const auto n=std::filesystem::file_size(path,ec);if(ec || n>Limit)Fail("file size limit or unreadable file");return Read(ReadFile(path));}
void Project::Save(const std::filesystem::path& target)const{
    const auto absolute=std::filesystem::absolute(target);const auto parent=std::filesystem::canonical(absolute.parent_path());const auto path=parent/absolute.filename();
    if(absolute.filename().empty() || Inside(path,std::filesystem::weakly_canonical(game)))throw std::runtime_error("Save editor projects outside the game folder");
    const auto status=std::filesystem::symlink_status(path);if(std::filesystem::exists(status)){if(!std::filesystem::is_regular_file(status))throw std::runtime_error("Project destination is not a regular file");Project::Open(path);}
    auto bytes=Bytes();auto verified=Project::Read(bytes);(void)verified;
    std::string pattern=(parent/("."+absolute.filename().string()+".XXXXXX")).string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0)throw std::system_error(errno,std::generic_category(),"Create project temporary file");
    try{
        std::size_t at=0;while(at<bytes.size()){const auto n=write(fd,bytes.data()+at,bytes.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::system_error(errno,std::generic_category(),"Write project");at+=std::size_t(n);}
        if(fsync(fd))throw std::system_error(errno,std::generic_category(),"Sync project");const auto close_result=close(fd);fd=-1;if(close_result)throw std::system_error(errno,std::generic_category(),"Close project");
        if(rename(name.data(),path.c_str()))throw std::system_error(errno,std::generic_category(),"Replace project");
    }catch(...){if(fd>=0)close(fd);unlink(name.data());throw;}
}
} // namespace BbMapEditor
