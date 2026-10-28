// SPDX-License-Identifier: GPL-2.0-or-later
// Native Metal/ImGui shell. Orbit projection follows dev's BbOverlay::MetalBackend::Map3d;
// asset decoding and terrain generation are shared with its in-game map, without a game process.
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#include "map_scene.h"
#include "bbport_map_view.h"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {
using namespace BbMapEditor;
namespace fs=std::filesystem;
struct Options {
    fs::path game,output,script;
    std::string map="m24_01_00_00",select,list_param;
    std::optional<BbMapArt::Vec3> translation;
    bool batch=false,list=false,list_regions=false,list_routes=false,list_treasures=false,duplicate=false;
};
Options options;
int exit_code=0;
void Finish() {
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
}
void ParamChooser(const char* label,int* value,const ParamTable* table) {
    ImGui::PushID(label);ImGui::TextUnformatted(label);ImGui::SetNextItemWidth(-1);ImGui::InputInt("##id",value);
    const auto* row=table?table->Find(*value):nullptr;
    const auto preview=row?std::to_string(row->id)+" "+row->name:*value==-1?std::string("None"):std::string("Row not found");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##rows",preview.c_str())) {
        static char search[128]={};if(ImGui::IsWindowAppearing()){search[0]=0;ImGui::SetKeyboardFocusHere();}
        ImGui::InputText("Search ID or name",search,sizeof(search));
        if(ImGui::Selectable("None (-1)",*value==-1))*value=-1;
        std::vector<const ParamRow*> matches;
        if(table)for(const auto& r:table->rows)if(std::to_string(r.id).find(search)!=std::string::npos || r.name.find(search)!=std::string::npos)matches.push_back(&r);
        ImGuiListClipper clipper;clipper.Begin(int(matches.size()));
        while(clipper.Step())for(int n=clipper.DisplayStart;n<clipper.DisplayEnd;++n) {
            const auto& r=*matches[n];const auto text=std::to_string(r.id)+" "+r.name;
            if(ImGui::Selectable(text.c_str(),*value==r.id))*value=r.id;
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

struct GpuMesh {id<MTLBuffer> positions,colors,indices;std::size_t count=0;};
class Viewport {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth_state;
    id<MTLTexture> depth;
    std::unordered_map<const BbMapArt::AreaMesh*,GpuMesh> meshes;
public:
    id<MTLTexture> color,ids;
    explicit Viewport(id<MTLDevice> d):device(d) {
        NSString* source=@R"msl(
          #include <metal_stdlib>
          using namespace metal;
          struct Camera {float4 center,turn,frame,r0,r1,r2;uint4 options;};
          struct Vertex {float4 position [[position]];float3 color,world;};
          vertex Vertex map_vertex(uint id [[vertex_id]],device const packed_float3* positions [[buffer(0)]],
              device const uint* colors [[buffer(1)]],constant Camera& c [[buffer(2)]]) {
            float4 p=float4(float3(positions[id]),1);float3 w=float3(dot(p,c.r0),dot(p,c.r1),dot(p,c.r2)),d=w-c.center.xyz;
            float u=d.x*c.turn.x-d.z*c.turn.y,v=d.x*c.turn.y+d.z*c.turn.x;
            float up=v*c.turn.z+d.y*c.turn.w,near=-v*c.turn.w+d.y*c.turn.z;
            Vertex o;o.position=float4(u*c.center.w/c.frame.x,up*c.center.w/c.frame.y,clamp(.5f-near/8000.f,0.f,1.f),1);
            uint rgb=colors[id];o.color=float3(rgb&255,(rgb>>8)&255,(rgb>>16)&255)/255.f;o.world=w;return o;
          }
          struct Pixel {float4 color [[color(0)]];uint id [[color(1)]];};
          fragment Pixel map_fragment(Vertex in [[stage_in]],constant Camera& c [[buffer(2)]]) {
            float3 normal=normalize(cross(dfdx(in.world),dfdy(in.world)));
            float light=.55f+.45f*abs(dot(normal,normalize(float3(-.45f,.78f,.43f))));
            float3 rgb=in.color*light;
            if(in.world.y>c.frame.z) discard_fragment();
            if(c.options.y) rgb=mix(rgb,float3(1,.65f,.12f),.5f);
            return {float4(rgb,1),c.options.x};
          }
        )msl";
        NSError* error=nil;auto library=[device newLibraryWithSource:source options:nil error:&error];
        if(!library) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto desc=[MTLRenderPipelineDescriptor new];
        desc.vertexFunction=[library newFunctionWithName:@"map_vertex"];desc.fragmentFunction=[library newFunctionWithName:@"map_fragment"];
        desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        desc.colorAttachments[1].pixelFormat=MTLPixelFormatR32Uint;desc.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
        if(!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String);
        auto dd=[MTLDepthStencilDescriptor new];dd.depthCompareFunction=MTLCompareFunctionLessEqual;dd.depthWriteEnabled=YES;
        depth_state=[device newDepthStencilStateWithDescriptor:dd];
    }
    void Clear() {meshes.clear();}
    void Render(id<MTLCommandBuffer> command,const Scene& scene,const BbMapView::Camera& camera,
                ImVec2 size,float dpi,int selected,int selected_region,const std::optional<Transform>& preview,float ceiling,
                bool show_objects,bool show_enemies,bool show_regions,int active_route,bool route_only) {
        const NSUInteger width=std::clamp(int(size.x*dpi),1,8192),height=std::clamp(int(size.y*dpi),1,8192);
        if(!color || color.width!=width || color.height!=height) {
            auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
            d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;color=[device newTextureWithDescriptor:d];
            d.pixelFormat=MTLPixelFormatR32Uint;d.storageMode=MTLStorageModeShared;ids=[device newTextureWithDescriptor:d];
            d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;depth=[device newTextureWithDescriptor:d];
            if(!color || !ids || !depth) throw std::runtime_error("Metal viewport allocation failed");
        }
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        for(int i=0;i<2;++i) {pass.colorAttachments[i].texture=i?ids:color;pass.colorAttachments[i].loadAction=MTLLoadActionClear;pass.colorAttachments[i].storeAction=MTLStoreActionStore;}
        pass.colorAttachments[0].clearColor=MTLClearColorMake(.045,.055,.075,1);
        pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionDontCare;pass.depthAttachment.clearDepth=1;
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];encoder.label=@"Map editor terrain and object picking";
        [encoder setRenderPipelineState:pipeline];[encoder setDepthStencilState:depth_state];[encoder setCullMode:MTLCullModeNone];
        auto draw=[&](const std::shared_ptr<const BbMapArt::AreaMesh>& mesh,const Transform& t,std::uint32_t id) {
            if(!mesh || mesh->indices.empty()) return;
            auto& gpu=meshes[mesh.get()];
            if(!gpu.positions) {
                gpu.positions=[device newBufferWithBytes:mesh->positions.data() length:mesh->positions.size()*4 options:MTLResourceStorageModeShared];
                gpu.colors=[device newBufferWithBytes:mesh->colors.data() length:mesh->colors.size()*4 options:MTLResourceStorageModeShared];
                gpu.indices=[device newBufferWithBytes:mesh->indices.data() length:mesh->indices.size()*4 options:MTLResourceStorageModeShared];gpu.count=mesh->indices.size();
                if(!gpu.positions || !gpu.colors || !gpu.indices) throw std::runtime_error("Metal model allocation failed");
            }
            struct Constants {float center[4],turn[4],frame[4],row[12];std::uint32_t options[4];} c{};
            c.center[0]=camera.x;c.center[1]=camera.y;c.center[2]=camera.z;c.center[3]=camera.zoom;
            c.turn[0]=std::cos(camera.yaw);c.turn[1]=std::sin(camera.yaw);c.turn[2]=std::sin(camera.pitch);c.turn[3]=std::cos(camera.pitch);
            c.frame[0]=size.x*.5f;c.frame[1]=size.y*.5f;c.frame[2]=ceiling;
            const float origin[3]={0,0,0},axes[3][3]={{1,0,0},{0,1,0},{0,0,1}};
            auto o=WorldPoint(t,origin);
            for(int r=0;r<3;++r) {c.row[r*4+3]=o[r];for(int a=0;a<3;++a)c.row[r*4+a]=WorldPoint(t,axes[a])[r]-o[r];}
            c.options[0]=id;c.options[1]=id && ((id&0x80000000u)?int((id&0x7fffffffu)-1)==selected_region:int(id)==selected+1);
            [encoder setVertexBuffer:gpu.positions offset:0 atIndex:0];[encoder setVertexBuffer:gpu.colors offset:0 atIndex:1];
            [encoder setVertexBytes:&c length:sizeof(c) atIndex:2];[encoder setFragmentBytes:&c length:sizeof(c) atIndex:2];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:gpu.count indexType:MTLIndexTypeUInt32 indexBuffer:gpu.indices indexBufferOffset:0];
        };
        draw(scene.terrain,{},0);
        const auto& objects=scene.document.Parts();
        for(std::size_t i=0;i<objects.size();++i) {
            const auto& o=objects[i];
            if(o.kind==PartKind::Enemy) {
                if(show_enemies) draw(scene.enemy_marker,int(i)==selected && preview?*preview:o.transform,std::uint32_t(i+1));
                continue;
            }
            if(!show_objects) continue;
            auto found=scene.models.find(objects[i].model);
            if(found!=scene.models.end()) draw(found->second,int(i)==selected && preview?*preview:objects[i].transform,std::uint32_t(i+1));
        }
        if(show_regions)for(std::size_t i=0;i<scene.document.Regions().size();++i) {
            const auto& r=scene.document.Regions()[i];
            if(route_only && active_route>=0 && int(i)!=selected_region &&
               std::ranges::none_of(scene.document.Routes()[active_route].data.points,[&](const auto& p){return p.index==r.key;}))continue;
            draw(scene.region_marker,int(i)==selected_region && preview?*preview:r.transform,0x80000000u|std::uint32_t(i+1));
        }
        [encoder endEncoding];
    }
};

