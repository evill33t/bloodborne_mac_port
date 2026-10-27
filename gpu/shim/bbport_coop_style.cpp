// SPDX-License-Identifier: GPL-2.0-or-later
// Visual adaptation of lKaniel's MenuArt/CoopArt/Party UI (bloodborne-server 8d5e54f),
// using its own GPL co-op artwork and read-only textures/fonts from the player's game.
#include "bbport_coop_style.h"
#include "bbport_game_art.h"
#include "bbport_settings.h"
#include "bbport_key_guide.h"
#include <map>
#include "bbport_overlay_metal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

asm(".section __TEXT,__const\n.balign 16\n.private_extern _bb_coop_art\n.globl _bb_coop_art\n_bb_coop_art:\n"
    ".incbin \"" BB_COOP_ART_PATH "\"\n.globl _bb_coop_art_end\n.private_extern _bb_coop_art_end\n_bb_coop_art_end:\n"
    ".balign 16\n.private_extern _bb_party_icons\n.globl _bb_party_icons\n_bb_party_icons:\n"
    ".incbin \"" BB_PARTY_ICONS_PATH "\"\n.globl _bb_party_icons_end\n.private_extern _bb_party_icons_end\n_bb_party_icons_end:\n"
    ".balign 8\n.private_extern _bb_coop_art_size\n.globl _bb_coop_art_size\n_bb_coop_art_size:\n.quad _bb_coop_art_end - _bb_coop_art\n"
    ".private_extern _bb_party_icons_size\n.globl _bb_party_icons_size\n_bb_party_icons_size:\n.quad _bb_party_icons_end - _bb_party_icons\n.text\n");
extern "C" const unsigned char bb_coop_art[],bb_coop_art_end[],bb_party_icons[],bb_party_icons_end[];
// Assembly computes lengths before the Mach-O linker reorders atoms under LTO.
extern "C" const uint64_t bb_coop_art_size,bb_party_icons_size,bb_font_ttf_size;
extern "C" const unsigned char bb_font_ttf[],bb_font_ttf_end[];

