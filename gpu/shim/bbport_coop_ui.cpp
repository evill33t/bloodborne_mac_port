// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_coop_ui.h"
#include "bbport_split.h"
#include "bbport_map_charts.h"
#include "bbport_map_view.h"
#include "bbport_settings.h"
#include "bbport_map_fog.h"
#include "bbport_overlay_metal.h"
#include "bbport_coop_style.h"
#include "bbport_party_layout.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
namespace BbCoopOverlay {
namespace {
constexpr float Pi=3.14159265358979323846f;
std::mutex snapshot_mutex;
BbCoopUi latest{};
std::atomic<bool> enabled{},capture{},visible{},trade_capture{};
std::atomic<bool> want_portrait{};
bool trade_open=false;
void Send(uint32_t action,int value=0,const char* text="",uint64_t id=0) {
    CommandRecord record{{BBGPU_EV_COOP_COMMAND,sizeof(record)}, {}};
    record.command.version=BB_COOP_UI_VERSION;record.command.action=action;
    record.command.value=value;record.command.id=id;
    std::snprintf(record.command.text,sizeof(record.command.text),"%s",text);
    if(BbSplit::Host()) BbSplit::Post(&record);else Receive(&record.r);
}
void Send(const BbCoopCommand& command) {
    const CommandRecord record{{BBGPU_EV_COOP_COMMAND,sizeof(record)},command};
    if(BbSplit::Host()) BbSplit::Post(&record);else Receive(&record.r);
}
void Lobby(int action,const char* id="") { Send(BB_COOP_LOBBY,action,id); }
const char* Hunter(const char* name) { return *name?name:"Hunter"; }
const char* Vital(int state) {
    static const char* names[]={"Away","Standing","Downed","Dead","Spectating"};
    return names[std::clamp(state,0,4)];
}
void CloseTeam() { Send(BB_COOP_TEAM_MENU,0); }
void HunterCards(const BbCoopUi& s) {
    const auto& lobby=s.lobby;const float scale=BbCoopStyle::Scale();
    const int columns=ImGui::GetContentRegionAvail().x>760*scale?4:2;
    if(!ImGui::BeginTable("hunter cards",columns,ImGuiTableFlags_SizingStretchSame)) return;
    for(int i=0;i<4;++i) {
        ImGui::TableNextColumn();ImGui::PushID(i);
        auto* draw=ImGui::GetWindowDrawList();const auto at=ImGui::GetCursorScreenPos();
        const float width=ImGui::GetContentRegionAvail().x,height=224*scale;
        const ImVec2 end(at.x+width,at.y+height),center(at.x+width*.5f,at.y+72*scale);
        const auto* member=i<std::clamp(lobby.member_count,0,4)?&lobby.members[i]:nullptr;
        const bool mine=member && !std::strcmp(member->id,lobby.me);
        const bool ready=member && (member->ready || member->state==BBCOOP_LOBBY_PLAYING);
        BbCoopStyle::Card(draw,at,end,mine);
        BbCoopStyle::Face(draw,s,member?member->id:"",member?Hunter(member->name):"",center,98*scale,i,ready,member?1.f:.35f);
        if(member && i==0) BbCoopStyle::Art(draw,BbCoopArt::Piece::MarkLeader,ImVec2(at.x+18*scale,at.y+15*scale),ImVec2(at.x+49*scale,at.y+46*scale));
        if(ready) BbCoopStyle::Art(draw,BbCoopArt::Piece::MarkReady,ImVec2(end.x-49*scale,at.y+15*scale),ImVec2(end.x-18*scale,at.y+46*scale));
        BbCoopStyle::Label(draw,ImVec2(center.x,at.y+132*scale),23*scale,member?BbCoopStyle::Ink():BbCoopStyle::Muted(.7f),member?Hunter(member->name):"An empty place",.5f,width-24*scale);
        const char* state=!member?"Invite a hunter below":member->state==BBCOOP_LOBBY_OFFLINE?"Away":member->state==BBCOOP_LOBBY_PLAYING?"In the hunt":member->ready?"Ready":"Choosing a character";
        BbCoopStyle::Label(draw,ImVec2(center.x,at.y+166*scale),16*scale,BbCoopStyle::Muted(),state,.5f,width-24*scale);
        const char* role=member?(mine?"Your hunter":i==0?"Leader of the hunt":"Companion"):"";
        BbCoopStyle::Label(draw,ImVec2(center.x,at.y+192*scale),14*scale,BbCoopStyle::Muted(.75f),role,.5f,width-24*scale);
        ImGui::Dummy(ImVec2(width,height));
        if(member && lobby.leader && i>0) {
            const auto next=ImGui::GetCursorScreenPos();
            const float button_width=ImGui::CalcTextSize("Remove").x+ImGui::GetStyle().FramePadding.x*2;
            ImGui::SetCursorScreenPos(ImVec2(end.x-button_width-14*scale,end.y-27*scale));
            if(ImGui::SmallButton("Remove")) Lobby(BBCOOP_TEAM_REMOVE,member->id);
            ImGui::SetCursorScreenPos(next);ImGui::Dummy(ImVec2(0,0));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();ImGui::Spacing();
}
void Team(const BbCoopUi& s) {
    const auto& lobby=s.lobby;
    const auto display=ImGui::GetIO().DisplaySize;const float scale=BbCoopStyle::Scale();
    ImGui::SetNextWindowSize(ImVec2(std::min(1440*scale,display.x-60*scale),std::min(940*scale,display.y-50*scale)),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    bool open=true;
    if(BbCoopStyle::Begin("Co-op Team",lobby.menu?&open:nullptr)) {
        ImGui::TextUnformatted(lobby.signed_in?"Connected":"Connecting to the co-op server...");
        if(*lobby.notice) ImGui::TextWrapped("%s",lobby.notice);
        if(!lobby.signed_in) ImGui::TextWrapped("Check the server address and connection if this persists. You can return to offline play from the launcher.");
        if(lobby.in_team) {
            HunterCards(s);
            if(*lobby.code) { ImGui::Text("Team code: %s",lobby.code);ImGui::SameLine();if(ImGui::SmallButton("Copy")) ImGui::SetClipboardText(lobby.code); }
            if(lobby.leader && ImGui::SmallButton("New team code")) Lobby(BBCOOP_TEAM_NEW_CODE);
            ImGui::SeparatorText("The hunt");
            const char* world=lobby.world>=0 && lobby.world<lobby.world_count?lobby.worlds[lobby.world].name:"New world";
            ImGui::BeginDisabled(!lobby.leader || lobby.started || lobby.menu);
            if(ImGui::BeginCombo("World",world)) {
                if(ImGui::Selectable("New world",lobby.world<0)) Lobby(BBCOOP_TEAM_WORLD);
                for(int i=0;i<std::clamp<int>(lobby.world_count,0,BBCOOP_LOBBY_WORLDS);++i) {
                    const auto& row=lobby.worlds[i];ImGui::PushID(i);ImGui::BeginDisabled(row.busy);
                    if(ImGui::Selectable(row.name,lobby.world==i)) Lobby(BBCOOP_TEAM_WORLD,row.id);
                    ImGui::EndDisabled();if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n%d lamps\n%s",row.created,row.when,row.lamps,row.characters);ImGui::PopID();
                } ImGui::EndCombo();
            }
            int death=lobby.death_mode;
            if(ImGui::Combo("On death",&death,"Down and revive\0Spectate teammates\0")) Lobby(BBCOOP_TEAM_DEATH_MODE,death?"1":"0");
            ImGui::EndDisabled();ImGui::BeginDisabled(!lobby.leader);
            bool invaders=lobby.allow_invaders!=0;
            if(ImGui::Checkbox("Allow invasions",&invaders)) Send(BB_COOP_ALLOW_INVADERS,invaders);
            ImGui::EndDisabled();
            ImGui::TextWrapped("Host rules: Rot %s, travel together %s, travel vote %s, enemy scaling %s",lobby.rule_rot?"on":"off",lobby.rule_travel?"on":"off",lobby.rule_vote?"on":"off",lobby.rule_scale?"on":"off");
            if(!lobby.menu && lobby.leader && !lobby.started) {
                if(BbCoopStyle::Button("Start hunt")) Send(BB_COOP_CHOOSE,BBCOOP_TEAM_GO_START);
                ImGui::SetItemDefaultFocus();
            }
            if(lobby.playing && !lobby.in_hunt) {
                if(BbCoopStyle::Button("Join the hunt")) Send(BB_COOP_JOIN_HUNT);
                ImGui::SetItemDefaultFocus();
            }
            if(BbCoopStyle::Button("Leave team")) Lobby(BBCOOP_TEAM_LEAVE);
            if(lobby.leader && !lobby.started && lobby.world>=0 && lobby.world<lobby.world_count) {
                static char forget[40]{};
                if(BbCoopStyle::Button("Forget selected world...")) { std::snprintf(forget,sizeof(forget),"%s",lobby.worlds[lobby.world].id);ImGui::OpenPopup("Forget world?"); }
                if(ImGui::BeginPopupModal("Forget world?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted("Remove this world from your saved-world list?");
                    if(BbCoopStyle::Button("Forget")) { Lobby(BBCOOP_TEAM_FORGET_WORLD,forget);ImGui::CloseCurrentPopup(); }
                    ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
                }
            }
        } else {
            HunterCards(s);ImGui::SeparatorText("Gather your hunters");
            ImGui::BeginDisabled(!lobby.signed_in);
            if(BbCoopStyle::Button("Create team")) Lobby(BBCOOP_TEAM_CREATE);
            ImGui::SetItemDefaultFocus();
            if(!lobby.menu) {ImGui::SameLine();if(BbCoopStyle::Button("Play alone")) Send(BB_COOP_CHOOSE,BBCOOP_TEAM_GO_ALONE);}
            static char code[32]{};ImGui::InputText("Team code",code,sizeof(code));ImGui::SameLine();if(BbCoopStyle::Button("Join")) Lobby(BBCOOP_TEAM_JOIN_CODE,code);
            if(lobby.playing) {
                const bool invading=s.has_invasion && s.invasion.role==BBCOOP_INVASION_INVADER && s.invasion.phase!=BBCOOP_INVASION_ENDED;
                if(BbCoopStyle::Button(invading?"Leave invasion":"Search for an invasion")) Send(BB_COOP_INVASION,!invading);
            }
            ImGui::EndDisabled();
        }
        if(lobby.invite_count || lobby.in_team) ImGui::SeparatorText("Invitations");
        for(int i=0;i<std::clamp<int>(lobby.invite_count,0,BBCOOP_LOBBY_INVITES);++i) {
            const auto& invite=lobby.invites[i];ImGui::PushID(100+i);ImGui::Separator();ImGui::Text("Invitation from %s",Hunter(invite.name));
            if(BbCoopStyle::Button("Accept")) Lobby(BBCOOP_TEAM_ACCEPT,invite.id);ImGui::SameLine();if(BbCoopStyle::Button("Decline")) Lobby(BBCOOP_TEAM_DECLINE,invite.id);ImGui::PopID();
        }
        if(lobby.in_team) {
            static char name[40]{};ImGui::InputText("Invite by name",name,sizeof(name));ImGui::SameLine();if(BbCoopStyle::Button("Invite")) Lobby(BBCOOP_TEAM_INVITE_NAME,name);
            if(ImGui::TreeNode("Online hunters")) {
                for(int i=0;i<std::clamp<int>(lobby.player_count,0,BBCOOP_LOBBY_PLAYERS);++i) {
                    const auto& p=lobby.players[i];ImGui::PushID(200+i);ImGui::TextUnformatted(Hunter(p.name));ImGui::SameLine();if(ImGui::SmallButton("Invite")) Lobby(BBCOOP_TEAM_INVITE,p.id);ImGui::PopID();
                } ImGui::TreePop();
            }
        }
        if(lobby.playing) {
            if(BbCoopStyle::Button("Trade items")) trade_open=true;
            ImGui::SameLine();if(BbCoopStyle::Button("World map")) Send(BB_COOP_MAP_MENU,1);
            if(s.duel.may_propose && BbCoopStyle::Button("Propose team duel")) Send(BB_COOP_DUEL_PROPOSE);
            if(s.duel.may_cancel && BbCoopStyle::Button("End duel")) Send(BB_COOP_DUEL_CANCEL);
        }
        if(lobby.menu && !lobby.playing) ImGui::TextWrapped("Close this menu and select Continue or Load Game to choose your character. Your team will be ready when that character's team screen opens.");
        if(lobby.menu) {
            if(BbCoopStyle::Button(lobby.playing?"Return to game":"Return to title")) CloseTeam();
            ImGui::SetItemDefaultFocus();
        }
        ImGui::TextDisabled("F6 Team   F8 Map   G Ping   F7 Free camera");
    }
    ImGui::End();if(!open) CloseTeam();
}
void Trade(const BbCoopUi& s) {
    if(!trade_open) return;
    const float scale=BbCoopStyle::Scale();const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(700*scale,display.x-40*scale),std::min(600*scale,display.y-50*scale)),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    if(BbCoopStyle::Begin("Trade with teammate",&trade_open)) {
        if(!s.trade_on || !s.partner_count) ImGui::TextWrapped("Trading is available with a standing teammate on your current map.");
        static int partner=0,quantity=1;static uint32_t selected_handle=0,selected_kind=0,selected_item=0;
        partner=std::clamp(partner,0,std::max(0,s.partner_count-1));
        if(ImGui::BeginCombo("Give to",s.partner_count?Hunter(s.partners[partner].name):"No teammate nearby")) {
            for(int i=0;i<std::clamp(s.partner_count,0,4);++i) if(ImGui::Selectable(Hunter(s.partners[i].name),partner==i)) partner=i;
            ImGui::EndCombo();
        }
        static ImGuiTextFilter filter;filter.Draw("Find item");
        const BbCoopInventoryItem* selected=nullptr;
        if(ImGui::BeginChild("inventory",ImVec2(0,std::max(100*scale,ImGui::GetContentRegionAvail().y-ImGui::GetFrameHeightWithSpacing()*3)),ImGuiChildFlags_Borders)) {
            for(int i=0;i<std::clamp(s.inventory_count,0,BB_COOP_INVENTORY_MAX);++i) {
                const auto& row=s.inventory[i];bool chosen=row.kind==selected_kind && row.id==selected_item && row.handle==selected_handle;
                if(chosen) selected=&row;
                if(!filter.PassFilter(row.name)) continue;
                ImGui::PushID(i);char label[144];std::snprintf(label,sizeof(label),"%s (%u)",row.name,row.count);
                if(ImGui::Selectable(label,chosen)) { selected_kind=row.kind;selected_item=row.id;selected_handle=row.handle;quantity=1;selected=&row; }
                ImGui::PopID();
            }
        } ImGui::EndChild();
        ImGui::InputInt("Quantity",&quantity);
        const int maximum=selected?std::min(selected->count,selected->kind==1?999999999u:selected->kind==3?1u:99u):1;
        quantity=std::clamp(quantity,1,std::max(1,maximum));
        static BbCoopCommand pending{};static char confirmation[256]{};
        ImGui::BeginDisabled(!selected || !s.partner_count || !s.trade_on);
        if(BbCoopStyle::Button("Offer...")) {
            pending={};pending.version=BB_COOP_UI_VERSION;pending.action=BB_COOP_TRADE_OFFER;
            pending.kind=selected->kind;pending.item=selected->id;pending.handle=selected->handle;pending.count=quantity;
            std::snprintf(pending.text,sizeof(pending.text),"%s",s.partners[partner].id);
            std::snprintf(confirmation,sizeof(confirmation),"Give %d %s to %s?",quantity,selected->name,Hunter(s.partners[partner].name));
            ImGui::OpenPopup("Confirm trade");
        } ImGui::EndDisabled();
        if(ImGui::BeginPopupModal("Confirm trade",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s",confirmation);ImGui::TextUnformatted("The items are reserved until your teammate answers.");
            if(BbCoopStyle::Button("Send offer")) {Send(pending);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    } ImGui::End();
}
void Map(const BbCoopUi& s) {
    bool open=true;
    const float scale=ImGui::GetFontSize()/17;
    const ImVec2 limit(ImGui::GetIO().DisplaySize.x-40,ImGui::GetIO().DisplaySize.y-60);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(650*scale,limit.x),std::min(780*scale,limit.y)),limit);
    ImGui::SetNextWindowSize(ImVec2(std::min(920*scale,limit.x),std::min(820*scale,limit.y)),ImGuiCond_FirstUseEver);
    if(BbCoopStyle::Begin("World map",&open)) {
        if(!s.has_map) ImGui::TextUnformatted("Waiting for the game map...");
        const auto& map=s.map;
        static uint32_t shown=0;static float zoom=1.0f;
        if(!shown) shown=map.map;
        std::map<uint32_t,bool> areas;areas[map.map]=true;
        for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) areas[map.lamps[i].map]=true;
        auto area_name=[&](uint32_t map) -> std::string {
            for(int i=0;i<std::clamp(s.area_count,0,48);++i) if(s.areas[i].map==map) return s.areas[i].name;
            char text[40];std::snprintf(text,sizeof(text),"Area %08x",map);return text;
        };
        const auto title=area_name(shown);
        if(ImGui::BeginCombo("Area",title.c_str())) { for(auto [area,unused]:areas) { (void)unused;if(ImGui::Selectable(area_name(area).c_str(),shown==area)) shown=area; }ImGui::EndCombo(); }
        if(s.explored_map!=shown) {
            BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_MAP_AREA;c.map=shown;Send(c);
        }
        int view=BbSettings::Get().map_view;
        if(ImGui::Combo("View",&view,"Coordinates\0Rendered terrain\0Live 3D\0")) {BbSettings::Get().map_view=view;BbSettings::Save();}
        bool fog=BbSettings::Get().map_fog;
        if(ImGui::Checkbox("Fog of war",&fog)) {BbSettings::Get().map_fog=fog;BbSettings::Save();}
        auto chart=view?BbMapCharts::Get(shown):nullptr;
        auto mesh=view==2 && chart?BbMapCharts::GetMesh(shown):nullptr;
        const bool live=view==2 && chart && mesh;
        static bool follow=true;static float pan_x=0,pan_z=0,yaw=0,pitch=.9f;
        static uint32_t previous=0;
        if(previous!=shown) {pan_x=pan_z=0;previous=shown;follow=true;}
        ImGui::SliderFloat("Zoom",&zoom,.1f,12.f,"%.1f px/m");
        ImGui::Checkbox("Follow player",&follow);ImGui::SameLine();
        const bool fit=BbCoopStyle::Button("Fit area");
        if(view==2) {ImGui::SliderAngle("Turn",&yaw,-180,180);ImGui::SliderAngle("Tilt",&pitch,20,90);}
        const ImVec2 start=ImGui::GetCursorScreenPos(),size=ImVec2(std::max(250.f,ImGui::GetContentRegionAvail().x),std::max(100.f,ImGui::GetContentRegionAvail().y-140.f-ImGui::GetTextLineHeightWithSpacing()*7-ImGui::GetFrameHeightWithSpacing()));
        ImGui::InvisibleButton("map canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
        const bool hovered=ImGui::IsItemHovered();auto* draw=ImGui::GetWindowDrawList();
        const ImVec2 end(start.x+size.x,start.y+size.y);
        float cx=shown==map.map?map.x:0,cz=shown==map.map?map.z:0,cy=shown==map.map?map.y:0;
        if(shown!=map.map) for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) if(map.lamps[i].map==shown) {cx=map.lamps[i].x;cz=map.lamps[i].z;cy=map.lamps[i].y;break;}
        if(follow) {pan_x=cx;pan_z=cz;}
        if(fit && chart) {follow=false;pan_x=chart->x0+chart->width/chart->ppm*.5f;pan_z=chart->z1-chart->height/chart->ppm*.5f;zoom=std::min(size.x/(chart->width/chart->ppm),size.y/(chart->height/chart->ppm))*.95f;}
        BbMapView::Camera camera{pan_x,cy,pan_z,zoom,live?yaw:0,live?pitch:1.5707963268f};
        if(hovered && ImGui::GetIO().MouseWheel!=0) zoom=std::clamp(zoom*std::pow(1.15f,ImGui::GetIO().MouseWheel),.1f,12.f);
        if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const auto& delta=ImGui::GetIO().MouseDelta;auto p=camera.Ground(-delta.x,-delta.y);
            pan_x=p[0];pan_z=p[1];follow=false;camera.x=pan_x;camera.z=pan_z;
        }
        camera.zoom=zoom;
        auto at=[&](float x,float z,float y) {auto p=camera.Project(x,y,z);return ImVec2(start.x+size.x*.5f+p[0],start.y+size.y*.5f+p[1]);};
        draw->PushClipRect(start,end,true);draw->AddRectFilled(start,end,IM_COL32(18,24,28,255));
        bool cleared=false;
        for(int i=0;i<std::clamp(map.cleared_count,0,24);++i) cleared|=map.cleared[i]==shown;
        if(chart) {
            static uint64_t last=0;static std::vector<uint8_t> pixels;
            uint64_t stamp=14695981039346656037ull;
            auto mix=[&](uint64_t n){stamp^=n;stamp*=1099511628211ull;};
            const bool floor=!live && shown==map.map && BbMapArt::UnderRoof(*chart,map.x,map.y,map.z);
            mix(reinterpret_cast<uintptr_t>(chart.get()));mix(shown);mix(cleared);mix(fog);
            mix(uint64_t(int64_t(std::floor(map.x/2))));mix(uint64_t(int64_t(std::floor(map.z/2))));mix(floor?uint64_t(int64_t(std::floor(cy))):UINT64_MAX);
            if(s.explored_map==shown) for(int i=0;i<std::clamp(s.explored_count,0,8192)*2;++i) mix(uint16_t(s.explored[i]));
            for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) mix(map.lamps[i].lit);
            if(last!=stamp) {
                pixels=chart->rgba;if(floor) BbMapArt::FloorView(*chart,std::floor(cy),pixels.data());
                std::vector<BbMapArt::Vec3> reveal;
                if(s.explored_map==shown) for(int i=0;i<std::clamp(s.explored_count,0,8192);++i)
                    reveal.push_back({(s.explored[2*i]+.5f)*float(BBCOOP_MAP_CELL),(s.explored[2*i+1]+.5f)*float(BBCOOP_MAP_CELL),40});
                for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {const auto& lamp=map.lamps[i];
                    if(lamp.map==shown && lamp.lit) reveal.push_back({lamp.x,lamp.z,60});}
                if(map.has_me && shown==map.map) reveal.push_back({map.x,map.z,40});
                if(fog && !cleared) BbMapView::Fog(*chart,reveal,pixels);
                last=stamp;
            }
            void* texture=BbOverlay::MetalBackend::MapTexture(stamp,*chart,pixels.data());
            if(live) texture=BbOverlay::MetalBackend::Map3d(shown,mesh,*chart,{camera.x,camera.y,camera.z,camera.zoom,camera.yaw,camera.pitch,size.x,size.y});
            const ImTextureRef ref{static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))};
            if(texture) {
                if(live) draw->AddImage(ref,start,end);
                else draw->AddImage(ref,at(chart->x0,chart->z1,cy),at(chart->x0+chart->width/chart->ppm,chart->z1-chart->height/chart->ppm,cy));
            }
        } else {
            if(cleared) draw->AddRectFilled(start,end,IM_COL32(38,54,48,255));
            else if(s.explored_map==shown) for(int i=0;i<std::clamp(s.explored_count,0,8192);++i) {
                const float x=s.explored[2*i]*BBCOOP_MAP_CELL,z=s.explored[2*i+1]*BBCOOP_MAP_CELL;
                draw->AddRectFilled(at(x,z+float(BBCOOP_MAP_CELL),cy),at(x+float(BBCOOP_MAP_CELL),z,cy),IM_COL32(45,65,61,255));
            }
            for(float x=0;x<size.x;x+=40) draw->AddLine(ImVec2(start.x+x,start.y),ImVec2(start.x+x,end.y),IM_COL32(40,48,52,255));
            for(float y=0;y<size.y;y+=40) draw->AddLine(ImVec2(start.x,start.y+y),ImVec2(end.x,start.y+y),IM_COL32(40,48,52,255));
        }
        for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {const auto& lamp=map.lamps[i];if(lamp.map==shown) draw->AddCircleFilled(at(lamp.x,lamp.z,lamp.y),5,lamp.lit?IM_COL32(255,210,90,255):IM_COL32(100,100,100,255));}
        for(int i=0;i<std::clamp(map.member_count,0,4);++i) {const auto& m=map.members[i];if(m.map==shown && m.has_at) {auto p=at(m.x,m.z,m.y);BbCoopStyle::Face(draw,s,m.board,m.name,p,28*scale,i,m.vital==BBCOOP_VITAL_UP);draw->AddText(ImVec2(p.x+17*scale,p.y),BbCoopStyle::Ink(),Hunter(m.name));}}
        if(map.has_me && shown==map.map) draw->AddCircleFilled(at(map.x,map.z,map.y),6,IM_COL32(130,255,150,255));
        if(map.has_stain && map.stain.map==shown) draw->AddCircleFilled(at(map.stain.x,map.stain.z,map.stain.y),6,IM_COL32(230,60,70,255));
        for(int i=0;i<std::clamp(s.pings.count,0,4);++i) {const auto& ping=s.pings.pings[i];if(ping.map==shown) draw->AddCircle(at(ping.x,ping.z,ping.y),9,IM_COL32(255,240,130,255),0,2);}
        draw->PopClipRect();
        if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const auto p=ImGui::GetIO().MousePos;auto ground=camera.Ground(p.x-start.x-size.x*.5f,p.y-start.y-size.y*.5f);
            BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_MAP_PING;c.map=shown;c.x=ground[0];c.z=ground[1];c.y=cy;Send(c);
        }
        if(view && (!chart || (view==2 && !mesh))) ImGui::TextWrapped("%s",BbMapCharts::Status(shown,chart && view==2).c_str());
        ImGui::TextDisabled("Drag: pan   Scroll: zoom   Right-click: ping at your floor height");
        ImGui::TextDisabled("Green: you   Medallions: teammates   Gold: lamps   Red: bloodstain");
        ImGui::TextDisabled(!fog?"Fog of war is off":cleared?"Area cleared":"Unexplored terrain is dimmed; exploration is saved.");
        if(map.has_stain) ImGui::Text("Bloodstain: %d echoes",map.stain.echoes);
        if(ImGui::BeginChild("lamps",ImVec2(0,140))) for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {
            const auto& lamp=map.lamps[i];if(lamp.map!=shown) continue;ImGui::PushID(i);
            ImGui::Text("%s%s",s.lamp_names[i],lamp.lit?"":" (unlit)");ImGui::SameLine();
            ImGui::BeginDisabled(!lamp.lit || map.block!=BBCOOP_MAP_GO);if(ImGui::SmallButton("Travel")) Send(BB_COOP_TRAVEL,lamp.warp_point);ImGui::EndDisabled();ImGui::PopID();
        } ImGui::EndChild();
        if(map.block!=BBCOOP_MAP_GO) ImGui::TextDisabled("Travel is unavailable while loading, downed, in combat with a boss, or already travelling.");
        if(BbCoopStyle::Button("Close map")) open=false;
    } ImGui::End();if(!open) Send(BB_COOP_MAP_MENU,0);
}
void InvasionStatus(const bbcoop_invasion& invasion) {
    const bool visitor=invasion.role==BBCOOP_INVASION_INVADER;
    switch(invasion.phase) {
    case BBCOOP_INVASION_SEARCHING:
        ImGui::TextUnformatted("Searching for a hunt to invade...");break;
    case BBCOOP_INVASION_APPROACHING:
        ImGui::Text(visitor?"Invading a hunt in %d seconds":"An adversary arrives in %d seconds",invasion.seconds);break;
    case BBCOOP_INVASION_ACTIVE:
        ImGui::TextUnformatted(visitor?"You are invading a hunt":"An adversary is in your world");break;
    case BBCOOP_INVASION_RETURNING:
        ImGui::TextUnformatted("Returning to your world...");break;
    case BBCOOP_INVASION_ENDED:
        switch(invasion.reason) {
        case BBCOOP_INVASION_INVADER_DIED:ImGui::TextUnformatted("Invader defeated");break;
        case BBCOOP_INVASION_TARGET_KILLED:ImGui::TextUnformatted("The host was defeated");break;
        case BBCOOP_INVASION_PARTY_WIPED:ImGui::TextUnformatted("The hunt was defeated");break;
        case BBCOOP_INVASION_NOT_FOUND:ImGui::TextUnformatted("No hunt found for an invasion");break;
        case BBCOOP_INVASION_REFUSED:ImGui::TextUnformatted("The server refused the invasion request");break;
        default:ImGui::TextUnformatted("The invasion ended");break;
        }break;
    default:break;
    }
}
void Hud(const BbCoopUi& s) {
    if(s.team_screen || s.map_screen || s.has_load) return;
    const auto& io=ImGui::GetIO();auto* draw=ImGui::GetBackgroundDrawList();
    const auto stage=BbPartyLayout::Stage({0,0,io.DisplaySize.x,io.DisplaySize.y});
    const float scale=stage.h/1080.f,left=stage.x+48*scale;
    const float top=stage.y+stage.h*.225f;
    const int count=s.has_panel?std::clamp(s.panel.member_count,0,4):0;
    if(!s.panel.native_list) for(int i=0;i<count;++i) {
        const auto& m=s.panel.members[i];const float y=top+i*72*scale;
        const bool down=m.vital==BBCOOP_VITAL_DOWN,dead=m.vital==BBCOOP_VITAL_DEAD || m.vital==BBCOOP_VITAL_SPECTATE;
        const float alpha=m.here?1.f:.5f;
        const ImU32 shade=down?IM_COL32(85,5,9,160):IM_COL32(5,5,5,170);
        draw->AddRectFilledMultiColor(ImVec2(left+20*scale,y),ImVec2(left+400*scale,y+62*scale),shade,0,0,shade);
        BbCoopStyle::Face(draw,s,m.board,Hunter(m.name),ImVec2(left+30*scale,y+29*scale),62*scale,i,false,alpha);
        if(down || dead || !m.here) BbCoopStyle::Icon(draw,down?BbPartyIcons::Icon::Downed:dead?BbPartyIcons::Icon::Fallen:BbPartyIcons::Icon::Elsewhere,
            ImVec2(left+42*scale,y+37*scale),24*scale,alpha);
        BbCoopStyle::Label(draw,ImVec2(left+74*scale,y+2*scale),22*scale,BbCoopStyle::Ink(alpha),Hunter(m.name),0,205*scale);
        BbCoopStyle::Supplies(draw,left+382*scale,y+6*scale,m.vials,m.bullets,scale);
        if(down) {
            const float share=m.needed?std::clamp(float(m.landed)/m.needed,0.f,1.f):0;
            char text[64];std::snprintf(text,sizeof(text),"Downed  -  revive %d%%",int(share*100));
            BbCoopStyle::Label(draw,ImVec2(left+76*scale,y+32*scale),15*scale,IM_COL32(239,148,135,255),text);
            if(share>0) {
                draw->PathArcTo(ImVec2(left+30*scale,y+29*scale),28*scale,-Pi*.5f,-Pi*.5f+share*2*Pi,48);
                draw->PathStroke(IM_COL32(200,45,34,240),0,3*scale);
            }
        } else if(dead || !m.here) BbCoopStyle::Label(draw,ImVec2(left+76*scale,y+33*scale),15*scale,BbCoopStyle::Muted(alpha),dead?Vital(m.vital):"In another area");
        else BbCoopStyle::HpBar(draw,ImVec2(left+74*scale,y+34*scale),306*scale,15*scale,m.hp_max>0?float(m.hp)/m.hp_max:0,alpha);
    }
    if(s.has_panel && (s.panel.vital==BBCOOP_VITAL_DOWN || s.panel.vital==BBCOOP_VITAL_SPECTATE)) {
        const bool down=s.panel.vital==BBCOOP_VITAL_DOWN;const float center=stage.x+stage.w*.5f,y=stage.y+stage.h*.49f;
        const float share=s.panel.needed?std::clamp(float(s.panel.landed)/s.panel.needed,0.f,1.f):0;
        draw->AddRectFilled(ImVec2(center-300*scale,y-65*scale),ImVec2(center+300*scale,y+133*scale),IM_COL32(0,0,0,150));
        BbCoopStyle::Ornament(draw,center-300*scale,center+300*scale,y-65*scale);
        if(down) {
            BbCoopStyle::Icon(draw,BbPartyIcons::Icon::ReviveRing,ImVec2(center-40*scale,y-45*scale),80*scale);
            if(share>0) {draw->PathArcTo(ImVec2(center,y-5*scale),30*scale,-Pi*.5f,-Pi*.5f+share*2*Pi,48);draw->PathStroke(IM_COL32(190,38,26,255),0,5*scale);}
            char text[32];std::snprintf(text,sizeof(text),"%d%%",int(share*100));
            BbCoopStyle::Label(draw,ImVec2(center,y-16*scale),20*scale,BbCoopStyle::Ink(),text,.5f);
        }
        char title[96];std::snprintf(title,sizeof(title),down?"You are down":"Spectating %s",Hunter(s.panel.watching_name));
        BbCoopStyle::Label(draw,ImVec2(center,y+42*scale),27*scale,BbCoopStyle::Ink(),title,.5f,570*scale);
        BbCoopStyle::Label(draw,ImVec2(center,y+81*scale),17*scale,BbCoopStyle::Muted(),down?"A companion can strike you to revive you":"L1 / R1: next companion",.5f);
        BbCoopStyle::Label(draw,ImVec2(center,y+108*scale),15*scale,BbCoopStyle::Muted(),"Hold Circle / Left Shift to wake at your lamp",.5f);
        draw->AddRectFilled(ImVec2(center-90*scale,y+135*scale),ImVec2(center+90*scale,y+139*scale),IM_COL32(60,50,48,220));
        if(s.panel.give_up>0) draw->AddRectFilled(ImVec2(center-90*scale,y+135*scale),ImVec2(center-90*scale+180*scale*std::clamp(s.panel.give_up,0.f,1.f),y+139*scale),IM_COL32(180,32,26,255));
    } else if(s.has_panel && s.panel.revive_target>=0 && s.panel.revive_target<count) {
        const auto& member=s.panel.members[s.panel.revive_target];const float center=stage.x+stage.w*.5f,y=stage.y+stage.h*.74f;
        char text[96];std::snprintf(text,sizeof(text),"R1 / R2  -  strike to revive %s",Hunter(member.name));
        BbCoopStyle::Label(draw,ImVec2(center,y),22*scale,BbCoopStyle::Ink(),text,.5f,700*scale);
    }
    if(s.has_compass) {
        const float center=stage.x+stage.w*.5f,y=stage.y+35*scale,span=190*scale;
        draw->AddLine(ImVec2(center-span,y),ImVec2(center+span,y),BbCoopStyle::Muted(.45f),scale);
        for(int tick=-4;tick<=4;++tick) draw->AddLine(ImVec2(center+tick*span/4,y-3*scale),ImVec2(center+tick*span/4,y+(tick%2?3:6)*scale),BbCoopStyle::Muted(.5f),scale);
        for(int i=0;i<std::clamp(s.compass.count,0,4);++i) {
            const auto& m=s.compass.markers[i];const float x=center+std::clamp(m.x,-1.f,1.f)*span,my=y+17*scale+(i%2)*38*scale;
            BbCoopStyle::Face(draw,s,m.board,m.name,ImVec2(x,my),32*scale,m.member,m.vital==BBCOOP_VITAL_UP);
            if(m.vital==BBCOOP_VITAL_DOWN) BbCoopStyle::Icon(draw,BbPartyIcons::Icon::Downed,ImVec2(x-14*scale,my-14*scale),28*scale);
            char text[48];std::snprintf(text,sizeof(text),"%s  %.0fm",Hunter(m.name),m.distance);
            BbCoopStyle::Label(draw,ImVec2(x,my+19*scale),12*scale,BbCoopStyle::Ink(),text,.5f,135*scale);
            if(m.pinned) draw->AddTriangleFilled(ImVec2(x,y-9*scale),ImVec2(x+(m.x<0?8:-8)*scale,y-13*scale),ImVec2(x+(m.x<0?8:-8)*scale,y-5*scale),BbCoopStyle::Ink());
        }
        if(s.compass.has_stain) draw->AddCircleFilled(ImVec2(center+std::clamp(s.compass.stain.x,-1.f,1.f)*span,y),4*scale,IM_COL32(200,45,38,255));
    }
    for(int i=0;i<std::clamp(s.pings.count,0,4);++i) {
        const auto& p=s.pings.pings[i];if(!p.on_screen) continue;
        ImVec2 point(p.screen_x*io.DisplaySize.x,p.screen_y*io.DisplaySize.y);
        const float r=8*scale;const ImU32 ink=IM_COL32(218,211,170,240);
        draw->AddQuad(ImVec2(point.x,point.y-r),ImVec2(point.x+r,point.y),ImVec2(point.x,point.y+r),ImVec2(point.x-r,point.y),ink,1.5f*scale);
        char text[80];std::snprintf(text,sizeof(text),"%s  %.0fm",Hunter(p.name),p.distance);
        BbCoopStyle::Label(draw,ImVec2(point.x+12*scale,point.y-8*scale),15*scale,ink,text,0,220*scale);
    }
    if(s.duel.countdown_shown) {
        char text[32];std::snprintf(text,sizeof(text),s.duel.countdown_fight?"Fight":"%d",s.duel.countdown_number);
        BbCoopStyle::Label(draw,ImVec2(stage.x+stage.w*.5f,stage.y+stage.h*.22f),70*scale,BbCoopStyle::Ink(),text,.5f);
    }
    const bool invasion=s.has_invasion && s.invasion.phase!=0;
    if(!s.lobby.signed_in || *s.notice || s.camera_on || s.skip_want || s.has_rot || invasion) {
        ImGui::SetNextWindowPos(ImVec2(left,top+(count*72+18)*scale),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.25f);ImGui::SetNextWindowSize(ImVec2(400*scale,0));
        if(ImGui::Begin("##hunt notices",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav)) {
            if(!s.lobby.signed_in) ImGui::TextUnformatted("Connecting to the hunt...");
            if(s.has_rot) ImGui::Text("Rot %d  -  rest at a lamp to clear",s.rot.count);
            if(s.camera_on) ImGui::TextUnformatted("Free camera  -  F7 to return");
            if(s.skip_want) ImGui::Text("Skip cutscene: %d / %d votes",s.skip_want,s.skip_watching);
            if(*s.notice) ImGui::TextWrapped("%s",s.notice);
            if(invasion) InvasionStatus(s.invasion);
        } ImGui::End();
    }
    static double entered=-1;
    if(!s.lobby.in_hunt) entered=-1;
    else if(entered<0) entered=ImGui::GetTime();
    if(entered>=0 && ImGui::GetTime()-entered<20) BbCoopStyle::Label(draw,ImVec2(left,top+(count*72+100)*scale),15*scale,BbCoopStyle::Muted(.8f),"F6  Team     F8  Map     G  Ping");
}
void Loading(const BbCoopUi& s) {
    const auto display=ImGui::GetIO().DisplaySize;const float scale=BbCoopStyle::Scale();
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(display);
    if(ImGui::Begin("##hunters entering",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs)) {
        auto* draw=ImGui::GetWindowDrawList();
        BbCoopStyle::Art(draw,BbCoopArt::Piece::Backdrop,ImVec2(0,0),display);
        draw->AddRectFilled(ImVec2(0,0),display,IM_COL32(0,0,0,125));
        BbCoopStyle::Label(draw,ImVec2(display.x*.5f,display.y*.12f),38*scale,BbCoopStyle::Ink(),"Entering the hunt",.5f);
        BbCoopStyle::Ornament(draw,display.x*.2f,display.x*.8f,display.y*.12f+52*scale);
        const int count=std::clamp(s.load.count,0,4);const float gap=22*scale;
        const float width=std::min(270*scale,(display.x-80*scale-gap*(std::max(1,count)-1))/std::max(1,count));
        const float height=330*scale,left=(display.x-(width+gap)*count+gap)*.5f,top=(display.y-height)*.52f;
        for(int i=0;i<count;++i) {
            const auto& member=s.load.rows[i];const ImVec2 at(left+i*(width+gap),top),end(at.x+width,at.y+height);
            const bool ready=member.stage==BBCOOP_LOAD_READY;
            BbCoopStyle::Card(draw,at,end,member.self);
            BbCoopStyle::Face(draw,s,"",member.name,ImVec2(at.x+width*.5f,at.y+90*scale),115*scale,i,ready);
            BbCoopStyle::Label(draw,ImVec2(at.x+width*.5f,at.y+177*scale),25*scale,BbCoopStyle::Ink(),Hunter(member.name),.5f,width-22*scale);
            const char* state=ready?"Ready":member.stage==BBCOOP_LOAD_JOINING?"Joining the hunt":member.stage==BBCOOP_LOAD_LOADING?"Loading":"Waiting for hunters";
            BbCoopStyle::Label(draw,ImVec2(at.x+width*.5f,at.y+219*scale),17*scale,BbCoopStyle::Muted(),state,.5f,width-22*scale);
            const float percent=std::clamp(member.percent*.01f,0.f,1.f);
            draw->AddRectFilled(ImVec2(at.x+25*scale,at.y+260*scale),ImVec2(end.x-25*scale,at.y+264*scale),IM_COL32(60,60,56,220));
            draw->AddRectFilled(ImVec2(at.x+25*scale,at.y+260*scale),ImVec2(at.x+25*scale+(width-50*scale)*percent,at.y+264*scale),BbCoopStyle::Ink());
            char text[16];std::snprintf(text,sizeof(text),"%d%%",std::clamp(member.percent,0,100));
            BbCoopStyle::Label(draw,ImVec2(at.x+width*.5f,at.y+282*scale),18*scale,BbCoopStyle::Muted(),text,.5f);
        }
        BbCoopStyle::Label(draw,ImVec2(display.x*.5f,display.y*.9f),18*scale,BbCoopStyle::Muted(),"Your companions will join you when the hunt is ready",.5f,display.x*.9f);
    } ImGui::End();
}
void Questions(const BbCoopUi& s) {
    if(s.trade_id) {
        ImGui::SetNextWindowSize(ImVec2(520*BbCoopStyle::Scale(),0),ImGuiCond_Always);
        if(BbCoopStyle::Begin("Trade offer",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextWrapped("%s",s.trade_question);if(BbCoopStyle::Button("Accept")) Send(BB_COOP_TRADE_ANSWER,1,"",s.trade_id);ImGui::SameLine();if(BbCoopStyle::Button("Decline")) Send(BB_COOP_TRADE_ANSWER,0,"",s.trade_id);
        } ImGui::End();
    }
    if(s.duel_id) { if(BbCoopStyle::Begin("Team duel invitation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text("%s proposes a duel.",Hunter(s.duel_name));if(BbCoopStyle::Button("Accept duel")) Send(BB_COOP_DUEL_ANSWER,1,"",s.duel_id);ImGui::SameLine();if(BbCoopStyle::Button("Decline duel")) Send(BB_COOP_DUEL_ANSWER,0,"",s.duel_id);}ImGui::End(); }
    if(s.has_travel && (s.travel.question || s.travel.waiting)) {if(BbCoopStyle::Begin("Travel with team",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text("%s%s",s.travel.from,s.travel.waiting?": waiting for the team":": travel together?");if(s.travel.question) {if(BbCoopStyle::Button("Yes")) Send(BB_COOP_TRAVEL_ANSWER,1);ImGui::SameLine();if(BbCoopStyle::Button("No")) Send(BB_COOP_TRAVEL_ANSWER,0);}if(s.travel.seconds_left>0) ImGui::Text("%.0f seconds left",s.travel.seconds_left);}ImGui::End();}
    if(s.has_load) Loading(s);
}
}
bool Dispatch(const BbGpuRecord* record) {
    if(record->type!=BBGPU_REC_COOP_UI) return false;
    if(record->bytes!=sizeof(SnapshotRecord)) return true;
    const auto& s=reinterpret_cast<const SnapshotRecord*>(record)->view;
    if(s.version!=BB_COOP_UI_VERSION) return true;
    {std::scoped_lock lock{snapshot_mutex};latest=s;}
    enabled=s.enabled!=0;visible=s.enabled!=0;
    want_portrait=s.enabled && s.want_portrait;
    capture=s.enabled && (s.team_screen || s.map_screen || s.has_load || s.trade_id || s.duel_id || (s.has_travel && s.travel.question));
    return true;
}
bool Visible() {return visible;}
bool WantsPortrait() {return want_portrait;}
bool MenuButtons() {
    if(!enabled) return false;
    bool opened=false;
    if(BbCoopStyle::Button("Co-op Team")) { Send(BB_COOP_TEAM_MENU,1);opened=true; }
    ImGui::SameLine();
    if(BbCoopStyle::Button("World Map")) { Send(BB_COOP_MAP_MENU,1);opened=true; }
    ImGui::Separator();return opened;
}
bool CapturesInput() {return capture || trade_capture;}
bool Shortcut(const SDL_Event& e) {
    if(!enabled || e.type!=SDL_EVENT_KEY_DOWN || e.key.repeat) return false;
    std::scoped_lock lock{snapshot_mutex};
    switch(e.key.scancode) {
    case SDL_SCANCODE_F6:Send(BB_COOP_TEAM_MENU,!latest.team_screen);return true;
    case SDL_SCANCODE_F8:Send(BB_COOP_MAP_MENU,!latest.map_screen);return true;
    case SDL_SCANCODE_F7:Send(BB_COOP_CAMERA);return true;
    case SDL_SCANCODE_G:if(!CapturesInput()) {Send(BB_COOP_PING);return true;}return false;
    case SDL_SCANCODE_ESCAPE:
        if(trade_open) {trade_open=false;trade_capture=false;}
        else if(latest.map_screen) Send(BB_COOP_MAP_MENU,0);
        else if(latest.team_screen && latest.lobby.menu) CloseTeam();else return false;
        return true;
    default:return false;
    }
}
void Render() {
    static BbCoopUi s{};{std::scoped_lock lock{snapshot_mutex};s=latest;}
    if(!s.enabled || !s.map_screen || BbSettings::Get().map_view!=2) BbOverlay::MetalBackend::ClearMap3d();
    if(!s.enabled) return;
    BbCoopStyle::Scope theme;
    trade_capture=trade_open;
    Hud(s);if(s.team_screen) Team(s);if(s.map_screen) Map(s);Trade(s);Questions(s);
}
}
