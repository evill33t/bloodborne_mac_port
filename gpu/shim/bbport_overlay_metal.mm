// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the settings overlay's Metal side (bbport_overlay_metal.h). ARC.
#import <Metal/Metal.h>

#include "bbport_overlay_metal.h"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "video_core/renderer_metal/mtl_device.h"

#include <cstdio>
#include <array>
#include <cmath>

namespace BbOverlay::MetalBackend {

namespace {
MTLPixelFormat expected_format = MTLPixelFormatInvalid;
id<MTLDevice> overlay_device;
std::array<id<MTLTexture>,4> portraits;
std::array<u32,4> portrait_versions{};
NSMutableArray<id>* frame_portraits;
id<MTLCommandBuffer> frame_buffer;
id<MTLTexture> map_texture;
u64 map_version=0;
id<MTLRenderPipelineState> map_pipeline;
id<MTLDepthStencilState> map_depth_state;
id<MTLBuffer> map_positions,map_colors,map_indices;
id<MTLTexture> map_color,map_depth,map_tops;
u32 mesh_map=0;
std::shared_ptr<const BbMapArt::AreaMesh> current_mesh;
}

bool Init(Metal::Device& device, u32 pixel_format) {
    expected_format = MTLPixelFormat(pixel_format);
    overlay_device=(__bridge id<MTLDevice>)device.NativeDevice();
    return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device.NativeDevice());
}

void Frame(void* command_buffer, void* target, const std::function<void()>& build) {
    id<MTLCommandBuffer> cmdbuf = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)target;
    if (texture.pixelFormat != expected_format) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::printf("Overlay: target format %u, not %u (the backend builds a pipeline per format)\n",
                        unsigned(texture.pixelFormat), unsigned(expected_format));
        }
    }
    // The backend keys its pipeline on the pass's formats from NewFrame, so the descriptor is
    // made before the frame and the encoder from it after.
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    ImGui_ImplMetal_NewFrame(pass);
    frame_portraits=[NSMutableArray array];
    frame_buffer=cmdbuf;
    build();
    frame_buffer=nil;
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdLists.Size == 0) {
        if(frame_portraits.count) {
            NSArray* keepalive=[frame_portraits copy];
            [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer>) { [keepalive count]; }];
        }
        return; // nothing to draw: no pass (the target keeps its contents)
    }
    id<MTLRenderCommandEncoder> encoder = [cmdbuf renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"Overlay";
    ImGui_ImplMetal_RenderDrawData(draw_data, cmdbuf, encoder);
    [encoder endEncoding];
    if(frame_portraits.count) {
        NSArray* keepalive=[frame_portraits copy];
        [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer>) { [keepalive count]; }];
    }
}

void* PortraitTexture(unsigned slot,u32 version,const u8* rgba,u32 side) {
    if(slot>=portraits.size() || !version || !rgba || side!=128 || !overlay_device) return nullptr;
    if(!portraits[slot] || portrait_versions[slot]!=version) {
        MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:side height:side mipmapped:NO];
        desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[overlay_device newTextureWithDescriptor:desc];
        if(!texture) return nullptr;
        [texture replaceRegion:MTLRegionMake2D(0,0,side,side) mipmapLevel:0 withBytes:rgba bytesPerRow:side*4];
        portraits[slot]=texture;portrait_versions[slot]=version;
    }
    [frame_portraits addObject:portraits[slot]];
    return (__bridge void*)portraits[slot];
}

void* MapTexture(u64 version,const BbMapArt::Chart& chart,const u8* rgba) {
    if(!overlay_device || !rgba || chart.width<=0 || chart.height<=0) return nullptr;
    if(!map_texture || map_version!=version) {
        auto* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:chart.width height:chart.height mipmapped:NO];
        desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> texture=[overlay_device newTextureWithDescriptor:desc];
        if(!texture) return nullptr;
        [texture replaceRegion:MTLRegionMake2D(0,0,chart.width,chart.height) mipmapLevel:0 withBytes:rgba bytesPerRow:chart.width*4];
        map_texture=texture;map_version=version;
    }
    [frame_portraits addObject:map_texture];return (__bridge void*)map_texture;
}

