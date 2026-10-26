// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_coop_ui.h"
#include "bbport_split.h"
#include "bbport_overlay_metal.h"
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
void Team(const BbCoopUi& s) {
    const auto& lobby=s.lobby;
    ImGui::SetNextWindowSize(ImVec2(740*ImGui::GetFontSize()/17,570*ImGui::GetFontSize()/17),ImGuiCond_FirstUseEver);
    bool open=true;
    if(ImGui::Begin("Co-op Team",lobby.menu?&open:nullptr)) {
        ImGui::TextUnformatted(lobby.signed_in?"Connected":"Connecting to the co-op server...");
        if(*lobby.notice) ImGui::TextWrapped("%s",lobby.notice);
        if(!lobby.signed_in) ImGui::TextWrapped("Check the server address and connection if this persists. You can return to offline play from the launcher.");
        if(lobby.in_team) {
            if(*lobby.code) { ImGui::Text("Team code: %s",lobby.code);ImGui::SameLine();if(ImGui::SmallButton("Copy")) ImGui::SetClipboardText(lobby.code); }
            if(lobby.leader && ImGui::SmallButton("New team code")) Lobby(BBCOOP_TEAM_NEW_CODE);
            if(ImGui::BeginTable("members",3,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Hunter");ImGui::TableSetupColumn("State");ImGui::TableSetupColumn("Team");ImGui::TableHeadersRow();
                for(int i=0;i<std::clamp(lobby.member_count,0,4);++i) {
                    const auto& member=lobby.members[i];ImGui::PushID(i);ImGui::TableNextRow();
                    ImGui::TableNextColumn();ImGui::Text("%s%s",Hunter(member.name),!std::strcmp(member.id,lobby.me)?" (you)":"");
                    ImGui::TableNextColumn();ImGui::TextUnformatted(member.state==BBCOOP_LOBBY_OFFLINE?"Offline":member.state==BBCOOP_LOBBY_PLAYING?"Playing":member.ready?"Ready":"At title");
                    ImGui::TableNextColumn();if(lobby.leader && i>0 && ImGui::SmallButton("Remove")) Lobby(BBCOOP_TEAM_REMOVE,member.id);
                    ImGui::PopID();
                } ImGui::EndTable();
            }
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
            if(ImGui::Checkbox("Allow invasions (requires server support)",&invaders)) Send(BB_COOP_ALLOW_INVADERS,invaders);
            ImGui::EndDisabled();
            ImGui::TextWrapped("Host rules: Rot %s, travel together %s, travel vote %s, enemy scaling %s",lobby.rule_rot?"on":"off",lobby.rule_travel?"on":"off",lobby.rule_vote?"on":"off",lobby.rule_scale?"on":"off");
            if(!lobby.menu && lobby.leader && !lobby.started && ImGui::Button("Start hunt")) Send(BB_COOP_CHOOSE,BBCOOP_TEAM_GO_START);
            if(lobby.playing && !lobby.in_hunt && ImGui::Button("Join the hunt")) Send(BB_COOP_JOIN_HUNT);
            if(ImGui::Button("Leave team")) Lobby(BBCOOP_TEAM_LEAVE);
            if(lobby.leader && !lobby.started && lobby.world>=0 && lobby.world<lobby.world_count) {
                static char forget[40]{};
                if(ImGui::Button("Forget selected world...")) { std::snprintf(forget,sizeof(forget),"%s",lobby.worlds[lobby.world].id);ImGui::OpenPopup("Forget world?"); }
                if(ImGui::BeginPopupModal("Forget world?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted("Remove this world from your saved-world list?");
                    if(ImGui::Button("Forget")) { Lobby(BBCOOP_TEAM_FORGET_WORLD,forget);ImGui::CloseCurrentPopup(); }
                    ImGui::SameLine();if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
                }
            }
        } else {
            ImGui::BeginDisabled(!lobby.signed_in);
            if(ImGui::Button("Create team")) Lobby(BBCOOP_TEAM_CREATE);
            if(!lobby.menu) {ImGui::SameLine();if(ImGui::Button("Play alone")) Send(BB_COOP_CHOOSE,BBCOOP_TEAM_GO_ALONE);}
            static char code[32]{};ImGui::InputText("Team code",code,sizeof(code));ImGui::SameLine();if(ImGui::Button("Join")) Lobby(BBCOOP_TEAM_JOIN_CODE,code);
            if(lobby.playing) {
                const bool invading=s.has_invasion && s.invasion.role==BBCOOP_INVASION_INVADER && s.invasion.phase!=BBCOOP_INVASION_ENDED;
                if(ImGui::Button(invading?"Leave invasion":"Search for an invasion")) Send(BB_COOP_INVASION,!invading);
            }
            ImGui::EndDisabled();
        }
        for(int i=0;i<std::clamp<int>(lobby.invite_count,0,BBCOOP_LOBBY_INVITES);++i) {
            const auto& invite=lobby.invites[i];ImGui::PushID(100+i);ImGui::Separator();ImGui::Text("Invitation from %s",Hunter(invite.name));
            if(ImGui::Button("Accept")) Lobby(BBCOOP_TEAM_ACCEPT,invite.id);ImGui::SameLine();if(ImGui::Button("Decline")) Lobby(BBCOOP_TEAM_DECLINE,invite.id);ImGui::PopID();
        }
        if(lobby.in_team) {
            static char name[40]{};ImGui::InputText("Invite by name",name,sizeof(name));ImGui::SameLine();if(ImGui::Button("Invite")) Lobby(BBCOOP_TEAM_INVITE_NAME,name);
            if(ImGui::TreeNode("Online hunters")) {
                for(int i=0;i<std::clamp<int>(lobby.player_count,0,BBCOOP_LOBBY_PLAYERS);++i) {
                    const auto& p=lobby.players[i];ImGui::PushID(200+i);ImGui::TextUnformatted(Hunter(p.name));ImGui::SameLine();if(ImGui::SmallButton("Invite")) Lobby(BBCOOP_TEAM_INVITE,p.id);ImGui::PopID();
                } ImGui::TreePop();
            }
        }
        if(lobby.playing) {
            if(ImGui::Button("Trade items")) trade_open=true;
            ImGui::SameLine();if(ImGui::Button("World map")) Send(BB_COOP_MAP_MENU,1);
            if(s.duel.may_propose && ImGui::Button("Propose team duel")) Send(BB_COOP_DUEL_PROPOSE);
            if(s.duel.may_cancel && ImGui::Button("End duel")) Send(BB_COOP_DUEL_CANCEL);
        }
        if(lobby.menu && !lobby.playing) ImGui::TextWrapped("Close this menu and select Continue or Load Game to choose your character. Your team will be ready when that character's team screen opens.");
        if(lobby.menu && ImGui::Button(lobby.playing?"Return to game":"Return to title")) CloseTeam();
        ImGui::TextDisabled("F6 Team   F8 Map   G Ping   F7 Free camera");
    }
    ImGui::End();if(!open) CloseTeam();
}
void Trade(const BbCoopUi& s) {
    if(!trade_open) return;
    ImGui::SetNextWindowSize(ImVec2(650*ImGui::GetFontSize()/17,470*ImGui::GetFontSize()/17),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Trade with teammate",&trade_open)) {
        if(!s.trade_on || !s.partner_count) ImGui::TextWrapped("Trading is available with a standing teammate on your current map.");
        static int partner=0,quantity=1;static uint32_t selected_handle=0,selected_kind=0,selected_item=0;
        partner=std::clamp(partner,0,std::max(0,s.partner_count-1));
        if(ImGui::BeginCombo("Give to",s.partner_count?Hunter(s.partners[partner].name):"No teammate nearby")) {
            for(int i=0;i<std::clamp(s.partner_count,0,4);++i) if(ImGui::Selectable(Hunter(s.partners[i].name),partner==i)) partner=i;
            ImGui::EndCombo();
        }
        static ImGuiTextFilter filter;filter.Draw("Find item");
        const BbCoopInventoryItem* selected=nullptr;
        if(ImGui::BeginChild("inventory",ImVec2(0,250*ImGui::GetFontSize()/17),ImGuiChildFlags_Borders)) {
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
        if(ImGui::Button("Offer...")) {
            pending={};pending.version=BB_COOP_UI_VERSION;pending.action=BB_COOP_TRADE_OFFER;
            pending.kind=selected->kind;pending.item=selected->id;pending.handle=selected->handle;pending.count=quantity;
            std::snprintf(pending.text,sizeof(pending.text),"%s",s.partners[partner].id);
            std::snprintf(confirmation,sizeof(confirmation),"Give %d %s to %s?",quantity,selected->name,Hunter(s.partners[partner].name));
            ImGui::OpenPopup("Confirm trade");
        } ImGui::EndDisabled();
        if(ImGui::BeginPopupModal("Confirm trade",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s",confirmation);ImGui::TextUnformatted("The items are reserved until your teammate answers.");
            if(ImGui::Button("Send offer")) {Send(pending);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    } ImGui::End();
}
void Map(const BbCoopUi& s) {
    bool open=true;ImGui::SetNextWindowSize(ImVec2(820*ImGui::GetFontSize()/17,650*ImGui::GetFontSize()/17),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("World map",&open)) {
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
        ImGui::SliderFloat("Zoom",&zoom,0.1f,5.0f,"%.1f");
        const ImVec2 start=ImGui::GetCursorScreenPos(),size=ImVec2(std::max(250.0f,ImGui::GetContentRegionAvail().x),300);
        ImGui::InvisibleButton("map canvas",size);auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(start,ImVec2(start.x+size.x,start.y+size.y),IM_COL32(18,24,28,255));
        float cx=shown==map.map?map.x:0,cz=shown==map.map?map.z:0;
        if(shown!=map.map) for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) if(map.lamps[i].map==shown) {cx=map.lamps[i].x;cz=map.lamps[i].z;break;}
        auto at=[&](float x,float z) {return ImVec2(start.x+size.x*.5f+(x-cx)*zoom,start.y+size.y*.5f-(z-cz)*zoom);};
        draw->PushClipRect(start,ImVec2(start.x+size.x,start.y+size.y),true);
        bool cleared=false;
        for(int i=0;i<std::clamp(map.cleared_count,0,24);++i) cleared|=map.cleared[i]==shown;
        if(cleared) draw->AddRectFilled(start,ImVec2(start.x+size.x,start.y+size.y),IM_COL32(38,54,48,255));
        else if(s.explored_map==shown) for(int i=0;i<std::clamp(s.explored_count,0,8192);++i) {
            const float x=s.explored[2*i]*BBCOOP_MAP_CELL,z=s.explored[2*i+1]*BBCOOP_MAP_CELL;
            draw->AddRectFilled(at(x,z+BBCOOP_MAP_CELL),at(x+BBCOOP_MAP_CELL,z),IM_COL32(45,65,61,255));
        }
        for(float x=0;x<size.x;x+=40) draw->AddLine(ImVec2(start.x+x,start.y),ImVec2(start.x+x,start.y+size.y),IM_COL32(40,48,52,255));
        for(float y=0;y<size.y;y+=40) draw->AddLine(ImVec2(start.x,start.y+y),ImVec2(start.x+size.x,start.y+y),IM_COL32(40,48,52,255));
        for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {const auto& lamp=map.lamps[i];if(lamp.map==shown) draw->AddCircleFilled(at(lamp.x,lamp.z),5,lamp.lit?IM_COL32(255,210,90,255):IM_COL32(100,100,100,255));}
        for(int i=0;i<std::clamp(map.member_count,0,4);++i) {const auto& m=map.members[i];if(m.map==shown && m.has_at) {auto p=at(m.x,m.z);draw->AddCircleFilled(p,5,IM_COL32(80,200,255,255));draw->AddText(ImVec2(p.x+7,p.y),IM_COL32_WHITE,Hunter(m.name));}}
        if(map.has_me && shown==map.map) draw->AddCircleFilled(at(map.x,map.z),6,IM_COL32(130,255,150,255));
        if(map.has_stain && map.stain.map==shown) draw->AddCircleFilled(at(map.stain.x,map.stain.z),6,IM_COL32(230,60,70,255));
        for(int i=0;i<std::clamp(s.pings.count,0,4);++i) {const auto& ping=s.pings.pings[i];if(ping.map==shown) draw->AddCircle(at(ping.x,ping.z),9,IM_COL32(255,240,130,255),0,2);}
        draw->PopClipRect();
        if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { const auto p=ImGui::GetIO().MousePos;BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_MAP_PING;c.map=shown;c.x=cx+(p.x-start.x-size.x*.5f)/zoom;c.z=cz-(p.y-start.y-size.y*.5f)/zoom;c.y=map.y;Send(c); }
        ImGui::TextDisabled("Green: you   Blue: teammates   Gold: lit lamps   Red: bloodstain   Right-click: ping");
        ImGui::TextDisabled(cleared?"Area cleared":"Explored ground is shaded green; exploration is saved.");
        if(map.has_stain) ImGui::Text("Bloodstain: %d echoes",map.stain.echoes);
        if(ImGui::BeginChild("lamps",ImVec2(0,140))) for(int i=0;i<std::clamp<int>(map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {
            const auto& lamp=map.lamps[i];if(lamp.map!=shown) continue;ImGui::PushID(i);
            ImGui::Text("%s%s",s.lamp_names[i],lamp.lit?"":" (unlit)");ImGui::SameLine();
            ImGui::BeginDisabled(!lamp.lit || map.block!=BBCOOP_MAP_GO);if(ImGui::SmallButton("Travel")) Send(BB_COOP_TRAVEL,lamp.warp_point);ImGui::EndDisabled();ImGui::PopID();
        } ImGui::EndChild();
        if(map.block!=BBCOOP_MAP_GO) ImGui::TextDisabled("Travel is unavailable while loading, downed, in combat with a boss, or already travelling.");
        if(ImGui::Button("Close map")) open=false;
    } ImGui::End();if(!open) Send(BB_COOP_MAP_MENU,0);
}
void Portrait(const BbCoopUi& s,const char* board) {
    for(int i=0;i<std::clamp(s.portrait_count,0,4);++i) {
        const auto& portrait=s.portraits[i];if(std::strcmp(portrait.board,board)) continue;
        void* texture=BbOverlay::MetalBackend::PortraitTexture(i,portrait.id,portrait.rgba,128);
        if(texture) {ImGui::Image(ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))},ImVec2(40,40));ImGui::SameLine();}
        return;
    }
}
void Hud(const BbCoopUi& s) {
    const auto& io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20,std::max(280.f,io.DisplaySize.y*.32f)),ImGuiCond_Always);ImGui::SetNextWindowBgAlpha(.65f);
    if(ImGui::Begin("##party",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted(s.lobby.signed_in?"Co-op connected   F6 Team   F8 Map":"Co-op connecting...");
        for(int i=0;i<std::clamp(s.panel.member_count,0,4);++i) {const auto& m=s.panel.members[i];Portrait(s,m.board);ImGui::Text("%s: %s%s",Hunter(m.name),Vital(m.vital),m.here?"":" (another area)");ImGui::ProgressBar(m.hp_max>0?std::clamp(float(m.hp)/m.hp_max,0.f,1.f):0,ImVec2(200,8),"");ImGui::Text("Vials %u   Bullets %u",m.vials,m.bullets);}
        if(s.has_panel && s.panel.vital==BBCOOP_VITAL_DOWN) {ImGui::TextUnformatted("DOWNED - a teammate can attack to revive you");ImGui::ProgressBar(s.panel.needed?float(s.panel.landed)/s.panel.needed:0,ImVec2(260,14),"Revive");ImGui::TextUnformatted("Hold Circle (Left Shift) to give up and respawn.");}
        if(s.panel.vital==BBCOOP_VITAL_SPECTATE) ImGui::Text("Spectating %s",Hunter(s.panel.watching_name));
        if(s.has_rot) ImGui::Text("Rot penalties: %d (rest at a lamp to clear)",s.rot.count);
        if(s.camera_on) ImGui::TextUnformatted("Free camera (F7 to return)");
        if(s.skip_want) ImGui::Text("Skip cutscene: %d / %d votes (Options)",s.skip_want,s.skip_watching);
        if(*s.notice) ImGui::TextWrapped("%s",s.notice);
        if(s.has_invasion) {ImGui::Text("Invasion: %s (phase %d)",Hunter(s.invasion.name),s.invasion.phase);}
        if(s.duel.countdown_shown) ImGui::Text("Duel: %s %d",s.duel.countdown_fight?"Fight!":"Starts in",s.duel.countdown_number);
    } ImGui::End();
    auto* draw=ImGui::GetForegroundDrawList();
    if(s.has_compass) {
        const float center=io.DisplaySize.x*.5f,y=60,span=200;
        draw->AddLine(ImVec2(center-span,y),ImVec2(center+span,y),IM_COL32(200,200,200,190));
        for(int i=0;i<std::clamp(s.compass.count,0,4);++i) {const auto& m=s.compass.markers[i];const float x=center+std::clamp(m.x,-1.f,1.f)*span;draw->AddCircleFilled(ImVec2(x,y),5, m.vital==BBCOOP_VITAL_DOWN?IM_COL32(255,80,70,255):IM_COL32(100,210,255,255));char label[64];std::snprintf(label,sizeof(label),"%s %.0fm",Hunter(m.name),m.distance);draw->AddText(ImVec2(x-30,y+9),IM_COL32_WHITE,label);}
        if(s.compass.has_stain) draw->AddCircleFilled(ImVec2(center+s.compass.stain.x*span,y),5,IM_COL32(220,50,50,255));
    }
    for(int i=0;i<std::clamp(s.pings.count,0,4);++i) {const auto& p=s.pings.pings[i];if(!p.on_screen) continue;ImVec2 point(p.screen_x*io.DisplaySize.x,p.screen_y*io.DisplaySize.y);draw->AddCircle(point,9,IM_COL32(255,230,90,255),0,2);char text[80];std::snprintf(text,sizeof(text),"%s %.0fm",Hunter(p.name),p.distance);draw->AddText(ImVec2(point.x+12,point.y),IM_COL32_WHITE,text);}
}
void Questions(const BbCoopUi& s) {
    if(s.trade_id) {
        ImGui::SetNextWindowSize(ImVec2(450,0),ImGuiCond_Always);
        if(ImGui::Begin("Trade offer",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextWrapped("%s",s.trade_question);if(ImGui::Button("Accept")) Send(BB_COOP_TRADE_ANSWER,1,"",s.trade_id);ImGui::SameLine();if(ImGui::Button("Decline")) Send(BB_COOP_TRADE_ANSWER,0,"",s.trade_id);
        } ImGui::End();
    }
    if(s.duel_id) { if(ImGui::Begin("Team duel invitation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text("%s proposes a duel.",Hunter(s.duel_name));if(ImGui::Button("Accept duel")) Send(BB_COOP_DUEL_ANSWER,1,"",s.duel_id);ImGui::SameLine();if(ImGui::Button("Decline duel")) Send(BB_COOP_DUEL_ANSWER,0,"",s.duel_id);}ImGui::End(); }
    if(s.has_travel && (s.travel.question || s.travel.waiting)) {if(ImGui::Begin("Travel with team",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text("%s%s",s.travel.from,s.travel.waiting?": waiting for the team":": travel together?");if(s.travel.question) {if(ImGui::Button("Yes")) Send(BB_COOP_TRAVEL_ANSWER,1);ImGui::SameLine();if(ImGui::Button("No")) Send(BB_COOP_TRAVEL_ANSWER,0);}if(s.travel.seconds_left>0) ImGui::Text("%.0f seconds left",s.travel.seconds_left);}ImGui::End();}
    if(s.has_load) {if(ImGui::Begin("Entering the hunt",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoCollapse)) {for(int i=0;i<std::clamp(s.load.count,0,4);++i) {const auto& row=s.load.rows[i];ImGui::TextUnformatted(Hunter(row.name));ImGui::ProgressBar(std::clamp(row.percent*.01f,0.f,1.f),ImVec2(300,15));}}ImGui::End();}
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
    if(ImGui::Button("Co-op Team")) { Send(BB_COOP_TEAM_MENU,1);opened=true; }
    ImGui::SameLine();
    if(ImGui::Button("World Map")) { Send(BB_COOP_MAP_MENU,1);opened=true; }
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
    if(!s.enabled) return;
    trade_capture=trade_open;
    Hud(s);if(s.team_screen) Team(s);if(s.map_screen) Map(s);Trade(s);Questions(s);
}
}
