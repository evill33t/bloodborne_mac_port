// SPDX-License-Identifier: GPL-2.0-or-later
// Metal fixture harness follows Mark Hall's test_mtl_overlay.mm. Fixture snapshots test
// theme isolation, input ownership, native GPU resource lifetime and small/wide layouts.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include "bbport_coop_ui.h"
#include "bbport_coop_style.h"
#include "bbport_map_charts.h"
#include <fstream>
#include <thread>
#include <unistd.h>
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "video_core/renderer_metal/mtl_device.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>
int main() {
    unsetenv("BB_MENU_OPEN");unsetenv("BB_OVERLAY");
    char temp[]="/tmp/bb-coop-parity-XXXXXX";assert(mkdtemp(temp));
    const std::filesystem::path root(temp);std::filesystem::create_directories(root/"dvdroot_ps4/map");
    const auto cache=root/"map/metal-v1-cbf29ce484222325";std::filesystem::create_directories(cache);
    BbMapArt::Chart chart;chart.width=chart.height=8;chart.ppm=1;chart.x0=-4;chart.z1=4;chart.rgba.assign(8*8*4,200);
    for(size_t i=3;i<chart.rgba.size();i+=4) chart.rgba[i]=255;
    chart.labels.push_back({19,0,0});chart.storeys.width=chart.storeys.height=4;
    chart.storeys.roof.assign(16,100);chart.storeys.level.assign(48,50);chart.storeys.rgb.assign(48,0xff888888);
    BbMapArt::AreaMesh mesh;mesh.positions={-4,10,-4,4,10,-4,0,10,4};mesh.colors.assign(3,0xff888888);mesh.indices={0,1,2};
    auto write=[](const auto& path,const auto& bytes) {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());};
    for(auto name:{"m24_01_00_00","m21_00_00_00"}) {write(cache/(std::string(name)+".bbmap"),BbMapArt::Save(chart));write(cache/(std::string(name)+"_v1.bbmap"),BbMapArt::Save(chart));write(cache/(std::string(name)+".bbmesh"),BbMapArt::SaveMesh(mesh));}
    setenv("BB_MAP_GAME_DIR",temp,1);setenv("BB_GPU_USER_DIR",temp,1);
    for(auto map:{0x18010000u,0x15000000u}) {
        for(int n=0;n<1000 && !BbMapCharts::Get(map);++n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for(int n=0;n<1000 && !BbMapCharts::Get(map,1);++n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for(int n=0;n<1000 && !BbMapCharts::GetMesh(0x18010000u);++n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    @autoreleasepool {
        Metal::Device device;auto mtl=(__bridge id<MTLDevice>)device.NativeDevice();
        auto queue=[mtl newCommandQueue];BbSettings::Get().show_fps=false;
        BbOverlay::Init(device,unsigned(MTLPixelFormatBGRA8Unorm));
        const auto original=ImGui::GetStyle().Colors[ImGuiCol_Button];
        auto record=std::make_unique<BbCoopOverlay::SnapshotRecord>();
        record->r={BBGPU_REC_COOP_UI,sizeof(*record)};auto& s=record->view;
        s.version=BB_COOP_UI_VERSION;s.enabled=1;s.has_panel=1;s.lobby.signed_in=1;
        s.lobby.in_team=1;s.lobby.in_hunt=1;s.lobby.playing=1;s.lobby.menu=1;s.lobby.leader=1;
        s.lobby.member_count=4;s.panel.member_count=4;s.panel.revive_target=-1;
        std::snprintf(s.lobby.me,sizeof(s.lobby.me),"hunter-0");
        const char* names[]={"A very long hunter name","Viktoria","Искатель","Companion elsewhere"};
        for(int i=0;i<4;++i) {
            auto& member=s.lobby.members[i];std::snprintf(member.name,sizeof(member.name),"%s",names[i]);
            std::snprintf(member.id,sizeof(member.id),"hunter-%d",i);member.state=BBCOOP_LOBBY_PLAYING;member.ready=1;
            auto& p=s.panel.members[i];std::snprintf(p.name,sizeof(p.name),"%s",names[i]);std::snprintf(p.board,sizeof(p.board),"%s",member.id);
            p.vital=i==1?BBCOOP_VITAL_DOWN:i==2?BBCOOP_VITAL_DEAD:BBCOOP_VITAL_UP;
            p.here=i!=3;p.hp=300;p.hp_max=594;p.landed=30;p.needed=100;p.vials=12;p.bullets=9;
        }
        for(auto dimensions:{std::pair{960,720},std::pair{1152,720},std::pair{1920,1080},std::pair{3440,1440}}) {
            const auto [width,height]=dimensions;
            auto* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
            desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
            auto texture=[mtl newTextureWithDescriptor:desc];
            for(int mode=0;mode<9;++mode) {
                s.map_screen=mode>=5;s.has_map=mode>=5;s.map.map=0x18010000;s.map.has_me=1;s.map.y=10;
                s.area_count=2;s.areas[0].map=0x18010000;s.areas[1].map=0x15000000;
                std::snprintf(s.areas[0].name,64,"Yharnam");std::snprintf(s.areas[1].name,64,"Hunter's Dream");
                s.label_count=1;s.labels[0].id=19;std::snprintf(s.labels[0].name,96,"Iosefka's Clinic");
                BbSettings::Get().map_view=mode>=5?mode-5:1;
                BbSettings::Get().menu_language=mode==7?BbLanguage::MenuLanguage::De:mode==8?BbLanguage::MenuLanguage::Ja:BbLanguage::MenuLanguage::English;
                s.team_screen=mode==0;s.has_load=mode==2;s.trade_id=mode==3?42:0;
                s.panel.member_count=mode==4?1:4;
                s.panel.members[0].vital=mode==4?BBCOOP_VITAL_DOWN:BBCOOP_VITAL_UP;
                s.panel.members[0].landed=0; // a fresh downed companion must not leave a draw path
                std::snprintf(s.trade_question,sizeof(s.trade_question),"Give 10 Blood Echoes to Viktoria?");
                s.panel.vital=mode==1?BBCOOP_VITAL_DOWN:BBCOOP_VITAL_UP;s.panel.landed=45;s.panel.needed=100;
                s.load.count=4;
                for(int i=0;i<4;++i) {std::snprintf(s.load.rows[i].name,sizeof(s.load.rows[i].name),"%s",names[i]);s.load.rows[i].percent=25*(i+1);s.load.rows[i].stage=i==3?BBCOOP_LOAD_READY:BBCOOP_LOAD_LOADING;}
                BbCoopOverlay::Dispatch(&record->r);
                assert(BbCoopOverlay::CapturesInput()==(mode==0 || mode==2 || mode==3 || mode>=5));
                for(int frame=0;frame<4;++frame) {
                    // Match the renderer's unretained command buffers: artwork must stay alive.
                    auto cmdbuf=[queue commandBufferWithUnretainedReferences];
                    auto* pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
                    pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor=MTLClearColorMake(.12,.14,.16,1);
                    [[cmdbuf renderCommandEncoderWithDescriptor:pass] endEncoding];
                    BbOverlay::Render((__bridge void*)cmdbuf,(__bridge void*)texture,width,height);
                    assert(BbCoopStyle::ArtworkReady());
                    assert(ImGui::GetCurrentContext()->ErrorCountCurrentFrame==0);
                    for(auto* list:ImGui::GetDrawData()->CmdLists) assert(list->_Path.Size==0);
                    [cmdbuf commit];[cmdbuf waitUntilCompleted];assert(cmdbuf.status==MTLCommandBufferStatusCompleted);
                    const auto restored=ImGui::GetStyle().Colors[ImGuiCol_Button];assert(!std::memcmp(&original,&restored,sizeof(original)));
                }
                if(const char* folder=std::getenv("BB_COOP_UI_SHOTS")) {
                    std::filesystem::create_directories(folder);
                    auto bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width*4 bitsPerPixel:32];
                    [texture getBytes:bitmap.bitmapData bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
                    for(size_t p=0;p<size_t(width)*height*4;p+=4) std::swap(bitmap.bitmapData[p],bitmap.bitmapData[p+2]);
                    NSString* path=[NSString stringWithFormat:@"%s/%dx%d-mode%d.png",folder,width,height,mode];
                    assert([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
                }
            }
        }
        std::puts("PASS: co-op presentation, unretained Metal resources, theme isolation, input ownership and four aspect ratios, four map projections and localized text");
    }
    std::filesystem::remove_all(root);
}