void ClearMap3d() {
    // In-flight frames hold their own strong references until command-buffer completion.
    map_positions=nil;map_colors=nil;map_indices=nil;map_tops=nil;map_color=nil;map_depth=nil;
    mesh_map=0;current_mesh.reset();
}

void* Map3d(u32 map,const std::shared_ptr<const BbMapArt::AreaMesh>& mesh,
            const BbMapArt::Chart& chart,const MapCamera& camera) {
    if(!overlay_device || !frame_buffer || !map_texture || !mesh || mesh->indices.empty()) return nullptr;
    if(!map_pipeline) {
        // MSL port of lKaniel's map_3d.vert/.frag: orthographic orbit, depth, textured top faces.
        NSString* source=@R"msl(
            #include <metal_stdlib>
            using namespace metal;
            struct Camera { float4 middle,turn,frame,chart; };
            struct Out { float4 position [[position]]; float4 color; float3 world; };
            vertex Out terrain_vertex(uint id [[vertex_id]],device const packed_float3* positions [[buffer(0)]],
                device const uint* colors [[buffer(1)]],constant Camera& c [[buffer(2)]]) {
                Out o;float3 world=positions[id],d=world-c.middle.xyz;
                float u=d.x*c.turn.x-d.z*c.turn.y,v=d.x*c.turn.y+d.z*c.turn.x;
                float up=v*c.turn.z+d.y*c.turn.w,near=-v*c.turn.w+d.y*c.turn.z;
                o.position=float4(u*c.middle.w/c.frame.x,up*c.middle.w/c.frame.y,clamp(.5f-near/8000.f,0.f,1.f),1);
                o.color=float4(float(colors[id]&255),float((colors[id]>>8)&255),float((colors[id]>>16)&255),255)/255.f;
                o.world=world;return o;
            }
            fragment float4 terrain_fragment(Out in [[stage_in]],constant Camera& c [[buffer(2)]],
                texture2d<float> picture [[texture(0)]],texture2d<short> tops [[texture(1)]]) {
                float3 normal=normalize(cross(dfdx(in.world),dfdy(in.world)));
                float3 toward=float3(-c.turn.w*c.turn.y,c.turn.z,-c.turn.w*c.turn.x);
                if(dot(normal,toward)<0) normal=-normal;
                float2 uv=float2((in.world.x-c.chart.x)/c.chart.z,(c.chart.y-in.world.z)/c.chart.w);
                constexpr sampler linear_sampler(coord::normalized,address::clamp_to_edge,filter::linear);
                if(all(uv>=0.f) && all(uv<1.f)) {
                    uint2 at=min(uint2(uv*float2(tops.get_width(),tops.get_height())),uint2(tops.get_width()-1,tops.get_height()-1));
                    int top=tops.read(at).r;float4 seen=picture.sample(linear_sampler,uv);
                    if(normal.y>.55f && top!=-32768 && abs(in.world.y-float(top)/10.f)<1.5f && seen.a>.05f) return float4(seen.rgb*seen.a,1);
                    float lit=.62f+.55f*max(dot(normal,normalize(float3(-.45f,.78f,.43f))),0.f);
                    // The picture also carries exploration: dim walls in the same unvisited ground.
                    float fog=max(.2f,seen.a);
                    return float4(min(in.color.rgb*1.45f*lit,1.f)*fog,1);
                }
                return float4(in.color.rgb*.6f,1);
            }
        )msl";
        NSError* error=nil;
        id<MTLLibrary> library=[overlay_device newLibraryWithSource:source options:nil error:&error];
        if(!library) {std::fprintf(stderr,"Map3d: %s\n",error.localizedDescription.UTF8String);return nullptr;}
        MTLRenderPipelineDescriptor* desc=[MTLRenderPipelineDescriptor new];
        desc.vertexFunction=[library newFunctionWithName:@"terrain_vertex"];
        desc.fragmentFunction=[library newFunctionWithName:@"terrain_fragment"];
        desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        desc.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        map_pipeline=[overlay_device newRenderPipelineStateWithDescriptor:desc error:&error];
        if(!map_pipeline) {std::fprintf(stderr,"Map3d: %s\n",error.localizedDescription.UTF8String);return nullptr;}
        MTLDepthStencilDescriptor* depth=[MTLDepthStencilDescriptor new];
        depth.depthCompareFunction=MTLCompareFunctionLessEqual;depth.depthWriteEnabled=YES;
        map_depth_state=[overlay_device newDepthStencilStateWithDescriptor:depth];
    }
    if(mesh_map!=map || current_mesh!=mesh) {
        map_positions=[overlay_device newBufferWithBytes:mesh->positions.data() length:mesh->positions.size()*4 options:MTLResourceStorageModeShared];
        map_colors=[overlay_device newBufferWithBytes:mesh->colors.data() length:mesh->colors.size()*4 options:MTLResourceStorageModeShared];
        map_indices=[overlay_device newBufferWithBytes:mesh->indices.data() length:mesh->indices.size()*4 options:MTLResourceStorageModeShared];
        const auto& s=chart.storeys;
        const int w=std::max(1,s.width),h=std::max(1,s.height);
        auto* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Sint width:w height:h mipmapped:NO];
        desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
        map_tops=[overlay_device newTextureWithDescriptor:desc];
        const int16_t missing=BbMapArt::Chart::NoHeight;
        [map_tops replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:s.roof.empty()?&missing:s.roof.data() bytesPerRow:w*2];
        mesh_map=map;current_mesh=mesh;
    }
    const auto width=NSUInteger(std::clamp(camera.width,1.f,4096.f)),height=NSUInteger(std::clamp(camera.height,1.f,4096.f));
    if(!map_color || map_color.width!=width || map_color.height!=height) {
        auto* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        map_color=[overlay_device newTextureWithDescriptor:desc];
        desc.pixelFormat=MTLPixelFormatDepth32Float;desc.usage=MTLTextureUsageRenderTarget;
        map_depth=[overlay_device newTextureWithDescriptor:desc];
    }
    if(!map_positions || !map_colors || !map_indices || !map_tops || !map_color || !map_depth) return nullptr;
    MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=map_color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(.07,.09,.11,1);
    pass.depthAttachment.texture=map_depth;pass.depthAttachment.loadAction=MTLLoadActionClear;
    pass.depthAttachment.storeAction=MTLStoreActionDontCare;pass.depthAttachment.clearDepth=1;
    auto encoder=[frame_buffer renderCommandEncoderWithDescriptor:pass];encoder.label=@"Multiplayer terrain map";
    [encoder setRenderPipelineState:map_pipeline];[encoder setDepthStencilState:map_depth_state];[encoder setCullMode:MTLCullModeNone];
    const float constants[16]={camera.cx,camera.cy,camera.cz,camera.zoom,std::cos(camera.yaw),std::sin(camera.yaw),std::sin(camera.pitch),std::cos(camera.pitch),
        width*.5f,height*.5f,0,0,chart.x0,chart.z1,chart.width/chart.ppm,chart.height/chart.ppm};
    [encoder setVertexBuffer:map_positions offset:0 atIndex:0];[encoder setVertexBuffer:map_colors offset:0 atIndex:1];
    [encoder setVertexBytes:constants length:sizeof(constants) atIndex:2];[encoder setFragmentBytes:constants length:sizeof(constants) atIndex:2];
    [encoder setFragmentTexture:map_texture atIndex:0];[encoder setFragmentTexture:map_tops atIndex:1];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:mesh->indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:map_indices indexBufferOffset:0];
    [encoder endEncoding];
    for(id resource in @[map_positions,map_colors,map_indices,map_color,map_depth,map_tops,map_texture,map_pipeline,map_depth_state]) [frame_portraits addObject:resource];
    return (__bridge void*)map_color;
}

} // namespace BbOverlay::MetalBackend