struct State {
    std::shared_ptr<Scene> scene;
    std::future<std::shared_ptr<Scene>> loading;
    std::stop_source stop;
    std::mutex mutex;
    std::string progress,error,status;
    std::string selection_name;
    std::vector<std::string> maps,commands;
    std::size_t command=0;
    int frames=0,selected=-1,selected_region=-1,active_route=-1,selected_treasure=-1;
    int region_key=-1,route_key=-1;
    bool region_selection=false;
    bool treasure_selection=false;
    std::string treasure_name;
    bool fitted=false;
    bool cutaway=false;
    bool show_objects=true,show_enemies=true;
    bool show_regions=false,route_only=true;
    int layer=0;
    float ceiling=10000;
    BbMapView::Camera camera{0,0,0,4,.45f,.85f};
    Transform fields;
    EnemyData enemy_fields;
    ItemContents item_fields;
    char filter[128]={};
    std::optional<Transform> drag;
    Transform drag_start;
    ImVec2 drag_mouse{};
    int drag_axis=-1;
    // Optional local input replay for reproducible viewport/picking checks; no system events.
    std::optional<std::array<ImVec2,2>> input_drag;
    int input_frame=0;
    fs::path capture;
    enum class Pending { None, Duplicate, Remove, Waypoint, Route, RemoveRoute } pending=Pending::None;
    std::vector<std::uint8_t> exported;
    std::vector<ItemLot> exported_lots;
    mutable std::uint64_t unsaved_version=UINT64_MAX;
    mutable bool unsaved_cached=false;
    ~State() {stop.request_stop();if(loading.valid()) loading.wait();}
    bool Unsaved() const {
        if(!scene)return false;const auto& doc=scene->document;
        if(unsaved_version!=doc.Version()) {unsaved_cached=doc.Changed() && (doc.ItemLots()!=exported_lots || doc.Inflated()!=exported);unsaved_version=doc.Version();}
        return unsaved_cached;
    }
    bool Discard() {
        if(!Unsaved()) return true;
        auto alert=[NSAlert new];alert.messageText=@"Discard unexported map changes?";
        [alert addButtonWithTitle:@"Keep Editing"];[alert addButtonWithTitle:@"Discard Changes"];
        return [alert runModal]==NSAlertSecondButtonReturn;
    }
    template<class F> void Action(F f) {
        try {error.clear();f();}catch(const std::exception& e) {error=e.what();}
    }
    void Load() {
        if(loading.valid()) return;
        scene.reset();selected=selected_region=active_route=selected_treasure=-1;treasure_selection=false;treasure_name.clear();region_key=route_key=-1;selection_name.clear();fitted=false;cutaway=false;exported.clear();exported_lots.clear();unsaved_version=UINT64_MAX;error.clear();progress="Opening map...";
        const auto game=options.game;
        const auto map=options.map;
        loading=std::async(std::launch::async,[this,game,map] {
            return std::make_shared<Scene>(LoadScene(game,map,[this](std::string s){std::scoped_lock lock(mutex);progress=std::move(s);},stop.get_token()));
        });
    }
    void Select(int index,bool focus) {
        selected=index;selected_region=selected_treasure=-1;treasure_selection=false;drag.reset();drag_axis=-1;
        if(!scene || index<0) return;
        const auto& object=scene->document.Parts().at(index);fields=object.transform;
        selection_name=object.name;region_selection=false;if(object.enemy)enemy_fields=*object.enemy;
        if(focus) {camera.x=fields.position[0];camera.y=fields.position[1]+1;camera.z=fields.position[2];camera.zoom=45;cutaway=true;ceiling=fields.position[1]+4;}
    }
    void SelectRegion(int index,bool focus) {
        selected=selected_treasure=-1;treasure_selection=false;selected_region=index;drag.reset();drag_axis=-1;
        if(!scene || index<0)return;
        const auto& region=scene->document.Regions().at(index);fields=region.transform;
        region_key=region.key;region_selection=true;show_regions=true;
        if(focus){camera.x=fields.position[0];camera.y=fields.position[1]+1;camera.z=fields.position[2];camera.zoom=45;cutaway=true;ceiling=fields.position[1]+4;}
    }
    std::optional<ItemLot> CurrentLot() const {
        if(selected_treasure<0)return {};const auto& t=scene->document.Treasures()[selected_treasure];
        const auto& lots=scene->document.ItemLots();const auto found=std::ranges::find(lots,t.data.lots[0],&ItemLot::id);if(found!=lots.end())return *found;
        const auto* table=scene->params.Table("ItemLotParam");if(!table || !table->Find(t.data.lots[0]))return {};
        try{return ReadItemLot(*table,t.data.lots[0]);}catch(const std::exception&){return {};}
    }
    void SelectTreasure(int index,bool focus) {
        const auto& t=scene->document.Treasures().at(index);const auto& parts=scene->document.Parts();const auto part=std::ranges::find(parts,t.data.part.name,&Part::name);
        Select(part==parts.end()?-1:int(part-parts.begin()),focus);selected_treasure=index;treasure_selection=true;treasure_name=t.name;
        if(const auto lot=CurrentLot())item_fields=lot->First();else item_fields={};
    }
    const Transform* Current() const {
        if(!scene)return nullptr;
        if(selected>=0)return &scene->document.Parts()[selected].transform;
        return selected_region>=0?&scene->document.Regions()[selected_region].transform:nullptr;
    }
    void SetTransform(const Transform& t) {
        if(selected>=0)scene->document.Set(selected,t);else if(selected_region>=0)scene->document.SetRegion(selected_region,t);
        else throw std::runtime_error("Select a placement or waypoint first");fields=t;
    }
    void SelectRoute(int index,bool focus) {
        active_route=index;if(index<0)return;const auto& route=scene->document.Routes().at(index);route_key=route.key;show_regions=true;
        BbMapArt::Vec3 low{FLT_MAX,FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX,-FLT_MAX};bool first=true;
        if(focus)for(const auto& p:route.data.points) {
            const auto& regions=scene->document.Regions();const auto found=std::ranges::find(regions,p.index,&Region::key);
            if(found!=regions.end()) {
                if(first){SelectRegion(int(found-regions.begin()),false);first=false;}
                for(int n=0;n<3;++n){low[n]=std::min(low[n],found->transform.position[n]);high[n]=std::max(high[n],found->transform.position[n]);}
            }
        }
        if(!first) {
            camera.x=(low[0]+high[0])*.5f;camera.y=(low[1]+high[1])*.5f+1;camera.z=(low[2]+high[2])*.5f;
            camera.zoom=std::min(45.f,BbMapView::FitZoom(camera,low[0]-1,low[2]-1,high[0]+1,high[2]+1,
                std::max(200.f,ImGui::GetIO().DisplaySize.x-340),std::max(200.f,ImGui::GetIO().DisplaySize.y-180),low[1],high[1]));
            cutaway=true;ceiling=high[1]+4;
        }
    }
    void Waypoint() {
        const auto position=Current()?Current()->position:BbMapArt::Vec3{camera.x,camera.y,camera.z};
        SelectRegion(int(scene->document.AddWaypoint(position)),false);layer=3;
        status="Created waypoint. Position it, then append it to a route.";
    }
    void NewRoute() {SelectRoute(int(scene->document.AddRoute()),false);layer=4;status="Created patrol. Append waypoints and assign it to a character.";}
    void AppendWaypoint() {
        if(active_route<0 || selected_region<0)throw std::runtime_error("Select a patrol and waypoint first");
        auto data=scene->document.Routes()[active_route].data;
        auto empty=std::ranges::find(data.points,-1,&Reference::index);if(empty==data.points.end())throw std::runtime_error("A patrol supports at most 32 waypoints");
        const auto& r=scene->document.Regions()[selected_region];*empty={r.key,r.name};scene->document.SetRoute(active_route,data);
    }
    void AssignRoute() {
        if(active_route<0 || selected<0 || !scene->document.Parts()[selected].enemy)throw std::runtime_error("Select a patrol and a character first");
        auto data=*scene->document.Parts()[selected].enemy;const auto& r=scene->document.Routes()[active_route];
        data.patrol={r.key,r.name};data.patrol_type=std::int16_t(r.data.type);data.move_points.fill({});
        scene->document.SetEnemy(selected,data);enemy_fields=*scene->document.Parts()[selected].enemy;
    }
    void Character(const EnemyData& data) {
        if(selected<0 || !scene->document.Parts()[selected].enemy)throw std::runtime_error("Select a character first");
        ValidateCharacterReferences(*scene->document.Parts()[selected].enemy,data,scene->params,scene->talk_scripts);
        scene->document.SetEnemy(selected,data);enemy_fields=data;
    }
    void Duplicate() {
        if(scene && selected_treasure>=0){const auto i=scene->document.DuplicateTreasure(selected_treasure);SelectTreasure(int(i),false);status="Created independent pickup. Move it and choose its contents.";return;}
        if(!scene || selected<0)return;
        const auto index=scene->document.Duplicate(selected);Select(int(index),false);
        status="Created "+scene->document.Parts()[index].name+". Move the copy to its new position.";
    }
    void Remove() {
        if(!scene)return;
        if(selected_treasure>=0){scene->document.RemoveTreasure(selected_treasure);Select(-1,false);treasure_selection=true;status="Removed pickup. Undo restores its object, event and item lot.";return;}
        if(selected_region>=0)scene->document.RemoveRegion(selected_region);
        else if(selected>=0)scene->document.Remove(selected);else return;
        Select(-1,false);status="Removed copy. Undo restores it.";
    }
    bool History(bool redo) {
        if(!scene)return false;
        const auto name=selected<0?selection_name:scene->document.Parts()[selected].name;
        const bool changed=redo?scene->document.Redo():scene->document.Undo();
        if(treasure_selection) {
            const auto& entries=scene->document.Treasures();const auto found=std::ranges::find(entries,treasure_name,&Treasure::name);
            if(found!=entries.end())SelectTreasure(int(found-entries.begin()),false);else{Select(-1,false);treasure_selection=true;}
        } else if(region_selection) {
            const auto& regions=scene->document.Regions();const auto found=std::ranges::find(regions,region_key,&Region::key);
            SelectRegion(found==regions.end()?-1:int(found-regions.begin()),false);
        } else {
            const auto& parts=scene->document.Parts();const auto found=std::ranges::find(parts,name,&Part::name);
            Select(found==parts.end()?-1:int(found-parts.begin()),false);
        }
        const auto& routes=scene->document.Routes();const auto route=std::ranges::find(routes,route_key,&Route::key);
        active_route=route==routes.end()?-1:int(route-routes.begin());
        if(changed)status=redo?"Redid last edit.":"Undid last edit.";
        return changed;
    }
    void Export(const fs::path& folder) {
        if(!scene) return;
        auto path=scene->document.Export(fs::absolute(folder),options.map);
        exported=scene->document.Inflated();exported_lots=scene->document.ItemLots();unsaved_version=UINT64_MAX;
        status="Exported mod: "+folder.filename().string()+". Enable it in Mods & Cheats, then reload the area.";
        std::printf("EXPORTED %s\n",path.c_str());
    }
    void ExportDialog() {
        if(!options.output.empty()) {Export(options.output);return;}
        auto panel=[NSSavePanel savePanel];panel.title=@"Export map mod to a new folder";
        panel.nameFieldStringValue=@"Bloodborne-map-mod";panel.canCreateDirectories=YES;
        if([panel runModal]==NSModalResponseOK) Export(panel.URL.fileSystemRepresentation);
    }
    void Poll() {
        const auto action=pending;pending=Pending::None;
        if(action!=Pending::None)Action([&]{
            if(action==Pending::Duplicate)Duplicate();else if(action==Pending::Remove)Remove();
            else if(action==Pending::Waypoint)Waypoint();else if(action==Pending::Route)NewRoute();
            else if(active_route>=0){scene->document.RemoveRoute(active_route);active_route=-1;}
        });
        if(loading.valid() && loading.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {scene=loading.get();std::printf("LOADED %s: %zu placements, %zu terrain triangles, %zu missing object models\n",options.map.c_str(),scene->document.Parts().size(),scene->terrain->indices.size()/3,scene->missing_models);}
            catch(const std::exception& e) {error=e.what();if(!commands.empty()) {exit_code=1;Finish();}}
            frames=0;
            if(scene && !options.select.empty()) {
                const auto& objects=scene->document.Parts();auto it=std::ranges::find(objects,options.select,&Part::name);
                if(it==objects.end()) throw std::runtime_error("Selected placement was not found");
                Select(int(it-objects.begin()),true);fitted=true;
            }
        }
    }
    void Script() {
        if(!scene || command>=commands.size() || ++frames<12) return;
        frames=0;std::istringstream line(commands[command++]);std::string verb;line>>verb;
        std::printf("EDITOR %s\n",commands[command-1].c_str());
        if(verb=="select") {
            std::string name;line>>name;const auto& objects=scene->document.Parts();auto it=std::ranges::find(objects,name,&Part::name);
            if(it==objects.end()) throw std::runtime_error("Script placement not found: "+name);
            Select(int(it-objects.begin()),true);fitted=true;if(layer>=3)layer=0;
        } else if(verb=="region" || verb=="route") {
            std::string name;std::getline(line>>std::ws,name);
            if(verb=="region") {
                const auto& entries=scene->document.Regions();const auto found=std::ranges::find(entries,name,&Region::name);
                if(found==entries.end())throw std::runtime_error("Region not found: "+name);SelectRegion(int(found-entries.begin()),true);layer=3;
            } else {
                const auto& entries=scene->document.Routes();const auto found=std::ranges::find(entries,name,&Route::name);
                if(found==entries.end())throw std::runtime_error("Patrol not found: "+name);SelectRoute(int(found-entries.begin()),true);layer=4;
            }
        } else if(verb=="treasure") {
            std::string name;std::getline(line>>std::ws,name);const auto& list=scene->document.Treasures();const auto found=std::ranges::find(list,name,&Treasure::name);
            if(found==list.end())throw std::runtime_error("Treasure not found: "+name);SelectTreasure(int(found-list.begin()),true);layer=6;
        } else if(verb=="item" || verb=="assert-item") {
            ItemContents contents;if(!(line>>contents.category>>contents.item>>contents.count) || selected_treasure<0)throw std::runtime_error("item needs a selected pickup and category ID count");
            if(verb=="item"){scene->document.SetItemContents(selected_treasure,contents);item_fields=contents;}
            else {const auto lot=CurrentLot();if(!lot || lot->First()!=contents)throw std::runtime_error("Unexpected pickup contents");}
        } else if(verb=="assert-treasure-count") {
            std::size_t n;if(!(line>>n) || n!=scene->document.Treasures().size())throw std::runtime_error("Unexpected treasure count");
        } else if(verb=="assert-unsaved") {
            int expected;if(!(line>>expected) || Unsaved()!=bool(expected))throw std::runtime_error("Unexpected export state");
        } else if(verb=="waypoint") {
            BbMapArt::Vec3 p;for(auto& f:p)if(!(line>>f))throw std::runtime_error("waypoint needs x y z");
            SelectRegion(int(scene->document.AddWaypoint(p)),true);layer=3;
        } else if(verb=="character" || verb=="assert-character") {
            if(selected<0 || !scene->document.Parts()[selected].enemy)throw std::runtime_error("Select a character first");
            auto data=*scene->document.Parts()[selected].enemy;
            if(!(line>>data.npc_param>>data.think_param>>data.talk_id>>data.chara_init>>data.init_anim>>data.damage_anim))throw std::runtime_error("character needs NPC AI Talk CharaInit IdleAnim DamageAnim");
            if(verb=="character")Character(data);
            else if(data!=*scene->document.Parts()[selected].enemy)throw std::runtime_error("Unexpected character settings");
        } else if(verb=="new-route") NewRoute();
        else if(verb=="route-add") AppendWaypoint();
        else if(verb=="assign-route") AssignRoute();
        else if(verb=="route-type") {
            if(active_route<0)throw std::runtime_error("Select a patrol first");auto data=scene->document.Routes()[active_route].data;
            if(!(line>>data.type))throw std::runtime_error("route-type needs a behavior number");scene->document.SetRoute(active_route,data);
        } else if(verb=="assert-region-count" || verb=="assert-route-count") {
            std::size_t n;if(!(line>>n) || n!=(verb=="assert-region-count"?scene->document.Regions().size():scene->document.Routes().size()))throw std::runtime_error("Unexpected region/patrol count");
        } else if(verb=="assert-route-point") {
            int slot;std::string name;if(!(line>>slot))throw std::runtime_error("Missing patrol slot");std::getline(line>>std::ws,name);
            if(active_route<0 || slot<0 || slot>=32 || scene->document.Routes()[active_route].data.points[slot].name!=name)throw std::runtime_error("Unexpected patrol waypoint");
        } else if(verb=="deselect") Select(-1,false);
        else if(verb=="click" || verb=="drag") {
            std::array<ImVec2,2> p;
            if(!(line>>p[0].x>>p[0].y)) throw std::runtime_error("click needs x y");
            p[1]=p[0];
            if(verb=="drag" && !(line>>p[1].x>>p[1].y)) throw std::runtime_error("drag needs x y end-x end-y");
            input_drag=p;input_frame=0;
        } else if(verb=="assert-selected") {
            std::string name;line>>name;
            const auto actual=selected>=0?scene->document.Parts()[selected].name:selected_region>=0?scene->document.Regions()[selected_region].name:std::string();
            if(actual!=name)throw std::runtime_error("Viewport selected the wrong placement");
        } else if(verb=="assert-position" || verb=="assert-rotation") {
            if(!Current()) throw std::runtime_error("Nothing selected");
            BbMapArt::Vec3 expected;for(float& v:expected)if(!(line>>v))throw std::runtime_error("assert-position needs x y z tolerance");
            float tolerance;if(!(line>>tolerance))throw std::runtime_error("Missing position tolerance");
            const auto& t=*Current();
            const auto& actual=verb=="assert-position"?t.position:t.rotation;
            for(int i=0;i<3;++i)if(std::abs(actual[i]-expected[i])>tolerance)throw std::runtime_error("Unexpected edited transform");
        } else if(verb=="move") {
            if(!Current()) throw std::runtime_error("Select a placement before moving it");
            auto t=*Current();
            for(auto& v:t.position) {float delta;if(!(line>>delta)) throw std::runtime_error("move needs three numbers");v+=delta;}
            SetTransform(t);
        } else if(verb=="rotate") {
            if(!Current()) throw std::runtime_error("Select a placement before rotating it");
            auto t=*Current();
            for(auto& v:t.rotation) {float delta;if(!(line>>delta)) throw std::runtime_error("rotate needs three degrees");v+=delta;}
            SetTransform(t);
        } else if(verb=="undo" || verb=="redo") {
            if(!History(verb=="redo"))throw std::runtime_error("Empty undo/redo history");
        } else if(verb=="duplicate" || verb=="remove") {
            if(!Current() || (verb=="duplicate" && selected<0))throw std::runtime_error("Select a placement first");
            if(verb=="duplicate")Duplicate();else Remove();
        } else if(verb=="assert-count") {
            std::size_t count;if(!(line>>count) || count!=scene->document.Parts().size())throw std::runtime_error("Unexpected placement count");
        } else if(verb=="export" || verb=="capture") {
            std::string path;std::getline(line>>std::ws,path);if(path.empty()) throw std::runtime_error("Missing script path");
            if(verb=="export") Export(path);else capture=path;
        } else if(verb=="quit") Finish();
        else throw std::runtime_error("Unknown editor command: "+verb);
    }
    void Input() {
        if(!input_drag) return;
        const float t=std::min(1.f,input_frame/8.f);const auto& p=*input_drag;
        auto& io=ImGui::GetIO();io.AddMousePosEvent(p[0].x+(p[1].x-p[0].x)*t,p[0].y+(p[1].y-p[0].y)*t);
        if(input_frame==0)io.AddMouseButtonEvent(0,true);
        if(input_frame==9) {io.AddMouseButtonEvent(0,false);input_drag.reset();}
        ++input_frame;
    }
};
std::unique_ptr<State> state;

