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

namespace BbOverlay::MetalBackend {

namespace {
MTLPixelFormat expected_format = MTLPixelFormatInvalid;
id<MTLDevice> overlay_device;
std::array<id<MTLTexture>,4> portraits;
std::array<u32,4> portrait_versions{};
NSMutableArray<id<MTLTexture>>* frame_portraits;
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
    build();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdLists.Size == 0) {
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

} // namespace BbOverlay::MetalBackend
