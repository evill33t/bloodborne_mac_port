// SPDX-License-Identifier: GPL-2.0-or-later
// Mac adaptation of lKaniel's co-op presentation at bloodborne-server 8d5e54f.
#pragma once
#include "bbport_coop_art.h"
#include "bbport_party_icons.h"
#include "gpu/bbseamless/ui.h"
#include <imgui.h>
#include <initializer_list>
namespace BbCoopStyle {
// Called under the overlay lock before NewFrame: fonts must never be added during a frame.
void Prepare();
bool ArtworkReady();
const char* Text(const char* english);
struct Hint {const char* pad;const char* keys;const char* text;};
void Guide(std::initializer_list<Hint> hints);
void Input(bool controller);
struct Scope { Scope();~Scope(); };
float Scale();
ImU32 Ink(float alpha=1);
ImU32 Muted(float alpha=1);
void Art(ImDrawList* draw,BbCoopArt::Piece piece,ImVec2 at,ImVec2 end,float alpha=1);
void Ornament(ImDrawList* draw,float x0,float x1,float y);
void Card(ImDrawList* draw,ImVec2 at,ImVec2 end,bool selected=false);
void Icon(ImDrawList* draw,BbPartyIcons::Icon icon,ImVec2 at,float side,float alpha=1);
void Face(ImDrawList* draw,const BbCoopUi& s,const char* board,const char* name,
          ImVec2 center,float side,int place=0,bool ready=false,float alpha=1);
void HpBar(ImDrawList* draw,ImVec2 at,float width,float height,float fill,float alpha=1);
void Supplies(ImDrawList* draw,float right,float y,uint32_t vials,uint32_t bullets,float scale);
void Label(ImDrawList* draw,ImVec2 at,float size,ImU32 color,const char* text,float align=0,float max_width=0);
void HeroBackdrop(ImDrawList* draw,ImVec2 at,ImVec2 end);
bool Begin(const char* title,bool* open=nullptr,ImGuiWindowFlags flags=0);
void Header(const char* title,const char* subtitle=nullptr);
bool Button(const char* label,const ImVec2& size=ImVec2(0,0));
}