void SaveImage(const fs::path& path,id<MTLBuffer> pixels,NSUInteger width,NSUInteger height) {
    auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width*4 bitsPerPixel:32];
    auto* src=static_cast<const std::uint8_t*>(pixels.contents);auto* dst=bitmap.bitmapData;
    for(NSUInteger i=0;i<width*height;++i) {dst[i*4]=src[i*4+2];dst[i*4+1]=src[i*4+1];dst[i*4+2]=src[i*4];dst[i*4+3]=255;}
    auto png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if(![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES]) throw std::runtime_error("Screenshot write failed");
    std::printf("CAPTURED %s\n",path.c_str());
}
} // namespace

@interface EditorController : NSViewController<MTKViewDelegate,NSWindowDelegate>
@end
@implementation EditorController {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    std::unique_ptr<Viewport> _viewport;
}
- (void)loadView {
    _device=MTLCreateSystemDefaultDevice();if(!_device) throw std::runtime_error("No Metal device");
    _queue=[_device newCommandQueue];_viewport=std::make_unique<Viewport>(_device);
    auto view=[[MTKView alloc] initWithFrame:NSMakeRect(0,0,1360,850) device:_device];
    view.delegate=self;view.colorPixelFormat=MTLPixelFormatBGRA8Unorm;view.framebufferOnly=NO;view.preferredFramesPerSecond=60;self.view=view;
    ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig base_font;base_font.SizePixels=16;ImGui::GetIO().Fonts->AddFontDefault(&base_font);
    const char* japanese_font="/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc";
    if(fs::exists(japanese_font)) {
        ImFontConfig fallback;fallback.MergeMode=true;
        ImGui::GetIO().Fonts->AddFontFromFileTTF(japanese_font,16,&fallback,ImGui::GetIO().Fonts->GetGlyphRangesJapanese());
    }
    ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();style.WindowRounding=0;style.FrameRounding=4;style.FontSizeBase=16;
    ImGui_ImplMetal_Init(_device);ImGui_ImplOSX_Init(view);
}
- (BOOL)windowShouldClose:(NSWindow*)window {return state->Discard();}
- (void)windowWillClose:(NSNotification*)notification {Finish();}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}
- (void)drawInMTKView:(MTKView*)view {
  @autoreleasepool {
    try {
        state->Poll();state->Script();
        auto drawable=view.currentDrawable;auto pass=view.currentRenderPassDescriptor;if(!drawable || !pass) return;
        ImGuiIO& io=ImGui::GetIO();io.DisplaySize=ImVec2(view.bounds.size.width,view.bounds.size.height);
        io.DisplayFramebufferScale=ImVec2(view.drawableSize.width/io.DisplaySize.x,view.drawableSize.height/io.DisplaySize.y);
        io.DeltaTime=1.f/60;ImGui_ImplMetal_NewFrame(pass);ImGui_ImplOSX_NewFrame(view);state->Input();ImGui::NewFrame();
        auto command=[_queue commandBuffer];
        ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Bloodborne Map Editor",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("BLOODBORNE  /  MAP EDITOR");ImGui::SameLine();ImGui::TextDisabled("Metal");ImGui::Separator();
        auto& s=*state;
        ImGui::SetNextItemWidth(190);
        ImGui::BeginDisabled(s.loading.valid());
        if(ImGui::BeginCombo("Map variant",options.map.c_str())) {
            for(std::size_t i=0;i<s.maps.size();++i) if(ImGui::Selectable(s.maps[i].c_str(),s.maps[i]==options.map) && s.maps[i]!=options.map && s.Discard()) {
                options.map=s.maps[i];options.select.clear();_viewport->Clear();s.Load();
            }
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("An area can load several variants together. Export the variant that owns this placement and verify it after reloading.");
        ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!s.scene);
        if(ImGui::Button("Export mod...  Cmd+Shift+E") || ImGui::Shortcut(ImGuiMod_Super|ImGuiMod_Shift|ImGuiKey_E)) s.Action([&]{s.ExportDialog();});
        ImGui::SameLine();
        if(ImGui::Button("Undo") || ImGui::Shortcut(ImGuiMod_Super|ImGuiKey_Z)) s.History(false);
        ImGui::SameLine();
        if(ImGui::Button("Redo") || ImGui::Shortcut(ImGuiMod_Super|ImGuiMod_Shift|ImGuiKey_Z)) s.History(true);
        if(s.scene && s.selected>=0) {
            if(ImGui::Shortcut(ImGuiMod_Super|ImGuiKey_D))s.pending=State::Pending::Duplicate;
            if(s.scene->document.Parts()[s.selected].added && (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)))s.pending=State::Pending::Remove;
        }
        if(s.scene && s.selected_region>=0 && s.scene->document.Regions()[s.selected_region].added &&
           (ImGui::Shortcut(ImGuiKey_Delete) || ImGui::Shortcut(ImGuiKey_Backspace)))s.pending=State::Pending::Remove;
        ImGui::EndDisabled();
        if(!s.error.empty()) ImGui::TextWrapped("%s",s.error.c_str());
        if(!s.scene) {std::scoped_lock lock(s.mutex);ImGui::TextWrapped("%s",s.progress.c_str());ImGui::TextDisabled("Loading your game assets. The window remains responsive.");}
        std::optional<ImVec2> pick;
        if(s.scene) {
            auto& scene=*s.scene;const auto& objects=scene.document.Parts();
            ImGui::BeginChild("Objects and transform",{300,-42},ImGuiChildFlags_Borders);
            const auto enemies=std::ranges::count(objects,PartKind::Enemy,&Part::kind);
            ImGui::Text("%zu objects / %zu enemies",objects.size()-enemies,std::size_t(enemies));
            ImGui::Text("%zu regions / %zu patrols",scene.document.Regions().size(),scene.document.Routes().size());
            ImGui::SetNextItemWidth(-1);ImGui::Combo("##layer",&s.layer,"Objects and characters\0Objects\0Enemies / NPCs\0Regions / waypoints\0Patrol routes\0NPCs with talk scripts\0Items / pickups\0");
            if(ImGui::Button("New waypoint"))s.pending=State::Pending::Waypoint;
            ImGui::SameLine();if(ImGui::Button("New patrol"))s.pending=State::Pending::Route;
            ImGui::InputTextWithHint("##filter","Filter by name or model",s.filter,sizeof(s.filter));
            ImGui::BeginChild("Placement list",{0,190},ImGuiChildFlags_Borders);
            if(s.layer<=2 || s.layer==5)for(std::size_t i=0;i<objects.size();++i) {
                const auto& o=objects[i];if(s.filter[0] && o.name.find(s.filter)==std::string::npos && o.model.find(s.filter)==std::string::npos) continue;
                if((s.layer==1 && o.kind!=PartKind::Object) || (s.layer==2 && o.kind!=PartKind::Enemy)) continue;
                if(s.layer==5 && (!o.enemy || o.enemy->talk_id<=0))continue;
                if(ImGui::Selectable(o.name.c_str(),int(i)==s.selected)) s.Select(int(i),true);
            }
            if(s.layer==3)for(std::size_t i=0;i<scene.document.Regions().size();++i) {
                const auto& r=scene.document.Regions()[i];if(s.filter[0] && r.name.find(s.filter)==std::string::npos)continue;
                ImGui::PushID(int(i));if(ImGui::Selectable(r.name.c_str(),int(i)==s.selected_region))s.SelectRegion(int(i),true);ImGui::PopID();
            }
            if(s.layer==6)for(std::size_t i=0;i<scene.document.Treasures().size();++i) {
                const auto& t=scene.document.Treasures()[i];if(s.filter[0] && t.name.find(s.filter)==std::string::npos && t.data.part.name.find(s.filter)==std::string::npos)continue;
                ImGui::PushID(int(i));if(ImGui::Selectable(t.name.c_str(),int(i)==s.selected_treasure))s.SelectTreasure(int(i),true);ImGui::PopID();
            }
            if(s.layer==4)for(std::size_t i=0;i<scene.document.Routes().size();++i) {
                const auto& r=scene.document.Routes()[i];if(s.filter[0] && r.name.find(s.filter)==std::string::npos)continue;
                ImGui::PushID(int(i));if(ImGui::Selectable(r.name.c_str(),int(i)==s.active_route))s.SelectRoute(int(i),true);ImGui::PopID();
            }
            ImGui::EndChild();ImGui::Separator();
            if(s.selected>=0) {
                const auto& o=objects[s.selected];ImGui::TextWrapped("%s",o.name.c_str());ImGui::TextDisabled("Model: %s",o.model.c_str());
                if(ImGui::Button("Duplicate  Cmd+D"))s.pending=State::Pending::Duplicate;
                if(o.added) {ImGui::SameLine();if(ImGui::Button("Remove copy"))s.pending=State::Pending::Remove;}
                ImGui::Text("Entity ID: %d",o.entity_id);ImGui::Text("Instance ID: %d",o.instance_id);
                ImGui::TextUnformatted("Position (metres)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##position",s.fields.position.data(),"%.3f");
                ImGui::TextUnformatted("Rotation (degrees)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##rotation",s.fields.rotation.data(),"%.2f");
                if(o.kind==PartKind::Object) {ImGui::TextUnformatted("Scale");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##scale",s.fields.scale.data(),"%.3f");}
                if(ImGui::Button("Apply transform")) s.Action([&]{scene.document.Set(s.selected,s.fields);});
                ImGui::SameLine();if(ImGui::Button("Reset")) {scene.document.Set(s.selected,o.original);s.fields=o.original;}
                if(ImGui::Button("Frame selected  F")) {if(s.selected_treasure>=0)s.SelectTreasure(s.selected_treasure,true);else s.Select(s.selected,true);}
                if(o.enemy) {
                    const auto& e=*o.enemy;
                    ImGui::TextWrapped("Enemy proxy: capsule and facing arrow. Character models are not loaded.");
                    if(const auto* table=scene.params.Table("NpcParam"))if(const auto info=ReadNpcInfo(*table,e.npc_param)) {
                        ImGui::Text("Base HP: %u / NPC type: %u",info->hp,unsigned(info->type));ImGui::Text("Team: %u",unsigned(info->team));
                        if(const auto* row=table->Find(e.npc_param))ImGui::TextWrapped("%s",row->name.c_str());
                    }
                    if(e.talk_id>0)ImGui::TextWrapped("Talk script assigned: %d. Events and saved quest state govern interaction and allegiance.",e.talk_id);
                    if(ImGui::CollapsingHeader("Character settings")) {
                        auto integer=[](const char* label,int* value){ImGui::TextUnformatted(label);ImGui::SetNextItemWidth(-1);const auto id="##"+std::string(label);ImGui::InputInt(id.c_str(),value);};
                        ParamChooser("NPC param",&s.enemy_fields.npc_param,scene.params.Table("NpcParam"));
                        ParamChooser("AI param",&s.enemy_fields.think_param,scene.params.Table("NpcThinkParam"));
                        integer("Talk script",&s.enemy_fields.talk_id);
                        const auto talk=scene.talk_scripts.find(s.enemy_fields.talk_id);
                        ImGui::TextWrapped("%s",talk==scene.talk_scripts.end()?(s.enemy_fields.talk_id<=0?"No talk script":"ESD not found in this area"):talk->second.c_str());
                        if(ImGui::BeginCombo("Area talk scripts",talk==scene.talk_scripts.end()?"Choose ESD":talk->second.c_str())) {
                            if(ImGui::Selectable("None",s.enemy_fields.talk_id<=0))s.enemy_fields.talk_id=0;
                            for(const auto& [id,name]:scene.talk_scripts)if(ImGui::Selectable(name.c_str(),id==s.enemy_fields.talk_id))s.enemy_fields.talk_id=id;
                            ImGui::EndCombo();
                        }
                        ParamChooser("Character init",&s.enemy_fields.chara_init,scene.params.Table("CharaInitParam"));
                        integer("Idle animation",&s.enemy_fields.init_anim);integer("Damage animation",&s.enemy_fields.damage_anim);
                        if(ImGui::Button("Apply character settings"))s.Action([&]{
                            auto data=*scene.document.Parts()[s.selected].enemy;
                            data.npc_param=s.enemy_fields.npc_param;data.think_param=s.enemy_fields.think_param;data.talk_id=s.enemy_fields.talk_id;
                            data.chara_init=s.enemy_fields.chara_init;data.init_anim=s.enemy_fields.init_anim;data.damage_anim=s.enemy_fields.damage_anim;
                            s.Character(data);
                        });
                        if(!scene.param_error.empty())ImGui::TextWrapped("Parameter lookup unavailable: %s",scene.param_error.c_str());
                        if(!scene.talk_error.empty())ImGui::TextWrapped("Talk lookup unavailable: %s",scene.talk_error.c_str());
                        ImGui::TextWrapped("New references must exist in this game's tables or area scripts. Model/AI and animation compatibility still require an in-game test. Quest events can override settings.");
                    }
                    if(ImGui::Button("Show assigned patrol") && e.patrol.index>=0) {
                        const auto& routes=scene.document.Routes();const auto found=std::ranges::find(routes,e.patrol.index,&Route::key);
                        if(found!=routes.end())s.SelectRoute(int(found-routes.begin()),true);
                    }
                    if(ImGui::Button("Clear patrol assignment"))s.Action([&]{auto data=*scene.document.Parts()[s.selected].enemy;data.patrol={};scene.document.SetEnemy(s.selected,data);});
                    if(ImGui::CollapsingHeader("Enemy references",ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("Identifiers and resolved references");
                        ImGui::Text("MSB type: %s",e.dummy?"DummyEnemy":"Enemy");
                        ImGui::Text("Entity ID: %d",e.entity_id);ImGui::Text("Instance ID: %d",e.instance_id);
                        ImGui::Text("NPC: %d / AI: %d",e.npc_param,e.think_param);
                        ImGui::Text("Talk: %d / CharaInit: %d",e.talk_id,e.chara_init);
                        ImGui::Text("Initial / damage anim: %d / %d",e.init_anim,e.damage_anim);
                        auto reference=[](const char* label,const Reference& r) {
                            if(r.index==-1) ImGui::TextDisabled("%s: none",label);
                            else if(r.Missing()) ImGui::TextWrapped("%s: UNRESOLVED index %d",label,r.index);
                            else ImGui::TextWrapped("%s: %s [%d]",label,r.name.c_str(),r.index);
                        };
                        reference("Loading collision",e.collision);reference("Patrol",e.patrol);
                        for(int i=0;i<8;++i) if(e.move_points[i].index!=-1) {const auto label="Move point "+std::to_string(i+1);reference(label.c_str(),e.move_points[i]);}
                        ImGui::TextWrapped("Open the assigned patrol to edit its points. Events can override routes or placement. Reload to test.");
                    }
                } else {
                    if(o.type==9)ImGui::TextWrapped("Dormant DummyObject. Duplicating it as a pickup creates an active Object copy.");
                    auto model=scene.models.find(o.model);
                    if(model==scene.models.end() || !model->second) ImGui::TextWrapped("Model unavailable. This object's placement can still be edited.");
                }
            } else if(s.selected_region>=0) {
                const auto& r=scene.document.Regions()[s.selected_region];ImGui::TextWrapped("%s",r.name.c_str());
                ImGui::Text("Entity ID: %d / shape: %u",r.entity_id,r.shape);
                ImGui::TextUnformatted("Position (metres)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##region-position",s.fields.position.data(),"%.3f");
                ImGui::TextUnformatted("Rotation (degrees)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##region-rotation",s.fields.rotation.data(),"%.2f");
                if(ImGui::Button("Apply waypoint transform"))s.Action([&]{s.SetTransform(s.fields);});
                ImGui::SameLine();if(ImGui::Button("Reset"))s.Action([&]{s.SetTransform(r.original);});
                if(r.added && ImGui::Button("Remove waypoint"))s.pending=State::Pending::Remove;
                ImGui::TextWrapped("Marker shows the region origin. Existing volume dimensions are preserved.");
            } else ImGui::TextWrapped("Select a placement, pickup, region or patrol in the list, or click its marker.");
            if(s.selected_treasure>=0) {
                const auto& t=scene.document.Treasures()[s.selected_treasure];ImGui::Separator();ImGui::TextWrapped("Pickup: %s",t.name.c_str());
                ImGui::TextWrapped("Object: %s",t.data.part.name.c_str());ImGui::Text("Item lots: %d / %d / %d",t.data.lots[0],t.data.lots[1],t.data.lots[2]);
                if(const auto lot=s.CurrentLot())ImGui::Text("Collection flag: %d",lot->Flag());
                if(ImGui::Button("Duplicate pickup  Cmd+D"))s.pending=State::Pending::Duplicate;
                ImGui::BeginDisabled(!t.added);if(ImGui::Button("Remove pickup"))s.pending=State::Pending::Remove;ImGui::EndDisabled();
                const char* categories[]={"Weapon","Armor","Consumable"};const int values[]={0,1,4};int category=s.item_fields.category==0?0:s.item_fields.category==1?1:s.item_fields.category==4?2:-1;
                if(ImGui::Combo("Category",&category,categories,3))s.item_fields.category=values[category];
                const char* table=s.item_fields.category==0?"EquipParamWeapon":s.item_fields.category==1?"EquipParamProtector":s.item_fields.category==4?"EquipParamGoods":"";
                ParamChooser("Item",&s.item_fields.item,scene.params.Table(table));ImGui::InputInt("Count",&s.item_fields.count);
                if(ImGui::Button("Apply item contents"))s.Action([&]{scene.document.SetItemContents(s.selected_treasure,s.item_fields);});
                auto data=t.data;if(ImGui::Checkbox("Start disabled",&data.disabled))scene.document.SetTreasure(s.selected_treasure,data);
                ImGui::TextWrapped("Apply gives one guaranteed stack. Existing pickup edits retain their collection flag; duplicated pickups get an independent flag. Export includes item parameters.");
            }
            if(s.active_route>=0 && ImGui::CollapsingHeader("Active patrol",ImGuiTreeNodeFlags_DefaultOpen)) {
                const auto& route=scene.document.Routes()[s.active_route];ImGui::TextWrapped("%s",route.name.c_str());
                const char* labels[]={"Loop","Reverse route","Random point","Unknown (3)","Unknown (4)","Walk once","Wander between points"};
                const auto type=route.data.type;const auto preview=type>=0 && type<7?labels[type]:"Unknown";
                if(ImGui::BeginCombo("Behavior",preview)) {
                    for(int n:{0,1,2,5,6})if(ImGui::Selectable(labels[n],n==type))s.Action([&]{auto data=route.data;data.type=n;scene.document.SetRoute(s.active_route,data);});
                    ImGui::EndCombo();
                }
                ImGui::BeginDisabled(s.selected_region<0);
                if(ImGui::Button("Append selected waypoint"))s.Action([&]{s.AppendWaypoint();});ImGui::EndDisabled();
                ImGui::BeginDisabled(s.selected<0 || !objects[s.selected].enemy);
                if(ImGui::Button("Assign to selected character"))s.Action([&]{s.AssignRoute();});ImGui::EndDisabled();
                ImGui::TextWrapped("Assigning a patrol replaces direct move points. Undo restores them.");
                for(int n=0;n<32;++n) {
                    const auto& point=route.data.points[n];if(point.index==-1)continue;ImGui::PushID(n);
                    const auto& regions=scene.document.Regions();const auto found=std::ranges::find(regions,point.index,&Region::key);
                    const auto label=std::to_string(n+1)+". "+(found==regions.end()?"Missing waypoint "+std::to_string(point.index):found->name);
                    if(ImGui::Selectable(label.c_str(),found!=regions.end() && int(found-regions.begin())==s.selected_region) && found!=regions.end())s.SelectRegion(int(found-regions.begin()),true);
                    ImGui::BeginDisabled(n==0);if(ImGui::SmallButton("Up"))s.Action([&]{auto data=route.data;std::swap(data.points[n],data.points[n-1]);scene.document.SetRoute(s.active_route,data);});ImGui::EndDisabled();
                    ImGui::SameLine();ImGui::BeginDisabled(n==31 || route.data.points[n+1].index==-1);
                    if(ImGui::SmallButton("Down"))s.Action([&]{auto data=route.data;std::swap(data.points[n],data.points[n+1]);scene.document.SetRoute(s.active_route,data);});ImGui::EndDisabled();
                    ImGui::SameLine();if(ImGui::SmallButton("Unlink"))s.Action([&]{auto data=route.data;std::move(data.points.begin()+n+1,data.points.end(),data.points.begin()+n);data.points.back()={};scene.document.SetRoute(s.active_route,data);});
                    ImGui::PopID();
                }
                if(route.added && ImGui::Button("Remove patrol"))s.pending=State::Pending::RemoveRoute;
                ImGui::TextWrapped("Routes use existing navigation. Keep points on connected walkable ground in the character's loading area.");
            }
            ImGui::Spacing();ImGui::TextWrapped("Drag the colored axis handles to move. Position is in metres; rotation is in degrees.");
            ImGui::Checkbox("Cut away roofs",&s.cutaway);
            if(s.cutaway) {ImGui::SetNextItemWidth(-1);ImGui::DragFloat("##ceiling",&s.ceiling,.25f,-1000,1000,"Ceiling: %.2f m");}
            const bool changed_objects=ImGui::Checkbox("Show objects",&s.show_objects);
            const bool changed_enemies=ImGui::Checkbox("Show enemies",&s.show_enemies);
            if(ImGui::Checkbox("Show regions",&s.show_regions) && !s.show_regions && s.selected_region>=0)s.Select(-1,false);
            if(s.show_regions)ImGui::Checkbox("Active patrol only",&s.route_only);
            if(changed_objects || changed_enemies) {
                if(s.selected>=0 && ((objects[s.selected].kind==PartKind::Enemy && !s.show_enemies) || (objects[s.selected].kind==PartKind::Object && !s.show_objects))) s.Select(-1,false);
            }
            ImGui::TextWrapped("Copies share the source's collision and behavior settings. Move them nearby, then reload to test.");
            if(scene.missing_models) ImGui::TextWrapped("%zu object models unavailable.",scene.missing_models);
            ImGui::EndChild();ImGui::SameLine();
            ImGui::BeginChild("Metal viewport",{0,-42},ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextDisabled("Right drag: orbit   Middle drag: pan   Wheel: zoom   F: frame selection");
            auto top=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();size.x=std::max(1.f,size.x);size.y=std::max(1.f,size.y);
            if(!s.fitted) {
                const auto& p=scene.terrain->positions;float x0=FLT_MAX,x1=-FLT_MAX,z0=FLT_MAX,z1=-FLT_MAX,y0=FLT_MAX,y1=-FLT_MAX;
                for(std::size_t i=0;i+2<p.size();i+=3) {x0=std::min(x0,p[i]);x1=std::max(x1,p[i]);y0=std::min(y0,p[i+1]);y1=std::max(y1,p[i+1]);z0=std::min(z0,p[i+2]);z1=std::max(z1,p[i+2]);}
                s.camera.x=(x0+x1)*.5f;s.camera.y=(y0+y1)*.5f;s.camera.z=(z0+z1)*.5f;
                s.camera.zoom=BbMapView::FitZoom(s.camera,x0,z0,x1,z1,size.x,size.y,y0,y1);s.fitted=true;
            }
            const float dpi=std::max(1.f,io.DisplayFramebufferScale.x);
            _viewport->Render(command,scene,s.camera,size,dpi,s.selected,s.selected_region,s.drag,s.cutaway?s.ceiling:1000000.f,
                              s.show_objects,s.show_enemies,s.show_regions,s.active_route,s.route_only);
            ImGui::Image((ImTextureID)(__bridge void*)_viewport->color,size);
            const bool hovered=ImGui::IsItemHovered();
            if(hovered) {
                s.camera.zoom=std::clamp(s.camera.zoom*std::pow(1.12f,io.MouseWheel),.05f,1000.f);
                if(ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {s.camera.yaw-=io.MouseDelta.x*.008f;s.camera.pitch=std::clamp(s.camera.pitch+io.MouseDelta.y*.008f,.08f,1.52f);}
                if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {auto a=s.camera.Ground(0,0),b=s.camera.Ground(io.MouseDelta.x,io.MouseDelta.y);s.camera.x-=b[0]-a[0];s.camera.z-=b[1]-a[1];}
                if(ImGui::IsKeyPressed(ImGuiKey_F)) {if(s.selected_treasure>=0)s.SelectTreasure(s.selected_treasure,true);else if(s.selected>=0)s.Select(s.selected,true);else if(s.selected_region>=0)s.SelectRegion(s.selected_region,true);}
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pick=ImVec2(io.MousePos.x-top.x,io.MousePos.y-top.y);
            }
            if(s.show_regions && s.active_route>=0) {
                const auto& route=scene.document.Routes()[s.active_route];auto* dl=ImGui::GetWindowDrawList();
                std::optional<ImVec2> first,previous;int number=0;
                for(const auto& point:route.data.points) {
                    if(point.index==-1)continue;++number;
                    const auto& regions=scene.document.Regions();const auto found=std::ranges::find(regions,point.index,&Region::key);if(found==regions.end())continue;
                    const auto& position=s.selected_region==int(found-regions.begin()) && s.drag?s.drag->position:found->transform.position;
                    const auto p=s.camera.Project(position[0],position[1]+.32f,position[2]);const ImVec2 at(top.x+size.x*.5f+p[0],top.y+size.y*.5f+p[1]);
                    if(previous)dl->AddLine(*previous,at,0xff66ddaau,2);else first=at;
                    dl->AddCircleFilled(at,4,0xff66ddaau);const auto label=std::to_string(number);dl->AddText({at.x+7,at.y-16},0xffbbffdd,label.c_str());previous=at;
                }
                if(route.data.type==0 && first && previous)dl->AddLine(*previous,*first,0x8866ddaau,1);
            }
            if(s.Current()) {
                const auto& t=s.drag?*s.drag:*s.Current();
                auto origin=s.camera.Project(t.position[0],t.position[1],t.position[2]);
                ImVec2 center(top.x+size.x*.5f+origin[0],top.y+size.y*.5f+origin[1]);
                auto* dl=ImGui::GetWindowDrawList();const ImU32 colors[]={0xff6666ff,0xff88dd66,0xffffbb66};
                for(int axis=0;axis<3;++axis) {
                    auto p=t.position;p[axis]+=1;auto projected=s.camera.Project(p[0],p[1],p[2]);
                    float dx=projected[0]-origin[0],dy=projected[1]-origin[1],len=std::sqrt(dx*dx+dy*dy);
                    if(len<.001f) continue;
                    ImVec2 end(center.x+dx/len*65,center.y+dy/len*65);
                    dl->AddLine(center,end,colors[axis],3);dl->AddCircleFilled(end,6,colors[axis]);dl->AddText({end.x+8,end.y-8},colors[axis],axis==0?"X":axis==1?"Y":"Z");
                    const float mx=io.MousePos.x-end.x,my=io.MousePos.y-end.y;
                    if(hovered && ImGui::IsMouseClicked(0) && mx*mx+my*my<144) {s.drag_axis=axis;s.drag_start=*s.Current();s.drag=s.drag_start;s.drag_mouse=io.MousePos;pick.reset();}
                    if(s.drag_axis==axis && s.drag) {
                        const float amount=((io.MousePos.x-s.drag_mouse.x)*dx+(io.MousePos.y-s.drag_mouse.y)*dy)/(len*len);
                        s.drag->position[axis]=s.drag_start.position[axis]+amount;
                    }
                }
                if(s.drag && !ImGui::IsMouseDown(0)) {s.Action([&]{s.SetTransform(*s.drag);});s.drag.reset();s.drag_axis=-1;}
            }
            ImGui::EndChild();
            ImGui::TextUnformatted(s.Unsaved()?"Unexported changes":"Map ready");ImGui::SameLine();ImGui::TextDisabled("%zu terrain triangles",scene.terrain->indices.size()/3);
            if(!s.status.empty()) ImGui::TextWrapped("%s",s.status.c_str());
        }
        ImGui::End();ImGui::Render();
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].clearColor=MTLClearColorMake(.04,.05,.06,1);
        auto encoder=[command renderCommandEncoderWithDescriptor:pass];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),command,encoder);[encoder endEncoding];
        id<MTLBuffer> pixels=nil;const auto shot=s.capture;
        if(!shot.empty()) {
            pixels=[_device newBufferWithLength:drawable.texture.width*drawable.texture.height*4 options:MTLResourceStorageModeShared];
            auto blit=[command blitCommandEncoder];[blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(drawable.texture.width,drawable.texture.height,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:drawable.texture.width*4 destinationBytesPerImage:drawable.texture.width*drawable.texture.height*4];[blit endEncoding];
        }
        [command presentDrawable:drawable];[command commit];
        if(pick || pixels) {
            [command waitUntilCompleted];
            if(command.status==MTLCommandBufferStatusError) throw std::runtime_error(command.error.localizedDescription.UTF8String);
            if(pixels) {SaveImage(shot,pixels,drawable.texture.width,drawable.texture.height);s.capture.clear();}
            if(pick) {
                std::uint32_t id=0;
                auto x=std::clamp(int(pick->x*io.DisplayFramebufferScale.x),0,int(_viewport->ids.width)-1),y=std::clamp(int(pick->y*io.DisplayFramebufferScale.y),0,int(_viewport->ids.height)-1);
                [_viewport->ids getBytes:&id bytesPerRow:4 fromRegion:MTLRegionMake2D(x,y,1,1) mipmapLevel:0];
                if(id&0x80000000u)s.SelectRegion(int((id&0x7fffffffu)-1),false);else {
                    s.Select(id?int(id)-1:-1,false);
                    if(s.layer==6 && s.selected>=0){const auto& doc=s.scene->document;const auto& ts=doc.Treasures();const auto found=std::ranges::find(ts,doc.Parts()[s.selected].name,[](const auto& t){return t.data.part.name;});if(found!=ts.end())s.SelectTreasure(int(found-ts.begin()),false);}
                }
            }
        }
    } catch(const std::exception& e) {
        state->error=e.what();std::fprintf(stderr,"Map editor: %s\n",e.what());
        if(!state->commands.empty()) {exit_code=1;Finish();}
    }
  }
}
@end

@interface EditorDelegate : NSObject<NSApplicationDelegate>
@property(strong) NSWindow* window;
@property(strong) EditorController* controller;
@end
@implementation EditorDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {return YES;}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    if(!state || state->commands.size() || state->Discard()) Finish();
    return NSTerminateCancel;
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    try {
        if(options.game.empty()) {
            auto panel=[NSOpenPanel openPanel];panel.title=@"Choose your Bloodborne game folder";panel.canChooseDirectories=YES;panel.canChooseFiles=NO;
            if([panel runModal]!=NSModalResponseOK) {Finish();return;}
            options.game=panel.URL.fileSystemRepresentation;
        }
        state=std::make_unique<State>();
        for(const auto& entry:fs::directory_iterator(options.game/"dvdroot_ps4/map/MapStudio")) {
            auto name=entry.path().filename().string();if(name.ends_with(".msb.dcx") && ValidMapName(name.substr(0,12)))state->maps.push_back(name.substr(0,12));
        }
        std::sort(state->maps.begin(),state->maps.end());
        if(!options.script.empty()) {std::ifstream f(options.script);if(!f)throw std::runtime_error("Cannot read editor script");for(std::string line;std::getline(f,line);)if(!line.empty() && line[0]!='#')state->commands.push_back(line);}
        self.controller=[EditorController new];
        self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(80,80,1360,850) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
        self.window.title=@"Bloodborne Map Editor";self.window.contentViewController=self.controller;self.window.delegate=self.controller;self.window.minSize=NSMakeSize(1000,700);
        [self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];state->Load();
    } catch(const std::exception& e) {std::fprintf(stderr,"Map editor: %s\n",e.what());exit_code=1;Finish();}
}
@end

