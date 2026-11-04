// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real Metal overlay and its game-command queue through SDL input.
#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "bbport_coop_ui.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "video_core/renderer_metal/mtl_device.h"

extern "C" int bbgpu_coop_next_command(BbCoopCommand*);

int main() {
    unsetenv("BB_MENU_OPEN"); unsetenv("BB_OVERLAY");
    @autoreleasepool {
        Metal::Device device;
        auto mtl=(__bridge id<MTLDevice>)device.NativeDevice();
        auto queue=[mtl newCommandQueue];
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:1152 height:720 mipmapped:NO];
        desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        auto texture=[mtl newTextureWithDescriptor:desc];
        BbSettings::Get().show_fps=false;
        BbOverlay::Init(device,unsigned(MTLPixelFormatBGRA8Unorm));
        auto record=std::make_unique<BbCoopOverlay::SnapshotRecord>();
        record->r={BBGPU_REC_COOP_UI,sizeof(*record)};
        auto& s=record->view;
        s.version=BB_COOP_UI_VERSION;s.enabled=1;s.lobby.signed_in=1;s.lobby.in_hunt=1;
        auto frame=[&] {
            BbCoopOverlay::Dispatch(&record->r);
            auto buffer=[queue commandBuffer];
            BbOverlay::Render((__bridge void*)buffer,(__bridge void*)texture,1152,720);
            [buffer commit];[buffer waitUntilCompleted];
            assert(buffer.status==MTLCommandBufferStatusCompleted);
        };
        auto drain=[](int expected) {
            BbCoopCommand c{};int count=0;
            while(bbgpu_coop_next_command(&c)) if(c.action==BB_COOP_TRAVEL_ANSWER) {
                assert(expected>=0 && c.value==expected);++count;
            }
            assert(count==(expected>=0?1:0));
        };
        auto key=[&](SDL_Keycode code, SDL_Scancode scan) {
            for(bool down:{true,false}) {
                SDL_Event e{};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
                e.key.key=code;e.key.scancode=scan;e.key.down=down;
                assert(BbOverlay::HandleEvent(e));frame();
            }
        };
        auto pad=[&](int button) {
            for(bool down:{true,false}) {
                SDL_Event e{};e.type=down?SDL_EVENT_GAMEPAD_BUTTON_DOWN:SDL_EVENT_GAMEPAD_BUTTON_UP;
                e.gbutton.button=button;e.gbutton.down=down;
                assert(BbOverlay::HandleEvent(e));frame();
            }
        };
        auto question=[&](int kind) {
            s.has_travel=0;s.travel={};frame();frame();
            s.has_travel=1;s.travel.question=kind;s.travel.point=2102962;s.travel.lamp_name_id=-1;
            frame();frame();drain(-1);
            assert(BbCoopOverlay::CapturesInput());
        };
        question(1);key(SDLK_ESCAPE,SDL_SCANCODE_ESCAPE);drain(0);
        // A stale 100 ms snapshot must not send a second answer.
        key(SDLK_RETURN,SDL_SCANCODE_RETURN);drain(-1);
        question(1);pad(SDL_GAMEPAD_BUTTON_EAST);drain(0);
        question(1);key(SDLK_RETURN,SDL_SCANCODE_RETURN);drain(1);
        question(1);pad(SDL_GAMEPAD_BUTTON_SOUTH);drain(1);
        question(1);key(SDLK_RIGHT,SDL_SCANCODE_RIGHT);key(SDLK_RETURN,SDL_SCANCODE_RETURN);drain(0);
        question(2);pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);pad(SDL_GAMEPAD_BUTTON_SOUTH);drain(0);
        question(2);key(SDLK_KP_ENTER,SDL_SCANCODE_KP_ENTER);drain(1);
        // A press already opening the lamp dialogue cannot also accept team travel.
        s.has_travel=0;s.travel={};frame();frame();
        s.has_travel=1;s.travel.question=1;s.travel.point=2102962;
        BbCoopOverlay::Dispatch(&record->r);
        SDL_Event opening{};opening.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        opening.gbutton.button=SDL_GAMEPAD_BUTTON_SOUTH;opening.gbutton.down=true;
        assert(BbOverlay::HandleEvent(opening));frame();frame();drain(-1);
        opening.type=SDL_EVENT_GAMEPAD_BUTTON_UP;opening.gbutton.down=false;
        assert(BbOverlay::HandleEvent(opening));frame();drain(-1);
        key(SDLK_SPACE,SDL_SCANCODE_SPACE);drain(1);
        s.travel={};s.travel.waiting=1;s.travel.seconds_left=10;frame();
        assert(!BbCoopOverlay::CapturesInput());drain(-1);
        s.travel={};s.travel.notice=1;frame();assert(!BbCoopOverlay::CapturesInput());drain(-1);
        s.has_travel=0;frame();assert(!BbCoopOverlay::CapturesInput());
        std::puts("PASS: Travel Together keyboard/controller confirm, cancel, selection, stale snapshots and input ownership");
    }
}