namespace BbCoopStyle {
namespace {
std::optional<BbCoopArt::Atlas> atlas;
std::optional<BbPartyIcons::Atlas> icons;
std::optional<BbGameArt::Image> wall,row;
std::optional<BbGameArt::GaugeArt> gauge;
ImFont* font=nullptr;
bool prepared=false,controller=false;
std::map<std::string,BbGameArt::Image> keys;
ImFont* language_fonts[BbLanguage::MenuLanguageCount]{};
bool language_tried[BbLanguage::MenuLanguageCount]{};
std::vector<uint8_t> Read(const char* relative);
void LanguageFont() {
    const auto language=BbSettings::Get().menu_language.load();const int index=int(language);
    if(language_tried[index]) {if(language_fonts[index]) font=language_fonts[index];return;}
    language_tried[index]=true;
    auto& io=ImGui::GetIO();ImFont* made=nullptr;
    for(const auto& folder:{std::string(BbLanguage::GameFolder(language)),std::string("engus")}) {
        auto ttf=BbGameArt::TtfFromGfxFont(Read(("menu/"+folder+"/font.gfx").c_str()));
        if(!ttf) continue;
        void* data=IM_ALLOC(ttf->size());std::memcpy(data,ttf->data(),ttf->size());
        ImFontConfig config;config.MergeMode=made!=nullptr;config.DstFont=made;
        auto* added=io.Fonts->AddFontFromMemoryTTF(data,ttf->size(),17,&config);if(!made) made=added;
        if(folder=="engus") break;
    }
    if(!made) made=io.Fonts->AddFontDefault();
    ImFontConfig merge;merge.MergeMode=true;merge.DstFont=made;merge.FontDataOwnedByAtlas=false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),bb_font_ttf_size,17,&merge);
    // macOS CJK fallback replaces the Windows/Linux system-font search in MenuArt::LoadLanguage.
    const char* path=language==BbLanguage::MenuLanguage::Ja?"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc":language==BbLanguage::MenuLanguage::Ko?"/System/Library/Fonts/AppleSDGothicNeo.ttc":(language==BbLanguage::MenuLanguage::ZhCn || language==BbLanguage::MenuLanguage::ZhTw)?"/System/Library/Fonts/PingFang.ttc":nullptr;
    if(path && std::filesystem::exists(path)) {ImFontConfig c;c.MergeMode=true;c.DstFont=made;io.Fonts->AddFontFromFileTTF(path,17,&c);}
    language_fonts[index]=made;font=made;
}
std::vector<uint8_t> Read(const char* relative) {
    const char* game=std::getenv("BB_MAP_GAME_DIR");if(!game) game=std::getenv("BB_GAME_DIR");
    if(!game) return {};
    std::ifstream file(std::filesystem::path(game)/"dvdroot_ps4"/relative,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
ImTextureRef Ref(void* texture) {return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)));}
ImU32 Tint(float a) {return IM_COL32(255,255,255,int(255*std::clamp(a,0.f,1.f)));}
void Part(ImDrawList* draw,BbCoopArt::Piece piece,ImVec2 at,ImVec2 end,BbCoopArt::Share part,float alpha) {
    if(!atlas) return;
    void* texture=BbOverlay::MetalBackend::ArtworkTexture(0,atlas->rgba.data(),atlas->width,atlas->height);
    if(!texture) return;
    const auto& r=atlas->rects[size_t(piece)];
    draw->AddImage(Ref(texture),at,end,ImVec2((r.x+r.width*part.x0)/atlas->width,(r.y+r.height*part.y0)/atlas->height),
        ImVec2((r.x+r.width*part.x1)/atlas->width,(r.y+r.height*part.y1)/atlas->height),Tint(alpha));
}
}
bool ArtworkReady() {return atlas.has_value() && icons.has_value() && bb_font_ttf_size>100000 && bb_font_ttf_size<2000000;}
float Scale() {return ImGui::GetFontSize()/17.f;}
ImU32 Ink(float alpha) {return IM_COL32(220,220,212,int(255*std::clamp(alpha,0.f,1.f)));}
ImU32 Muted(float alpha) {return IM_COL32(163,166,164,int(255*std::clamp(alpha,0.f,1.f)));}
void Prepare() {
    if(prepared) {LanguageFont();return;}
    prepared=true;
    atlas=BbCoopArt::Decode(bb_coop_art,bb_coop_art_size);
    if(atlas) BbCoopArt::Greyed(*atlas,BbCoopArt::SoulsGrey);
    icons=BbPartyIcons::Decode(bb_party_icons,bb_party_icons_size);
    auto common=Read("menu/common.tpf.dcx");
    if(auto tpf=BbGameArt::TpfFile::Read(common)) {
        wall=tpf->Piece("MENU_Common_00001",{0,0,1920,1080});
        row=tpf->Piece("MENU_Common_00001",{988,1532,1562,1580});
        for(const char* name:{"LS","RS","Cross","Circle","Square","Triangle","L1","R1","L2","R2","L3","R3","Options","LeftStick","RightStick","Dpad"})
            if(auto image=tpf->Piece(std::string(BbGameArt::KeyGuideTextureNamed(name)),{})) keys.emplace(name,std::move(*image));
    }
    gauge=BbGameArt::LoadGaugeArt(common);
    auto& io=ImGui::GetIO();
    if(auto ttf=BbGameArt::TtfFromGfxFont(Read("menu/engus/font.gfx"))) {
        void* data=IM_ALLOC(ttf->size());std::memcpy(data,ttf->data(),ttf->size());
        ImFontConfig config;config.FontDataOwnedByAtlas=true;
        std::snprintf(config.Name,sizeof(config.Name),"Bloodborne menus");
        font=io.Fonts->AddFontFromMemoryTTF(data,ttf->size(),17,&config);
    }
    if(!font) for(const char* path:{"/System/Library/Fonts/Supplemental/Georgia.ttf","/Library/Fonts/Georgia.ttf"}) {
        if(!std::filesystem::exists(path)) continue;
        font=io.Fonts->AddFontFromFileTTF(path,17);if(font) break;
    }
    if(font) {
        ImFontConfig merge;merge.MergeMode=true;merge.FontDataOwnedByAtlas=false;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),bb_font_ttf_size,17,&merge);
    }
    LanguageFont();
    std::printf("Co-op UI: reference artwork %s, status icons %s, game wall %s, HP art %s, menu font %s\n",
        atlas?"ready":"fallback",icons?"ready":"fallback",wall?"ready":"fallback",gauge?"ready":"fallback",font?"ready":"fallback");
}
Scope::Scope() {
    ImGui::PushFont(font,17);
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.86f,.86f,.83f,1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,ImVec4(.56f,.58f,.57f,1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.035f,.035f,.033f,.97f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.03f,.03f,.029f,.35f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,ImVec4(.045f,.045f,.04f,.99f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(.45f,.45f,.40f,.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.12f,.12f,.11f,.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.22f,.22f,.20f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.30f,.29f,.25f,1));
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.17f,.17f,.15f,.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.32f,.31f,.26f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.40f,.38f,.30f,1));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.23f,.23f,.20f,.8f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.32f,.31f,.26f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,ImVec4(.40f,.38f,.30f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,ImVec4(.77f,.77f,.69f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg,ImVec4(.25f,.25f,.22f,1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,ImVec4(.55f,.55f,.49f,1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(.85f,.82f,.69f,1));
    ImGui::PushStyleColor(ImGuiCol_NavCursor,ImVec4(.85f,.82f,.69f,1));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,ImVec4(.56f,.08f,.07f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,1*Scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1);
}
Scope::~Scope() {ImGui::PopStyleVar(4);ImGui::PopStyleColor(21);ImGui::PopFont();}
void Art(ImDrawList* draw,BbCoopArt::Piece piece,ImVec2 at,ImVec2 end,float alpha) {Part(draw,piece,at,end,{0,0,1,1},alpha);}
void Ornament(ImDrawList* draw,float x0,float x1,float y) {
    const float width=std::min(x1-x0,420.f*Scale()),center=(x0+x1)*.5f;
    if(atlas) Art(draw,BbCoopArt::Piece::Divider,ImVec2(center-width*.5f,y),ImVec2(center+width*.5f,y+12*Scale()),.8f);
    else draw->AddLine(ImVec2(x0,y),ImVec2(x1,y),Muted(.4f));
}
void Card(ImDrawList* draw,ImVec2 at,ImVec2 end,bool selected) {
    if(!atlas) {draw->AddRectFilled(at,end,IM_COL32(20,20,19,240));draw->AddRect(at,end,Muted(.6f));return;}
    using namespace BbCoopArt;
    const auto& r=atlas->rects[size_t(Piece::Card)];
    const float k=std::min((end.x-at.x)/r.width,(end.y-at.y)/r.height),cw=CardParts::CornerX*r.width*k,ch=CardParts::CornerY*r.height*k;
    Part(draw,Piece::Card,ImVec2(at.x+cw,at.y+ch),ImVec2(end.x-cw,end.y-ch),CardParts::Inside,1);
    Part(draw,Piece::Card,ImVec2(at.x+cw,at.y),ImVec2(end.x-cw,at.y+ch),CardParts::Top,1);
    Part(draw,Piece::Card,ImVec2(at.x+cw,end.y-ch),ImVec2(end.x-cw,end.y),CardParts::Bottom,1);
    Part(draw,Piece::Card,ImVec2(at.x,at.y+ch),ImVec2(at.x+cw,end.y-ch),CardParts::Left,1);
    Part(draw,Piece::Card,ImVec2(end.x-cw,at.y+ch),ImVec2(end.x,end.y-ch),CardParts::Right,1);
    const Share corners[]={{0,0,CardParts::CornerX,CardParts::CornerY},{1-CardParts::CornerX,0,1,CardParts::CornerY},
        {0,1-CardParts::CornerY,CardParts::CornerX,1},{1-CardParts::CornerX,1-CardParts::CornerY,1,1}};
    const ImVec2 places[]={at,ImVec2(end.x-cw,at.y),ImVec2(at.x,end.y-ch),ImVec2(end.x-cw,end.y-ch)};
    for(int i=0;i<4;++i) Part(draw,Piece::Card,places[i],ImVec2(places[i].x+cw,places[i].y+ch),corners[i],1);
    if(selected) draw->AddRect(ImVec2(at.x+4*Scale(),at.y+4*Scale()),ImVec2(end.x-4*Scale(),end.y-4*Scale()),Ink(.5f),0,0,Scale());
}
void Icon(ImDrawList* draw,BbPartyIcons::Icon icon,ImVec2 at,float side,float alpha) {
    void* texture=icons?BbOverlay::MetalBackend::ArtworkTexture(1,icons->rgba.data(),icons->width,icons->height):nullptr;
    if(texture) draw->AddImage(Ref(texture),at,ImVec2(at.x+side,at.y+side),ImVec2(BbPartyIcons::U0(icon),0),ImVec2(BbPartyIcons::U1(icon),1),Tint(alpha));
    else draw->AddCircle(ImVec2(at.x+side*.5f,at.y+side*.5f),side*.3f,Muted(alpha));
}
void Label(ImDrawList* draw,ImVec2 at,float size,ImU32 color,const char* text,float align,float max_width) {
    text=Text(text);
    ImFont* face=font?font:ImGui::GetFont();
    float width=face->CalcTextSizeA(size,FLT_MAX,0,text).x;
    if(max_width>0 && width>max_width) {size*=max_width/width;width=max_width;}
    at.x-=width*align;
    draw->AddText(face,size,ImVec2(at.x+Scale(),at.y+Scale()),IM_COL32(0,0,0,(color>>24)*.75f),text);
    draw->AddText(face,size,at,color,text);
}
void Face(ImDrawList* draw,const BbCoopUi& s,const char* board,const char* name,ImVec2 center,float side,int place,bool ready,float alpha) {
    ImVec2 at(center.x-side*.5f,center.y-side*.5f),end(center.x+side*.5f,center.y+side*.5f);
    draw->AddCircleFilled(center,side*.43f,IM_COL32(9,9,8,int(245*alpha)));
    void* texture=nullptr;
    for(int i=0;i<std::clamp(s.portrait_count,0,4);++i) if(board && *board && !std::strcmp(s.portraits[i].board,board)) {
        texture=BbOverlay::MetalBackend::PortraitTexture(i,s.portraits[i].id,s.portraits[i].rgba,128);break;
    }
    if(texture) draw->AddImageRounded(Ref(texture),ImVec2(at.x+side*.11f,at.y+side*.11f),ImVec2(end.x-side*.11f,end.y-side*.11f),ImVec2(0,0),ImVec2(1,1),Tint(alpha),side*.4f);
    else if(atlas) Art(draw,BbCoopArt::Piece(int(BbCoopArt::Piece::Sigil0)+(place%4)),ImVec2(at.x+side*.2f,at.y+side*.2f),ImVec2(end.x-side*.2f,end.y-side*.2f),alpha*.9f);
    else if(name && *name) {
        // One complete UTF-8 initial, so a non-Latin hunter never produces a broken glyph.
        char initial[5]{};unsigned char c=name[0];int bytes=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        std::snprintf(initial,std::min(bytes+1,5),"%s",name);Label(draw,ImVec2(center.x,center.y-side*.2f),side*.4f,Ink(alpha),initial,.5f);
    }
    if(atlas) Art(draw,ready?BbCoopArt::Piece::MedallionReady:BbCoopArt::Piece::Medallion,at,end,alpha);
    else draw->AddCircle(center,side*.44f,Ink(alpha*.7f),32,2*Scale());
}
void HpBar(ImDrawList* draw,ImVec2 at,float width,float height,float fill,float alpha) {
    fill=std::clamp(fill,0.f,1.f);
    void* texture=gauge?BbOverlay::MetalBackend::ArtworkTexture(4,gauge->rgba.data(),gauge->width,gauge->height):nullptr;
    if(texture) {
        const auto part=[&](BbGameArt::PixelRect r,ImVec2 p0,ImVec2 p1) {draw->AddImage(Ref(texture),p0,p1,ImVec2(float(r.x0)/gauge->width,float(r.y0)/gauge->height),ImVec2(float(r.x1)/gauge->width,float(r.y1)/gauge->height),Tint(alpha));};
        // Keep the original caps while stretching the gauge's middle.
        auto r=BbGameArt::GaugeFrame;const float cap=std::min(width*.1f,height*.5f);
        part({r.x0,r.y0,r.x0+8,r.y1},at,ImVec2(at.x+cap,at.y+height));
        part({r.x0+8,r.y0,r.x1-8,r.y1},ImVec2(at.x+cap,at.y),ImVec2(at.x+width-cap,at.y+height));
        part({r.x1-8,r.y0,r.x1,r.y1},ImVec2(at.x+width-cap,at.y),ImVec2(at.x+width,at.y+height));
        r=BbGameArt::GaugeFill;r.x1=r.x0+int((r.x1-r.x0)*fill);
        if(fill>0) part(r,ImVec2(at.x+cap,at.y+height*.22f),ImVec2(at.x+cap+(width-2*cap)*fill,at.y+height*.78f));
    } else {
        draw->AddRectFilled(at,ImVec2(at.x+width,at.y+height),IM_COL32(30,29,28,int(220*alpha)));
        draw->AddRectFilled(at,ImVec2(at.x+width*fill,at.y+height),IM_COL32(155,29,22,int(255*alpha)));
        draw->AddRect(at,ImVec2(at.x+width,at.y+height),Muted(alpha*.5f));
    }
}
void Supplies(ImDrawList* draw,float right,float y,uint32_t vials,uint32_t bullets,float scale) {
    char text[24];std::snprintf(text,sizeof(text),"%u",bullets);
    Label(draw,ImVec2(right,y),15*scale,Ink(),text,1);right-=25*scale;
    draw->AddRectFilled(ImVec2(right-4*scale,y+3*scale),ImVec2(right,y+14*scale),Ink(.8f),scale);right-=18*scale;
    std::snprintf(text,sizeof(text),"%u",vials);Label(draw,ImVec2(right,y),15*scale,Ink(),text,1);right-=26*scale;
    draw->AddCircleFilled(ImVec2(right,y+10*scale),4*scale,IM_COL32(165,28,24,240));
    draw->AddRectFilled(ImVec2(right-1.5f*scale,y),ImVec2(right+1.5f*scale,y+8*scale),Ink(.8f));
}
void Header(const char* title,const char* subtitle) {
    const auto at=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x,scale=Scale();
    Label(ImGui::GetWindowDrawList(),ImVec2(at.x+width*.5f,at.y),28*scale,Ink(),title,.5f,width*.85f);
    Ornament(ImGui::GetWindowDrawList(),at.x,at.x+width,at.y+35*scale);
    ImGui::Dummy(ImVec2(0,56*scale));
    if(subtitle) {ImGui::TextDisabled("%s",subtitle);ImGui::Spacing();}
}
void HeroBackdrop(ImDrawList* draw,ImVec2 at,ImVec2 end) {
    u32 width=0,height=0;
    void* texture=BbOverlay::MetalBackend::HeroTexture(width,height);
    if(texture && width && height && end.x>at.x && end.y>at.y) {
        // Fill the screen/panel without stretching the original 3:2 artwork.
        const float scale=std::max((end.x-at.x)/width,(end.y-at.y)/height);
        const float u=(end.x-at.x)/(width*scale),v=(end.y-at.y)/(height*scale);
        draw->AddImage(Ref(texture),at,end,ImVec2((1-u)*.5f,(1-v)*.5f),ImVec2((1+u)*.5f,(1+v)*.5f));
    } else Art(draw,BbCoopArt::Piece::Backdrop,at,end);
    draw->AddRectFilled(at,end,IM_COL32(0,0,0,80));
}
bool Begin(const char* title,bool* open,ImGuiWindowFlags flags) {
    const std::string translated=std::string(Text(title))+"###"+title;
    if(flags&ImGuiWindowFlags_AlwaysAutoResize) {
        const auto display=ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    }
    const bool visible=ImGui::Begin(translated.c_str(),nullptr,flags|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse);
    if(!visible) return false;
    auto* draw=ImGui::GetWindowDrawList();const auto at=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
    if(wall) if(void* texture=BbOverlay::MetalBackend::ArtworkTexture(2,wall->rgba.data(),wall->width,wall->height)) {
        draw->AddImage(Ref(texture),at,ImVec2(at.x+size.x,at.y+size.y),ImVec2(0,0),ImVec2(1,1),Tint(.9f));
        draw->AddRectFilled(at,ImVec2(at.x+size.x,at.y+size.y),IM_COL32(0,0,0,100));
    }
    draw->AddRect(ImVec2(at.x+3,at.y+3),ImVec2(at.x+size.x-3,at.y+size.y-3),Muted(.5f));
    if(open) {
        const auto content=ImGui::GetCursorScreenPos();
        ImGui::SetCursorPosX(size.x-ImGui::GetFrameHeight()-ImGui::GetStyle().WindowPadding.x);
        ImGui::PushID(title);if(ImGui::SmallButton("X")) *open=false;ImGui::PopID();
        ImGui::SetCursorScreenPos(content);
    }
    Header(title);return true;
}
bool Button(const char* label,const ImVec2& size) {
    const std::string translated=std::string(Text(label))+"###"+label;
    label=translated.c_str();
    const bool clicked=ImGui::Button(label,size);
    const auto at=ImGui::GetItemRectMin(),end=ImGui::GetItemRectMax();auto* draw=ImGui::GetWindowDrawList();
    const bool selected=ImGui::IsItemHovered() || ImGui::IsItemFocused();const float alpha=ImGui::GetStyle().Alpha;
    if(row) if(void* texture=BbOverlay::MetalBackend::ArtworkTexture(3,row->rgba.data(),row->width,row->height))
        draw->AddImage(Ref(texture),at,end,ImVec2(0,0),ImVec2(1,1),Tint(alpha));
    if(selected) {draw->AddRectFilled(at,end,IM_COL32(205,202,178,int(25*alpha)));draw->AddRect(at,end,Ink(alpha*.85f),0,0,Scale());}
    std::string text=label;text=text.substr(0,text.find("##"));
    const auto extent=ImGui::CalcTextSize(text.c_str());
    Label(draw,ImVec2((at.x+end.x)*.5f,(at.y+end.y-extent.y)*.5f),ImGui::GetFontSize(),Ink(alpha),text.c_str(),.5f,end.x-at.x-8*Scale());
    return clicked;
}
const char* Text(const char* english) {return BbLanguage::Text(BbSettings::Get().menu_language,english);}
void Input(bool pad) {controller=pad;}
void Guide(std::initializer_list<Hint> hints) {
    // Kaniel's Ui::Keys and BbKeyGuide::Arrange, adapted to retained Metal textures.
    const char* setting=std::getenv("BB_ICONS_SET");
    const bool pad=setting && std::strcmp(setting,"auto")?std::strcmp(setting,"keyboard")!=0:controller;
    const float height=24*Scale(),gap=6*Scale(),entry_gap=18*Scale();
    std::vector<BbKeyGuide::Entry> entries;
    std::vector<std::vector<std::string>> words;
    std::vector<const Hint*> shown;
    for(const auto& hint:hints) {
        const char* text=pad?hint.pad:hint.keys;if(!text || !*text) continue;
        std::vector<std::string> parts;std::string list=text;if(list=="LeftStick") list="LS";if(list=="RightStick") list="RS";size_t start=0;
        while(start<list.size()) {auto end=list.find(' ',start);if(end==std::string::npos) end=list.size();parts.push_back(list.substr(start,end-start));start=end+1;}
        float width=0;for(const auto& word:parts) width+=pad?height:std::max(height,ImGui::CalcTextSize(word.c_str()).x+8*Scale());
        width+=gap*(parts.size()-1);entries.push_back({width,ImGui::CalcTextSize(Text(hint.text)).x});words.push_back(std::move(parts));shown.push_back(&hint);
    }
    auto layout=BbKeyGuide::Arrange(entries,gap,entry_gap,ImGui::GetContentRegionAvail().x);
    const auto at=ImGui::GetCursorScreenPos();auto* draw=ImGui::GetWindowDrawList();unsigned index=8;
    for(size_t i=0;i<shown.size();++i) {
        const auto place=layout.places[i];float x=at.x+place.x,y=at.y+place.line*(height+8*Scale());
        for(const auto& word:words[i]) {
            float w=pad?height:std::max(height,ImGui::CalcTextSize(word.c_str()).x+8*Scale());
            auto found=keys.find(word);
            if(pad && found!=keys.end()) {
                index=8+unsigned(std::distance(keys.begin(),found));const auto& image=found->second;
                void* texture=BbOverlay::MetalBackend::ArtworkTexture(index,image.rgba.data(),image.width,image.height);
                if(texture) draw->AddImage(Ref(texture),ImVec2(x,y),ImVec2(x+w*layout.scale,y+height*layout.scale));
            } else {draw->AddRect(ImVec2(x,y),ImVec2(x+w*layout.scale,y+height*layout.scale),Muted(.8f),3*Scale());Label(draw,ImVec2(x+w*layout.scale*.5f,y+4*Scale()),13*Scale()*layout.scale,Ink(),word.c_str(),.5f);}
            x+=(w+gap)*layout.scale;
        }
        Label(draw,ImVec2(x,y+4*Scale()),15*Scale()*layout.scale,Muted(),Text(shown[i]->text));
    }
    ImGui::Dummy(ImVec2(0,layout.lines*(height+8*Scale())));
}

}