int main(int argc,char** argv) {
  @autoreleasepool {
    try {
        for(int i=1;i<argc;++i) {
            const std::string key=argv[i];
            auto value=[&](){if(i+1==argc)throw std::runtime_error("Missing value for "+key);return std::string(argv[++i]);};
            if(key=="--game")options.game=value();else if(key=="--map")options.map=value();
            else if(key=="--select")options.select=value();else if(key=="--export")options.output=value();
            else if(key=="--script")options.script=value();else if(key=="--batch")options.batch=true;
            else if(key=="--list")options.list=true;
            else if(key=="--list-regions")options.list_regions=true;
            else if(key=="--list-routes")options.list_routes=true;
            else if(key=="--list-treasures")options.list_treasures=true;
            else if(key=="--list-param")options.list_param=value();
            else if(key=="--duplicate")options.duplicate=true;
            else if(key=="--translate") {BbMapArt::Vec3 v;for(auto& f:v) {const auto text=value();std::size_t used;f=std::stof(text,&used);if(used!=text.size())throw std::runtime_error("Invalid translation");}options.translation=v;}
            else if(key=="--help") {std::puts("bbmap-editor [--game folder] [--map m24_01_00_00] [--select part] [--script file]\n  --batch --select part [--duplicate] --translate X Y Z --export new-mod-folder\n  --list / --list-regions / --list-routes / --list-treasures: inspect map records\n  --list-param NpcParam|NpcThinkParam|CharaInitParam|ItemLotParam|EquipParamGoods|TalkScripts");return 0;}
            else throw std::runtime_error("Unknown option: "+key);
        }
        if(!ValidMapName(options.map)) throw std::runtime_error("Expected a map name such as m24_01_00_00");
        if(!options.list_param.empty()) {
            if(options.list_param=="TalkScripts") {for(const auto& [id,name]:GameParams::TalkScripts(options.game,options.map))std::printf("%d\t%s\n",id,name.c_str());return 0;}
            const auto params=GameParams::Open(options.game);const auto* table=params.Table(options.list_param);
            if(!table)throw std::runtime_error("Unknown editor parameter table: "+options.list_param);
            for(const auto& row:table->rows)std::printf("%d\t%s\n",row.id,row.name.c_str());return 0;
        }
        const bool inspect=options.list || options.list_regions || options.list_routes || options.list_treasures;
        if((options.translation || options.duplicate) && (!options.batch || inspect))throw std::runtime_error("Use --batch for command-line edits");
        if(options.batch || inspect) {
            auto doc=Document::Open(options.game/"dvdroot_ps4/map/MapStudio"/(options.map+".msb.dcx"));
            if(options.list) {for(const auto& o:doc.Parts()) std::printf("%s\t%s\t%.6f %.6f %.6f\t%s\t%d\n",o.name.c_str(),o.model.c_str(),o.transform.position[0],o.transform.position[1],o.transform.position[2],o.enemy?"enemy":"object",o.entity_id);return 0;}
            if(options.list_regions) {for(const auto& r:doc.Regions())std::printf("%s\t%.6f %.6f %.6f\t%u\t%d\n",r.name.c_str(),r.transform.position[0],r.transform.position[1],r.transform.position[2],r.shape,r.entity_id);return 0;}
            if(options.list_routes) {for(const auto& r:doc.Routes()){std::printf("%s\t%d\t%d",r.name.c_str(),r.key,r.data.type);for(const auto& p:r.data.points)if(p.index!=-1)std::printf("\t%s [%d]",p.name.c_str(),p.index);std::puts("");}return 0;}
            if(options.list_treasures) {for(const auto& t:doc.Treasures())std::printf("%s\t%s\t%d %d %d\tchest=%d disabled=%d\n",t.name.c_str(),t.data.part.name.c_str(),t.data.lots[0],t.data.lots[1],t.data.lots[2],t.data.in_chest,t.data.disabled);return 0;}
            if(options.output.empty())throw std::runtime_error("--batch needs --export with a new mod folder");
            if(options.translation || options.duplicate) {
                const auto& objects=doc.Parts();auto found=std::ranges::find(objects,options.select,&Part::name);
                if(found==objects.end())throw std::runtime_error("Placement not found");
                auto index=std::size_t(found-objects.begin());if(options.duplicate)index=doc.Duplicate(index);
                auto t=doc.Parts()[index].transform;
                if(options.translation)for(int i=0;i<3;++i)t.position[i]+=(*options.translation)[i];
                doc.Set(index,t);
                if(options.duplicate)std::printf("CREATED %s entity=%d instance=%d\n",doc.Parts()[index].name.c_str(),doc.Parts()[index].entity_id,doc.Parts()[index].instance_id);
            }
            std::printf("EXPORTED %s\n",doc.Export(fs::absolute(options.output),options.map).c_str());return 0;
        }
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        auto menu=[NSMenu new];auto root=[NSMenuItem new];[menu addItem:root];auto app=[NSMenu new];
        [app addItemWithTitle:@"Quit Map Editor" action:@selector(terminate:) keyEquivalent:@"q"];root.submenu=app;NSApp.mainMenu=menu;
        auto delegate=[EditorDelegate new];NSApp.delegate=delegate;[NSApp run];
        if(state) {state->stop.request_stop();state.reset();}
        if(ImGui::GetCurrentContext()) {ImGui_ImplOSX_Shutdown();ImGui_ImplMetal_Shutdown();ImGui::DestroyContext();}
        return exit_code;
    } catch(const std::exception& e) {std::fprintf(stderr,"Map editor: %s\n",e.what());return 1;}
  }
}
