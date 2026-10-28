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
    std::string map="m24_01_00_00",select;
    std::optional<BbMapArt::Vec3> translation;
    bool batch=false,list=false;
};
Options options;
int exit_code=0;
void Finish() {
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
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
                ImVec2 size,float dpi,int selected,const std::optional<Transform>& preview,float ceiling) {
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
            c.options[0]=id;c.options[1]=id && int(id)==selected+1;
            [encoder setVertexBuffer:gpu.positions offset:0 atIndex:0];[encoder setVertexBuffer:gpu.colors offset:0 atIndex:1];
            [encoder setVertexBytes:&c length:sizeof(c) atIndex:2];[encoder setFragmentBytes:&c length:sizeof(c) atIndex:2];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:gpu.count indexType:MTLIndexTypeUInt32 indexBuffer:gpu.indices indexBufferOffset:0];
        };
        draw(scene.terrain,{},0);
        const auto& objects=scene.document.Objects();
        for(std::size_t i=0;i<objects.size();++i) {
            auto found=scene.models.find(objects[i].model);
            if(found!=scene.models.end()) draw(found->second,int(i)==selected && preview?*preview:objects[i].transform,std::uint32_t(i+1));
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
    std::vector<std::string> maps,commands;
    std::size_t command=0;
    int frames=0,selected=-1;
    bool fitted=false;
    bool cutaway=false;
    float ceiling=10000;
    BbMapView::Camera camera{0,0,0,4,.45f,.85f};
    Transform fields;
    char filter[128]={};
    std::optional<Transform> drag;
    Transform drag_start;
    ImVec2 drag_mouse{};
    int drag_axis=-1;
    // Optional local input replay for reproducible viewport/picking checks; no system events.
    std::optional<std::array<ImVec2,2>> input_drag;
    int input_frame=0;
    fs::path capture;
    std::vector<std::uint8_t> exported;
    ~State() {stop.request_stop();if(loading.valid()) loading.wait();}
    bool Unsaved() const {return scene && scene->document.Changed() && scene->document.Inflated()!=exported;}
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
        scene.reset();selected=-1;fitted=false;cutaway=false;exported.clear();error.clear();progress="Opening map...";
        const auto game=options.game;
        const auto map=options.map;
        loading=std::async(std::launch::async,[this,game,map] {
            return std::make_shared<Scene>(LoadScene(game,map,[this](std::string s){std::scoped_lock lock(mutex);progress=std::move(s);},stop.get_token()));
        });
    }
    void Select(int index,bool focus) {
        selected=index;drag.reset();drag_axis=-1;
        if(!scene || index<0) return;
        const auto& object=scene->document.Objects().at(index);fields=object.transform;
        if(focus) {camera.x=fields.position[0];camera.y=fields.position[1]+1;camera.z=fields.position[2];camera.zoom=45;cutaway=true;ceiling=fields.position[1]+4;}
    }
    void Export(const fs::path& folder) {
        if(!scene) return;
        auto path=scene->document.Export(fs::absolute(folder),options.map);
        exported=scene->document.Inflated();status="Exported mod: "+folder.filename().string()+". Enable it in Mods & Cheats, then reload the area.";
        std::printf("EXPORTED %s\n",path.c_str());
    }
    void ExportDialog() {
        if(!options.output.empty()) {Export(options.output);return;}
        auto panel=[NSSavePanel savePanel];panel.title=@"Export map mod to a new folder";
        panel.nameFieldStringValue=@"Bloodborne-map-mod";panel.canCreateDirectories=YES;
        if([panel runModal]==NSModalResponseOK) Export(panel.URL.fileSystemRepresentation);
    }
    void Poll() {
        if(loading.valid() && loading.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {scene=loading.get();std::printf("LOADED %s: %zu objects, %zu terrain triangles, %zu missing models\n",options.map.c_str(),scene->document.Objects().size(),scene->terrain->indices.size()/3,scene->missing_models);}
            catch(const std::exception& e) {error=e.what();if(!commands.empty()) {exit_code=1;Finish();}}
            frames=0;
            if(scene && !options.select.empty()) {
                const auto& objects=scene->document.Objects();auto it=std::ranges::find(objects,options.select,&Object::name);
                if(it==objects.end()) throw std::runtime_error("Selected object was not found");
                Select(int(it-objects.begin()),true);fitted=true;
            }
        }
    }
    void Script() {
        if(!scene || command>=commands.size() || ++frames<12) return;
        frames=0;std::istringstream line(commands[command++]);std::string verb;line>>verb;
        std::printf("EDITOR %s\n",commands[command-1].c_str());
        if(verb=="select") {
            std::string name;line>>name;const auto& objects=scene->document.Objects();auto it=std::ranges::find(objects,name,&Object::name);
            if(it==objects.end()) throw std::runtime_error("Script object not found: "+name);
            Select(int(it-objects.begin()),true);fitted=true;
        } else if(verb=="deselect") Select(-1,false);
        else if(verb=="click" || verb=="drag") {
            std::array<ImVec2,2> p;
            if(!(line>>p[0].x>>p[0].y)) throw std::runtime_error("click needs x y");
            p[1]=p[0];
            if(verb=="drag" && !(line>>p[1].x>>p[1].y)) throw std::runtime_error("drag needs x y end-x end-y");
            input_drag=p;input_frame=0;
        } else if(verb=="assert-selected") {
            std::string name;line>>name;
            if(selected<0 || scene->document.Objects()[selected].name!=name) throw std::runtime_error("Viewport selected the wrong object");
        } else if(verb=="assert-position") {
            if(selected<0) throw std::runtime_error("Nothing selected");
            BbMapArt::Vec3 expected;for(float& v:expected)if(!(line>>v))throw std::runtime_error("assert-position needs x y z tolerance");
            float tolerance;if(!(line>>tolerance))throw std::runtime_error("Missing position tolerance");
            for(int i=0;i<3;++i)if(std::abs(scene->document.Objects()[selected].transform.position[i]-expected[i])>tolerance)throw std::runtime_error("Unexpected edited position");
        } else if(verb=="move") {
            if(selected<0) throw std::runtime_error("Select an object before moving it");
            auto t=scene->document.Objects()[selected].transform;
            for(auto& v:t.position) {float delta;if(!(line>>delta)) throw std::runtime_error("move needs three numbers");v+=delta;}
            scene->document.Set(selected,t);fields=t;
        } else if(verb=="undo" || verb=="redo") {
            if(!(verb=="undo"?scene->document.Undo():scene->document.Redo())) throw std::runtime_error("Empty undo/redo history");
            if(selected>=0) fields=scene->document.Objects()[selected].transform;
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
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Different game phases can load different map variants. Export the variant used by your test save.");
        ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!s.scene);
        if(ImGui::Button("Export mod...  Cmd+Shift+E") || ImGui::Shortcut(ImGuiMod_Super|ImGuiMod_Shift|ImGuiKey_E)) s.Action([&]{s.ExportDialog();});
        ImGui::SameLine();
        if(ImGui::Button("Undo") || ImGui::Shortcut(ImGuiMod_Super|ImGuiKey_Z)) {s.scene->document.Undo();if(s.selected>=0)s.fields=s.scene->document.Objects()[s.selected].transform;}
        ImGui::SameLine();
        if(ImGui::Button("Redo") || ImGui::Shortcut(ImGuiMod_Super|ImGuiMod_Shift|ImGuiKey_Z)) {s.scene->document.Redo();if(s.selected>=0)s.fields=s.scene->document.Objects()[s.selected].transform;}
        ImGui::EndDisabled();
        if(!s.error.empty()) ImGui::TextWrapped("%s",s.error.c_str());
        if(!s.scene) {std::scoped_lock lock(s.mutex);ImGui::TextWrapped("%s",s.progress.c_str());ImGui::TextDisabled("Loading your game assets. The window remains responsive.");}
        std::optional<ImVec2> pick;
        if(s.scene) {
            auto& scene=*s.scene;const auto& objects=scene.document.Objects();
            ImGui::BeginChild("Objects and transform",{300,-42},ImGuiChildFlags_Borders);
            ImGui::Text("Objects (%zu)",objects.size());ImGui::InputTextWithHint("##filter","Filter by object or model",s.filter,sizeof(s.filter));
            ImGui::BeginChild("Object list",{0,235},ImGuiChildFlags_Borders);
            for(std::size_t i=0;i<objects.size();++i) {
                const auto& o=objects[i];if(s.filter[0] && o.name.find(s.filter)==std::string::npos && o.model.find(s.filter)==std::string::npos) continue;
                if(ImGui::Selectable(o.name.c_str(),int(i)==s.selected)) s.Select(int(i),true);
            }
            ImGui::EndChild();ImGui::Separator();
            if(s.selected>=0) {
                const auto& o=objects[s.selected];ImGui::TextWrapped("%s",o.name.c_str());ImGui::TextDisabled("Model: %s",o.model.c_str());
                ImGui::TextUnformatted("Position (metres)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##position",s.fields.position.data(),"%.3f");
                ImGui::TextUnformatted("Rotation (degrees)");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##rotation",s.fields.rotation.data(),"%.2f");
                ImGui::TextUnformatted("Scale");ImGui::SetNextItemWidth(-1);ImGui::InputFloat3("##scale",s.fields.scale.data(),"%.3f");
                if(ImGui::Button("Apply transform")) s.Action([&]{scene.document.Set(s.selected,s.fields);});
                ImGui::SameLine();if(ImGui::Button("Reset")) {scene.document.Set(s.selected,o.original);s.fields=o.original;}
                if(ImGui::Button("Frame selected  F")) s.Select(s.selected,true);
                auto model=scene.models.find(o.model);
                if(model==scene.models.end() || !model->second) ImGui::TextWrapped("Model unavailable. This object's placement can still be edited.");
            } else ImGui::TextWrapped("Select an object in the list or click its geometry.");
            ImGui::Spacing();ImGui::TextWrapped("Drag the colored axis handles to move. Position is in metres; rotation is in degrees.");
            ImGui::Checkbox("Cut away roofs",&s.cutaway);
            if(s.cutaway) {ImGui::SetNextItemWidth(-1);ImGui::DragFloat("##ceiling",&s.ceiling,.25f,-1000,1000,"Ceiling: %.2f m");}
            ImGui::TextDisabled("Existing object placements only.");
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
            _viewport->Render(command,scene,s.camera,size,dpi,s.selected,s.drag,s.cutaway?s.ceiling:1000000.f);
            ImGui::Image((ImTextureID)(__bridge void*)_viewport->color,size);
            const bool hovered=ImGui::IsItemHovered();
            if(hovered) {
                s.camera.zoom=std::clamp(s.camera.zoom*std::pow(1.12f,io.MouseWheel),.05f,1000.f);
                if(ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {s.camera.yaw-=io.MouseDelta.x*.008f;s.camera.pitch=std::clamp(s.camera.pitch+io.MouseDelta.y*.008f,.08f,1.52f);}
                if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {auto a=s.camera.Ground(0,0),b=s.camera.Ground(io.MouseDelta.x,io.MouseDelta.y);s.camera.x-=b[0]-a[0];s.camera.z-=b[1]-a[1];}
                if(s.selected>=0 && ImGui::IsKeyPressed(ImGuiKey_F)) s.Select(s.selected,true);
                if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pick=ImVec2(io.MousePos.x-top.x,io.MousePos.y-top.y);
            }
            if(s.selected>=0) {
                const auto& t=s.drag?*s.drag:objects[s.selected].transform;
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
                    if(hovered && ImGui::IsMouseClicked(0) && mx*mx+my*my<144) {s.drag_axis=axis;s.drag_start=objects[s.selected].transform;s.drag=s.drag_start;s.drag_mouse=io.MousePos;pick.reset();}
                    if(s.drag_axis==axis && s.drag) {
                        const float amount=((io.MousePos.x-s.drag_mouse.x)*dx+(io.MousePos.y-s.drag_mouse.y)*dy)/(len*len);
                        s.drag->position[axis]=s.drag_start.position[axis]+amount;
                    }
                }
                if(s.drag && !ImGui::IsMouseDown(0)) {s.Action([&]{scene.document.Set(s.selected,*s.drag);});s.fields=scene.document.Objects()[s.selected].transform;s.drag.reset();s.drag_axis=-1;}
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
                s.Select(id?int(id)-1:-1,false);
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
            else if(key=="--translate") {BbMapArt::Vec3 v;for(auto& f:v) {const auto text=value();std::size_t used;f=std::stof(text,&used);if(used!=text.size())throw std::runtime_error("Invalid translation");}options.translation=v;}
            else if(key=="--help") {std::puts("bbmap-editor [--game folder] [--map m24_01_00_00] [--select object] [--script file]\n  --batch --select object --translate X Y Z --export new-mod-folder\n  --list: inspect existing object placements without opening the editor");return 0;}
            else throw std::runtime_error("Unknown option: "+key);
        }
        if(!ValidMapName(options.map)) throw std::runtime_error("Expected a map name such as m24_01_00_00");
        if(options.translation && !options.batch) throw std::runtime_error("Use --batch for a command-line translation, or move the object in the editor");
        if(options.batch || options.list) {
            auto doc=Document::Open(options.game/"dvdroot_ps4/map/MapStudio"/(options.map+".msb.dcx"));
            if(options.list) {for(const auto& o:doc.Objects()) std::printf("%s\t%s\t%.6f %.6f %.6f\n",o.name.c_str(),o.model.c_str(),o.transform.position[0],o.transform.position[1],o.transform.position[2]);return 0;}
            if(options.output.empty())throw std::runtime_error("--batch needs --export with a new mod folder");
            if(options.translation) {
                const auto& objects=doc.Objects();auto found=std::ranges::find(objects,options.select,&Object::name);
                if(found==objects.end())throw std::runtime_error("Object not found");
                auto t=found->transform;for(int i=0;i<3;++i)t.position[i]+=(*options.translation)[i];doc.Set(found-objects.begin(),t);
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
