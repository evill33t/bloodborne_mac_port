// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_coop_ui.h"
#include "bbport_split.h"
#include "bbport_map_charts.h"
#include "bbport_map_view.h"
#include "bbport_map_atlas.h"
#include <imgui_internal.h>
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
const char* T(const char* text) {return BbCoopStyle::Text(text);}
const char* Popup(const char* title) {static std::map<std::string,std::string> names;auto& text=names[title];text=std::string(T(title))+"###"+title;return text.c_str();}
constexpr float Pi=3.14159265358979323846f;
std::mutex snapshot_mutex;
BbCoopUi latest{};
std::atomic<bool> enabled{},capture{},visible{},trade_capture{};
std::atomic<bool> want_portrait{};
bool trade_open=false,map_was_open=false;
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
        const auto* member=i<std::clamp<int>(lobby.member_count,0,4)?&lobby.members[i]:nullptr;
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
            if(ImGui::SmallButton(T("Remove"))) Lobby(BBCOOP_TEAM_REMOVE,member->id);
            ImGui::SetCursorScreenPos(next);ImGui::Dummy(ImVec2(0,0));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();ImGui::Spacing();
}
// Host and online display preferences mirror Kaniel's Display and Team screens.
void Preferences() {
    if(!ImGui::CollapsingHeader(T("Online display"))) return;
    auto& v=BbSettings::Get();bool changed=false;
    auto toggle=[&](const char* name,std::atomic<bool>& setting) {bool on=setting;if(ImGui::Checkbox(T(name),&on)) {setting=on;changed=true;}};
    toggle("Teammate compass",v.compass);
    int position=v.compass_position;const char* positions[]={T("Top"),T("Bottom"),T("Around the middle")};
    if(ImGui::Combo(T("Compass position"),&position,positions,3)) {v.compass_position=position;changed=true;}
    toggle("Bloodstains on compass and map",v.bloodstains);toggle("Team pings",v.team_pings);
    int key=v.ping_key;if(ImGui::Combo(T("Ping key"),&key,BbSettings::PingKeyNames,BbSettings::PingKeyCount)) {v.ping_key=key;changed=true;}
    toggle("Free camera hotkey",v.free_camera);
    int map_view=v.map_view;const char* maps[]={T("Coordinates"),T("Rendered terrain"),T("Live 3D"),T("Prerendered")};
    if(ImGui::Combo(T("Map"),&map_view,maps,4)) {v.map_view=map_view;changed=true;}
    toggle("Map fog of war",v.map_fog);
    int language=v.menu_language_auto?0:int(v.menu_language.load())+1;
    const char* language_name=language?BbLanguage::LanguageName(v.menu_language):T("Game language");
    if(ImGui::BeginCombo(T("Language"),language_name)) {
        if(ImGui::Selectable(T("Game language"),!language)) {v.menu_language_auto=true;v.menu_language=BbLanguage::GameLanguage();changed=true;}
        for(int i=0;i<BbLanguage::MenuLanguageCount;++i) if(ImGui::Selectable(BbLanguage::LanguageName(static_cast<BbLanguage::MenuLanguage>(i)),language==i+1)) {v.menu_language_auto=false;v.menu_language=static_cast<BbLanguage::MenuLanguage>(i);changed=true;}
        ImGui::EndCombo();
    }
    if(changed) BbSettings::Save();
}
// v1.3.1 social actions use the same bounded cross-process lobby commands as invitations.
void SocialPlayer(const bbcoop_lobby_entry& player, const bbcoop_lobby& lobby) {
    ImGui::PushID(player.id);
    ImGui::TextUnformatted(Hunter(player.name)); ImGui::SameLine();
    ImGui::TextDisabled("%s", T(player.state == BBCOOP_LOBBY_PLAYING ? "In the hunt" :
                               player.state == BBCOOP_LOBBY_OFFLINE ? "Away" : "Choosing a character"));
    if (player.relation == BBCOOP_FRIEND_RECEIVED) {
        if (ImGui::SmallButton(T("Accept friend request"))) Lobby(BBCOOP_TEAM_FRIEND_ADD, player.id);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Decline"))) Lobby(BBCOOP_TEAM_FRIEND_DECLINE, player.id);
    } else if (player.relation == BBCOOP_FRIEND_SENT) {
        if (ImGui::SmallButton(T("Cancel friend request"))) Lobby(BBCOOP_TEAM_FRIEND_REMOVE, player.id);
    } else if (player.relation == BBCOOP_FRIEND_YES) {
        if (ImGui::SmallButton(T("Remove friend..."))) ImGui::OpenPopup(Popup("Remove friend?"));
        if (ImGui::BeginPopupModal(Popup("Remove friend?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(Hunter(player.name));
            if (BbCoopStyle::Button("Remove")) { Lobby(BBCOOP_TEAM_FRIEND_REMOVE, player.id); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); if (BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    } else if (player.relation != BBCOOP_FRIEND_SELF && std::strcmp(player.id, lobby.me)) {
        if (ImGui::SmallButton(T("Add friend"))) Lobby(BBCOOP_TEAM_FRIEND_ADD, player.id);
    }
    if (player.relation != BBCOOP_FRIEND_SELF && std::strcmp(player.id, lobby.me)) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!lobby.in_team || player.in_my_team || player.state == BBCOOP_LOBBY_OFFLINE ||
                             lobby.member_count >= std::clamp<int>(lobby.team_size, 1, BBCOOP_LOBBY_TEAM));
        if (ImGui::SmallButton(T("Invite"))) Lobby(BBCOOP_TEAM_INVITE, player.id);
        ImGui::EndDisabled();
    }
    ImGui::Separator(); ImGui::PopID();
}
void Friends(const bbcoop_lobby& lobby) {
    if (!ImGui::BeginPopupModal(Popup("Friends"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static char query[40]{};
    const bool entered = ImGui::InputText(T("Hunter's name"), query, sizeof(query), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (BbCoopStyle::Button("Search") || entered) Lobby(BBCOOP_TEAM_SEARCH, query);
    ImGui::SameLine();
    if (BbCoopStyle::Button("Clear")) { query[0] = 0; Lobby(BBCOOP_TEAM_SEARCH); }
    ImGui::BeginChild("social lists", ImVec2(std::min(650*BbCoopStyle::Scale(), ImGui::GetIO().DisplaySize.x-100*BbCoopStyle::Scale()), 300*BbCoopStyle::Scale()), ImGuiChildFlags_Borders);
    const auto list = [&](const char* title, const auto& entries, int count) {
        ImGui::PushID(title); ImGui::SeparatorText(T(title));
        for (int i=0; i<std::clamp(count, 0, int(std::size(entries))); ++i) SocialPlayer(entries[i], lobby);
        ImGui::PopID();
    };
    list("Your friends", lobby.friends, lobby.friend_count);
    list("Friend requests", lobby.requests, lobby.request_count);
    list("Sent requests", lobby.sent, lobby.sent_count);
    list("Search results", lobby.search, lobby.search_count);
    ImGui::EndChild();
    if (BbCoopStyle::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void OpenTeams(const bbcoop_lobby& lobby) {
    if (lobby.in_team || !lobby.open_count) return;
    if (!ImGui::TreeNode(T("Open teams"))) return;
    for (int i=0; i<std::clamp<int>(lobby.open_count,0,BBCOOP_LOBBY_OPEN); ++i) {
        const auto& team=lobby.open_teams[i]; ImGui::PushID(team.id);
        ImGui::Text("%s (%d/%d)", Hunter(team.name), team.members, std::clamp<int>(lobby.team_size,1,BBCOOP_LOBBY_TEAM));
        ImGui::SameLine();
        if (team.asked) { if (ImGui::SmallButton(T("Cancel request"))) Lobby(BBCOOP_TEAM_ASK_CANCEL,team.id); }
        else if (ImGui::SmallButton(T(team.open?"Join":"Ask to join"))) Lobby(BBCOOP_TEAM_ASK_JOIN,team.id);
        ImGui::PopID();
    }
    ImGui::TreePop();
}
void WorldPicker(const BbCoopUi& s) {
    const auto& lobby=s.lobby;const auto display=ImGui::GetIO().DisplaySize;const float scale=BbCoopStyle::Scale();
    ImGui::SetNextWindowSize(ImVec2(std::min(display.x-60*scale,1100.f*scale),std::min(display.y-60*scale,750.f*scale)),ImGuiCond_Appearing);
    if(!ImGui::BeginPopupModal(Popup("Choose a world"),nullptr,ImGuiWindowFlags_NoCollapse)) return;
    ImGui::TextWrapped(T("%s"),T("Every hunt in a world shares its progress. Your character keeps its own equipment."));
    static char selected[40]{};static char forget[40]{};static char reset[40]{};
    if(ImGui::BeginChild("world list",ImVec2(0,std::max(150.f,ImGui::GetContentRegionAvail().y-100*scale)),ImGuiChildFlags_Borders)) {
        if(ImGui::Selectable(T("A new world"),lobby.world<0)) {Lobby(BBCOOP_TEAM_WORLD);ImGui::CloseCurrentPopup();}
        ImGui::TextWrapped(T("%s"),T("Starts from the leader's save; kept on the server from the first hunt."));
        for(int i=0;i<std::clamp<int>(lobby.world_count,0,BBCOOP_LOBBY_WORLDS);++i) {
            const auto& world=lobby.worlds[i];ImGui::PushID(world.id);ImGui::Separator();
            ImGui::BeginDisabled(world.busy);const auto at=ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("##world",lobby.world==i,ImGuiSelectableFlags_None,ImVec2(0,ImGui::GetTextLineHeightWithSpacing()*4+12*scale))) {std::snprintf(selected,sizeof(selected),"%s",world.id);Lobby(BBCOOP_TEAM_WORLD,world.id);ImGui::CloseCurrentPopup();}
            const auto next=ImGui::GetCursorScreenPos();ImGui::SetCursorScreenPos(ImVec2(at.x+10*scale,at.y+6*scale));
            ImGui::TextUnformatted(world.name);
            ImGui::Text(T("%s%s   %s%s   %d %s"),T("Last played "),world.when,T("Made "),world.created,world.lamps,T("lamps lit"));
            ImGui::TextWrapped(T("%s"),world.characters);if(world.busy) ImGui::TextUnformatted(T("In use"));
            ImGui::SetCursorScreenPos(next);ImGui::Dummy(ImVec2(0,0));ImGui::EndDisabled();ImGui::PopID();
        }
    } ImGui::EndChild();
    if(lobby.world>=0 && lobby.world<lobby.world_count && !lobby.worlds[lobby.world].busy) {
        if(BbCoopStyle::Button("Reset lamps...")) { std::snprintf(reset,sizeof(reset),"%s",lobby.worlds[lobby.world].id); ImGui::OpenPopup(Popup("Reset lamps?")); }
        if(ImGui::BeginPopupModal(Popup("Reset lamps?"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s",T("Extinguish every lamp in this world? Character equipment is kept."));
            if(BbCoopStyle::Button("Reset lamps")) { Lobby(BBCOOP_TEAM_RESET_LAMPS,reset); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
        if(BbCoopStyle::Button("Forget this world")) {std::snprintf(forget,sizeof(forget),"%s",lobby.worlds[lobby.world].id);ImGui::OpenPopup(Popup("Forget world?"));}
        if(ImGui::BeginPopupModal(Popup("Forget world?"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(T("%s"),T("Forgotten worlds go off the list; the server keeps their files aside."));
            if(BbCoopStyle::Button("Forget")) {Lobby(BBCOOP_TEAM_FORGET_WORLD,forget);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    }
    if(BbCoopStyle::Button("Back")) ImGui::CloseCurrentPopup();
    BbCoopStyle::Guide({{"Cross","Enter","Choose"},{"Circle","Esc","Back"}});
    if(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void Team(const BbCoopUi& s) {
    const auto& lobby=s.lobby;
    const auto display=ImGui::GetIO().DisplaySize;const float scale=BbCoopStyle::Scale();
    ImGui::SetNextWindowSize(ImVec2(std::min(1440*scale,display.x-60*scale),std::min(940*scale,display.y-50*scale)),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    bool open=true;
    if(BbCoopStyle::Begin("Co-op Team",lobby.menu?&open:nullptr)) {
        ImGui::TextUnformatted(T(lobby.signed_in?"Connected":"Connecting to the co-op server..."));
        ImGui::BeginDisabled(!lobby.signed_in);
        if(BbCoopStyle::Button("Friends")) ImGui::OpenPopup(Popup("Friends"));
        ImGui::SameLine();
        const auto cog=ImGui::GetCursorScreenPos(); const float cog_side=32*scale;
        if(ImGui::InvisibleButton("Team settings",ImVec2(cog_side,cog_side))) ImGui::OpenPopup(Popup("Team settings"));
        BbCoopStyle::Art(ImGui::GetWindowDrawList(),BbCoopArt::Piece::Settings,cog,ImVec2(cog.x+cog_side,cog.y+cog_side));
        if(ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("%s",T("Team settings"));
        ImGui::EndDisabled();
        Friends(lobby);
        if(ImGui::BeginPopupModal(Popup("Team settings"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            Preferences();
            if(BbCoopStyle::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if(*lobby.notice) ImGui::TextWrapped(T("%s"),lobby.notice);
        if(!lobby.signed_in) ImGui::TextWrapped("%s",T("Check the server address and connection if this persists. You can return to offline play from the launcher."));
        if(lobby.in_team) {
            HunterCards(s);
            if(*lobby.code) { ImGui::Text("%s: %s",T("Team code"),lobby.code);ImGui::SameLine();if(ImGui::SmallButton(T("Copy"))) ImGui::SetClipboardText(lobby.code); }
            if(lobby.leader && ImGui::SmallButton(T("New team code"))) Lobby(BBCOOP_TEAM_NEW_CODE);
            ImGui::SeparatorText(T("The hunt"));
            const char* world=lobby.world>=0 && lobby.world<lobby.world_count?lobby.worlds[lobby.world].name:T("A new world");
            ImGui::Text(T("%s: %s"),T("The world"),world);
            ImGui::BeginDisabled(!lobby.leader || lobby.started || lobby.menu);
            if(BbCoopStyle::Button("Choose a world")) ImGui::OpenPopup(Popup("Choose a world"));
            WorldPicker(s);
            int death=lobby.death_mode;
            const char* death_modes[]={T("Go down (revive)"),T("Spectate teammates")};
            if(ImGui::Combo(T("On death"),&death,death_modes,2)) Lobby(BBCOOP_TEAM_DEATH_MODE,death?"1":"0");
            ImGui::EndDisabled();ImGui::BeginDisabled(!lobby.leader);
            bool invaders=lobby.allow_invaders!=0;
            if(ImGui::Checkbox(T("Allow invasions"),&invaders)) Send(BB_COOP_ALLOW_INVADERS,invaders);
            ImGui::EndDisabled();
            int join=std::clamp<int>(lobby.join,0,2); const char* modes[]={T("Invite only"),T("Ask to join"),T("Open")};
            ImGui::BeginDisabled(!lobby.leader);
            if(ImGui::Combo(T("Team joining"),&join,modes,3)) { const char* values[]={"0","1","2"};Lobby(BBCOOP_TEAM_OPEN,values[join]); }
            for(int i=0; i<std::clamp<int>(lobby.ask_count,0,BBCOOP_LOBBY_ASKS); ++i) {
                const auto& ask=lobby.asks[i]; ImGui::PushID(ask.id); ImGui::TextUnformatted(Hunter(ask.name)); ImGui::SameLine();
                if(ImGui::SmallButton(T("Accept"))) Lobby(BBCOOP_TEAM_ASK_ACCEPT,ask.id);
                ImGui::SameLine(); if(ImGui::SmallButton(T("Decline"))) Lobby(BBCOOP_TEAM_ASK_DECLINE,ask.id);
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            std::string rules;
            auto rule=[&](bool on,const char* text) {if(on) {if(!rules.empty()) rules+=" · ";rules+=T(text);}};
            rule(lobby.rule_tempo==1,"Boss tempo: Aggressive");rule(lobby.rule_tempo==2,"Boss tempo: Relentless");
            rule(lobby.rule_rot,"Rot");rule(lobby.rule_travel,lobby.rule_vote?"Travel together (vote)":"Travel together");rule(lobby.rule_scale,"Enemies scale with the team");
            ImGui::TextWrapped("%s%s",T("Host rules: "),rules.empty()?T("none"):rules.c_str());
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
                if(BbCoopStyle::Button("Forget selected world...")) { std::snprintf(forget,sizeof(forget),"%s",lobby.worlds[lobby.world].id);ImGui::OpenPopup(Popup("Forget world?")); }
                if(ImGui::BeginPopupModal(Popup("Forget world?"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(T("Remove this world from your saved-world list?"));
                    if(BbCoopStyle::Button("Forget")) { Lobby(BBCOOP_TEAM_FORGET_WORLD,forget);ImGui::CloseCurrentPopup(); }
                    ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
                }
            }
        } else {
            HunterCards(s);ImGui::SeparatorText(T("Gather your hunters"));
            ImGui::BeginDisabled(!lobby.signed_in);
            if(lobby.signed_in && !lobby.menu) {if(BbCoopStyle::Button("Choose a world")) ImGui::OpenPopup(Popup("Choose a world"));WorldPicker(s);}
            if(BbCoopStyle::Button("Create team")) Lobby(BBCOOP_TEAM_CREATE);
            ImGui::SetItemDefaultFocus();
            if(!lobby.menu) {ImGui::SameLine();if(BbCoopStyle::Button("Play alone")) Send(BB_COOP_CHOOSE,BBCOOP_TEAM_GO_ALONE);}
            static char code[32]{};ImGui::InputText(T("Team code"),code,sizeof(code));ImGui::SameLine();if(BbCoopStyle::Button("Join")) Lobby(BBCOOP_TEAM_JOIN_CODE,code);
            if(lobby.playing) {
                const bool invading=s.has_invasion && s.invasion.role==BBCOOP_INVASION_INVADER && s.invasion.phase!=BBCOOP_INVASION_ENDED;
                if(BbCoopStyle::Button(invading?"Leave invasion":"Search for an invasion")) Send(BB_COOP_INVASION,!invading);
            }
            ImGui::EndDisabled();
        }
        OpenTeams(lobby);
        if(lobby.invite_count || lobby.in_team) ImGui::SeparatorText(T("Invitations"));
        for(int i=0;i<std::clamp<int>(lobby.invite_count,0,BBCOOP_LOBBY_INVITES);++i) {
            const auto& invite=lobby.invites[i];ImGui::PushID(100+i);ImGui::Separator();ImGui::Text(T("Invitation from %s"),Hunter(invite.name));
            if(BbCoopStyle::Button("Accept")) {if(lobby.playing) Send(BB_COOP_JOIN_HUNT,0,invite.id);else Lobby(BBCOOP_TEAM_ACCEPT,invite.id);}ImGui::SameLine();if(BbCoopStyle::Button("Decline")) Lobby(BBCOOP_TEAM_DECLINE,invite.id);ImGui::PopID();
        }
        if(lobby.in_team) {
            static char name[40]{};ImGui::InputText(T("Invite by name"),name,sizeof(name));ImGui::SameLine();if(BbCoopStyle::Button("Invite")) Lobby(BBCOOP_TEAM_INVITE_NAME,name);
        }
        if(ImGui::TreeNode(T("Hunters online"))) {
            static ImGuiTextFilter hunters_filter; hunters_filter.Draw(T("Find hunter"));
            for(int i=0;i<std::clamp<int>(lobby.player_count,0,BBCOOP_LOBBY_PLAYERS);++i) {
                const auto& p=lobby.players[i];if(!hunters_filter.PassFilter(p.name)) continue;ImGui::PushID(200+i);ImGui::TextUnformatted(Hunter(p.name));ImGui::SameLine();
                ImGui::TextDisabled(T("%s"),T(p.state==BBCOOP_LOBBY_PLAYING?"In the hunt":p.state==BBCOOP_LOBBY_OFFLINE?"Away":"Choosing a character"));ImGui::SameLine();
                ImGui::BeginDisabled(!lobby.in_team || lobby.member_count>=4);
                if(ImGui::SmallButton(T("Invite"))) Lobby(BBCOOP_TEAM_INVITE,p.id);
                ImGui::EndDisabled();ImGui::PopID();
            } ImGui::TreePop();
        }
        if(lobby.playing) {
            if(BbCoopStyle::Button("Trade items")) trade_open=true;
            ImGui::SameLine();if(BbCoopStyle::Button("World map")) Send(BB_COOP_MAP_MENU,1);
            if(s.duel.may_propose && BbCoopStyle::Button("Propose team duel")) Send(BB_COOP_DUEL_PROPOSE);
            if(s.duel.may_cancel && BbCoopStyle::Button("End duel")) Send(BB_COOP_DUEL_CANCEL);
        }
        if(lobby.menu && !lobby.playing) ImGui::TextWrapped("%s",T("Close this menu and select Continue or Load Game to choose your character. Your team will be ready when that character's team screen opens."));
        if(lobby.menu) {
            if(BbCoopStyle::Button(lobby.playing?"Return to game":"Return to title")) CloseTeam();
            ImGui::SetItemDefaultFocus();
        }
        BbCoopStyle::Guide({{"Options","F6","Co-op"},{"TouchLeft","F8","Map"},{"Circle","Esc","Back"}});
    }
    ImGui::End();if(!open) CloseTeam();
}
void Trade(const BbCoopUi& s) {
    if(!trade_open) return;
    const float scale=BbCoopStyle::Scale();const auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(700*scale,display.x-40*scale),std::min(600*scale,display.y-50*scale)),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    if(BbCoopStyle::Begin("Trade with teammate",&trade_open)) {
        if(!s.trade_on || !s.partner_count) ImGui::TextWrapped("%s",T("Trading is available with a standing teammate on your current map."));
        static int partner=0,quantity=1;static uint32_t selected_handle=0,selected_kind=0,selected_item=0;
        partner=std::clamp(partner,0,std::max(0,s.partner_count-1));
        if(ImGui::BeginCombo(T("Give to"),s.partner_count?Hunter(s.partners[partner].name):"No teammate nearby")) {
            for(int i=0;i<std::clamp(s.partner_count,0,4);++i) if(ImGui::Selectable(Hunter(s.partners[i].name),partner==i)) partner=i;
            ImGui::EndCombo();
        }
        static ImGuiTextFilter filter;filter.Draw(T("Find item"));
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
        ImGui::InputInt(T("Quantity"),&quantity);
        const int maximum=selected?std::min(selected->count,selected->kind==1?999999999u:selected->kind==3?1u:99u):1;
        quantity=std::clamp(quantity,1,std::max(1,maximum));
        static BbCoopCommand pending{};static char confirmation[256]{};
        ImGui::BeginDisabled(!selected || !s.partner_count || !s.trade_on);
        if(BbCoopStyle::Button("Offer...")) {
            pending={};pending.version=BB_COOP_UI_VERSION;pending.action=BB_COOP_TRADE_OFFER;
            pending.kind=selected->kind;pending.item=selected->id;pending.handle=selected->handle;pending.count=quantity;
            std::snprintf(pending.text,sizeof(pending.text),"%s",s.partners[partner].id);
            std::snprintf(confirmation,sizeof(confirmation),"%s: %d %s → %s?",T("Give"),quantity,selected->name,Hunter(s.partners[partner].name));
            ImGui::OpenPopup(Popup("Confirm trade"));
        } ImGui::EndDisabled();
        if(ImGui::BeginPopupModal(Popup("Confirm trade"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(T("%s"),confirmation);ImGui::TextUnformatted(T("The items are reserved until your teammate answers."));
            if(BbCoopStyle::Button("Send offer")) {Send(pending);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(BbCoopStyle::Button("Cancel")) ImGui::CloseCurrentPopup();ImGui::EndPopup();
        }
    } ImGui::End();
}
// SPDX-License-Identifier: GPL-2.0-or-later
// Native Metal adaptation of Kaniel / lKaniel's Map namespace (bbport_overlay.cpp, 8d5e54f).
bool Pressed(std::initializer_list<ImGuiKey> keys,bool repeat=false) {
    for(auto key:keys) if(ImGui::IsKeyPressed(key,repeat)) return true;
    return false;
}
void Map(const BbCoopUi& s) {
    const auto& map=s.map;auto& prefs=BbSettings::Get();auto& io=ImGui::GetIO();
    const float scale=BbCoopStyle::Scale();bool open=true;
    static uint32_t shown=0,previous=0,requested=0;
    static float zoom=3,pan_x=0,pan_z=0,cy=0,yaw=0,pitch=.9f;
    static bool follow=true;static int floors=0,focused=0,side=1;
    static int three_d=2;static int travel_ask=0;
    static std::map<uint32_t,std::vector<int16_t>> explored;
    if(s.explored_other_map) explored[s.explored_other_map]={s.explored_other,s.explored_other+std::clamp(s.explored_other_count,0,8192)*2};
    if(s.explored_map) explored[s.explored_map]={s.explored,s.explored+std::clamp(s.explored_count,0,8192)*2};
    std::vector<uint32_t> areas;
    for(int i=0;i<std::clamp(s.area_count,0,48);++i) if(s.areas[i].map) areas.push_back(s.areas[i].map);
    if(areas.empty() && map.map) areas.push_back(map.map);
    if(!map_was_open) {shown=map.map;previous=0;requested=0;follow=true;map_was_open=true;}
    auto area_name=[&](uint32_t id)->const char* {
        for(int i=0;i<std::clamp(s.area_count,0,48);++i) if(s.areas[i].map==id) return s.areas[i].name;
        return T("Map");
    };
    const auto display=io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(20*scale,20*scale),ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(display.x-40*scale,display.y-40*scale),ImGuiCond_Appearing);
    if(BbCoopStyle::Begin("World map",&open,ImGuiWindowFlags_NoNavInputs)) {
        const bool keys=!io.WantTextInput && !ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId);
        if(keys && Pressed({ImGuiKey_GamepadFaceRight,ImGuiKey_GamepadBack,ImGuiKey_M})) open=false;
        if(keys && !areas.empty() && Pressed({ImGuiKey_GamepadL1,ImGuiKey_GamepadR1,ImGuiKey_Q,ImGuiKey_E,ImGuiKey_PageUp,ImGuiKey_PageDown})) {
            auto it=std::find(areas.begin(),areas.end(),shown);int n=it==areas.end()?0:int(it-areas.begin());
            n+=Pressed({ImGuiKey_GamepadR1,ImGuiKey_E,ImGuiKey_PageDown})?1:-1;
            shown=areas[(n+int(areas.size()))%areas.size()];
        }
        if(ImGui::BeginCombo(T("Area"),area_name(shown))) {
            for(auto id:areas) if(ImGui::Selectable(area_name(id),id==shown)) shown=id;
            ImGui::EndCombo();
        }
        const char* views[]={T("Coordinates"),T("Rendered terrain"),T("Live 3D"),T("Prerendered")};
        int view=prefs.map_view;
        if(ImGui::Combo(T("View"),&view,views,4)) {prefs.map_view=view;BbSettings::Save();}
        if(keys && Pressed({ImGuiKey_GamepadL3,ImGuiKey_V})) {
            if(view>=2) {three_d=view;view=1;} else view=three_d;
            prefs.map_view=view;BbSettings::Save();
        }
        bool fog=prefs.map_fog;
        if(ImGui::Checkbox(T("Fog of war"),&fog)) {prefs.map_fog=fog;BbSettings::Save();}
        ImGui::SameLine();ImGui::Checkbox(T("Follow player"),&follow);ImGui::SameLine();
        bool fit=BbCoopStyle::Button("Fit area");ImGui::SameLine();
        bool fit_all=BbCoopStyle::Button("All areas");
        const bool asked_before=travel_ask!=0;
        if(keys && Pressed({ImGuiKey_GamepadFaceUp,ImGuiKey_Tab})) fit_all=true;
        if(keys && Pressed({ImGuiKey_GamepadR3,ImGuiKey_F})) floors=(floors+1)%3;
        const char* floor_names[]={T("Automatic"),T("Roofs"),T("Floors")};
        ImGui::SameLine();ImGui::SetNextItemWidth(170*scale);
        ImGui::Combo(T("Level"),&floors,floor_names,3);
        if(view==3) {ImGui::SameLine();if(BbCoopStyle::Button("Turn")) side=side%4+1;}
        int projected=view==3?side:0;
        for(auto id:areas) if(view) {BbMapCharts::Prepare(id);if(projected) BbMapCharts::Prepare(id,projected);if(view==2) BbMapCharts::PrepareMesh(id);}
        auto chart=view?BbMapCharts::Get(shown):nullptr;
        auto side_chart=projected?BbMapCharts::Get(shown,projected):nullptr;
        auto mesh=view==2 && chart?BbMapCharts::GetMesh(shown):nullptr;
        bool live=view==2 && chart && mesh;
        bool oblique=projected && bool(side_chart);
        auto pictures=view?BbMapCharts::Atlas(oblique?projected:0):decltype(BbMapCharts::Atlas()){};
        if(oblique) pictures[shown]=side_chart;else if(chart) pictures[shown]=chart;
        if(previous!=shown) {
            const auto offset=BbMapView::AtlasOffset(shown);
            float x=shown==map.map?map.x:0,z=shown==map.map?map.z:0;cy=shown==map.map?map.y:0;
            if(shown!=map.map) for(int i=0;i<map.lamp_count;++i) if(map.lamps[i].map==shown) {x=map.lamps[i].x;z=map.lamps[i].z;cy=map.lamps[i].y;break;}
            pan_x=x+offset.dx;pan_z=z+offset.dz;previous=shown;focused=0;follow=true;
        }
        const auto me_offset=BbMapView::AtlasOffset(map.map);
        if(follow && map.has_me && shown==map.map) {pan_x=map.x+me_offset.dx;pan_z=map.z+me_offset.dz;cy=map.y;}
        if(keys && Pressed({ImGuiKey_GamepadStart,ImGuiKey_C}) && map.has_me) {shown=map.map;pan_x=map.x+me_offset.dx;pan_z=map.z+me_offset.dz;cy=map.y;follow=true;}
        if(s.explored_map!=shown && requested!=shown) {requested=shown;BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_MAP_AREA;c.map=shown;Send(c);}
        const float list_width=std::min(290*scale,ImGui::GetContentRegionAvail().x*.28f);
        const auto start=ImGui::GetCursorScreenPos();
        const ImVec2 size(std::max(180.f,ImGui::GetContentRegionAvail().x-list_width-20*scale),std::max(100.f,ImGui::GetContentRegionAvail().y-110*scale));
        ImGui::InvisibleButton("map canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
        const bool hovered=ImGui::IsItemHovered();const ImVec2 end(start.x+size.x,start.y+size.y),middle(start.x+size.x*.5f,start.y+size.y*.5f);
        auto* draw=ImGui::GetWindowDrawList();
        if(fit_all && !pictures.empty()) {
            float x0=FLT_MAX,z0=FLT_MAX,x1=-FLT_MAX,z1=-FLT_MAX;
            for(const auto& [id,p]:BbMapCharts::Atlas()) {const auto o=BbMapView::AtlasOffset(id);x0=std::min(x0,p->x0+o.dx);x1=std::max(x1,p->x0+p->width/p->ppm+o.dx);z0=std::min(z0,p->z1-p->height/p->ppm+o.dz);z1=std::max(z1,p->z1+o.dz);}
            float low=cy,high=cy;
            if(live) {
                auto scene=BbMapCharts::MeshAtlas();scene[shown]=mesh;
                low=FLT_MAX;high=-FLT_MAX;
                for(const auto& [id,m]:scene) {const auto offset=BbMapView::AtlasOffset(id);
                    for(size_t i=0;i+2<m->positions.size();i+=3) {x0=std::min(x0,m->positions[i]+offset.dx);x1=std::max(x1,m->positions[i]+offset.dx);z0=std::min(z0,m->positions[i+2]+offset.dz);z1=std::max(z1,m->positions[i+2]+offset.dz);low=std::min(low,m->positions[i+1]);high=std::max(high,m->positions[i+1]);}
                }
                if(low<=high) cy=(low+high)*.5f;else low=high=cy;
            }
            if(x0<x1 && z0<z1) {pan_x=(x0+x1)*.5f;pan_z=(z0+z1)*.5f;zoom=BbMapView::FitZoom({pan_x,cy,pan_z,1,live?yaw:oblique?BbMapArt::ViewOf(projected).yaw:0,live?pitch:oblique?BbMapArt::ViewOf(projected).pitch:Pi*.5f},x0,z0,x1,z1,size.x,size.y,low,high);follow=false;}
        } else if(fit && chart) {const auto o=BbMapView::AtlasOffset(shown);pan_x=chart->x0+chart->width/chart->ppm*.5f+o.dx;pan_z=chart->z1-chart->height/chart->ppm*.5f+o.dz;zoom=.9f*std::min(size.x/(chart->width/chart->ppm),size.y/(chart->height/chart->ppm));follow=false;}
        const float dt=std::min(io.DeltaTime,.1f);
        auto analog=[](ImGuiKey k){return ImGui::GetKeyData(k)->AnalogValue;};
        if(keys) {
            float lx=analog(ImGuiKey_GamepadLStickRight)-analog(ImGuiKey_GamepadLStickLeft)+(ImGui::IsKeyDown(ImGuiKey_D)?1:0)-(ImGui::IsKeyDown(ImGuiKey_A)?1:0);
            float ly=analog(ImGuiKey_GamepadLStickDown)-analog(ImGuiKey_GamepadLStickUp)+(ImGui::IsKeyDown(ImGuiKey_S)?1:0)-(ImGui::IsKeyDown(ImGuiKey_W)?1:0);
            const float rx=analog(ImGuiKey_GamepadRStickRight)-analog(ImGuiKey_GamepadRStickLeft),ry=analog(ImGuiKey_GamepadRStickDown)-analog(ImGuiKey_GamepadRStickUp);
            if(live) {yaw+=(rx+(ImGui::IsKeyDown(ImGuiKey_X)?1:0)-(ImGui::IsKeyDown(ImGuiKey_Z)?1:0))*1.8f*dt;pitch=std::clamp(pitch-ry*.9f*dt,.35f,1.5707f);}
            else zoom*=std::pow(2.f,-ry*dt*1.6f);
            zoom*=std::pow(2.f,dt*1.5f*((ImGui::IsKeyDown(ImGuiKey_GamepadR2)?1:0)-(ImGui::IsKeyDown(ImGuiKey_GamepadL2)?1:0)+(ImGui::IsKeyDown(ImGuiKey_Equal)?1:0)-(ImGui::IsKeyDown(ImGuiKey_Minus)?1:0)));
            if(std::abs(lx)+std::abs(ly)>.05f) {BbMapView::Camera moving{pan_x,cy,pan_z,zoom,live?yaw:oblique?BbMapArt::ViewOf(projected).yaw:0,live?pitch:oblique?BbMapArt::ViewOf(projected).pitch:Pi*.5f};auto p=moving.Ground(lx*dt*400,-ly*dt*400);pan_x=p[0];pan_z=p[1];follow=false;}
        }
        if(oblique) {yaw=BbMapArt::ViewOf(projected).yaw;pitch=BbMapArt::ViewOf(projected).pitch;}
        zoom=std::clamp(zoom,.05f,12.f);
        BbMapView::Camera camera{pan_x,cy,pan_z,zoom,live||oblique?yaw:0,live||oblique?pitch:Pi*.5f};
        if(hovered && io.MouseWheel) zoom=std::clamp(zoom*std::pow(1.25f,io.MouseWheel),.05f,12.f);
        if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {auto p=camera.Ground(-io.MouseDelta.x,-io.MouseDelta.y);pan_x=p[0];pan_z=p[1];follow=false;}
        if(live && hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right,6)) {yaw+=io.MouseDelta.x*.006f;pitch=std::clamp(pitch+io.MouseDelta.y*.004f,.35f,1.5707f);}
        if(!follow) {
            const auto flat=BbMapCharts::Atlas();
            auto under=[&](uint32_t id) {auto it=flat.find(id);if(it==flat.end()) return false;const auto o=BbMapView::AtlasOffset(id);const auto& c=*it->second;auto px=c.PixelOf(pan_x-o.dx,pan_z-o.dz);int x=int(px[0]),y=int(px[1]);return x>=0 && y>=0 && x<c.width && y<c.height && c.rgba[(size_t(y)*c.width+x)*4+3]!=0;};
            if(!under(shown)) for(const auto& [id,c]:flat) if(under(id)) {shown=previous=id;focused=0;break;}
        }
        camera.x=pan_x;camera.z=pan_z;camera.yaw=live||oblique?yaw:0;camera.pitch=live||oblique?pitch:Pi*.5f;camera.zoom=zoom;
        auto at=[&](uint32_t id,float x,float z,float y) {const auto o=BbMapView::AtlasOffset(id);auto p=camera.Project(x+o.dx,y,z+o.dz);return ImVec2(middle.x+p[0],middle.y+p[1]);};
        auto floor_at=[&](uint32_t id,float x,float z) {
            auto c=id==shown?chart:nullptr;
            if(!c) {const auto atlas=BbMapCharts::Atlas();if(auto it=atlas.find(id);it!=atlas.end()) c=it->second;}
            if(!c) return cy;
            auto pixel=c->PixelOf(x,z);int px=int(pixel[0])/BbMapArt::Chart::FloorCell,py=int(pixel[1])/BbMapArt::Chart::FloorCell;
            if(px<0 || py<0 || px>=c->storeys.width || py>=c->storeys.height) return cy;
            auto h=BbMapArt::ShownHeight(*c,size_t(py)*c->storeys.width+px,floors==1?NAN:cy);
            return h==BbMapArt::Chart::NoHeight?cy:h*.1f;
        };
        draw->PushClipRect(start,end,true);draw->AddRectFilled(start,end,IM_COL32(18,21,23,255));
        // Flat charts use the atlas world's coordinates. Oblique charts carry projected (u,w).
        std::vector<uint32_t> order;for(const auto& [id,p]:pictures) if(id!=shown) order.push_back(id);if(pictures.contains(shown)) order.push_back(shown);
        static std::map<uint32_t,std::pair<uint64_t,std::vector<uint8_t>>> fogged;
        for(auto id:order) {
            const auto& picture=*pictures[id];bool cleared=false;for(int i=0;i<map.cleared_count;++i) cleared|=map.cleared[i]==(id&0xffff0000u);
            auto& [last,pixels]=fogged[id];uint64_t stamp=uint64_t(reinterpret_cast<uintptr_t>(pictures[id].get()));
            auto mix=[&](uint64_t n){stamp^=n;stamp*=1099511628211ull;};mix(id);mix(view);mix(fog);mix(cleared);mix(floors);mix(int(cy*2));
            if(explored.contains(id)) {mix(explored[id].size());for(auto n:explored[id]) mix(uint16_t(n));}
            if(map.has_me && id==map.map) {mix(int(map.x/4));mix(int(map.z/4));}
            for(int i=0;i<map.lamp_count;++i) if(map.lamps[i].map==id) mix(map.lamps[i].lit);
            if(stamp!=last) {
                pixels=picture.rgba;
                bool floor=floors==2 || (floors==0 && chart && shown==map.map && BbMapArt::UnderRoof(*chart,map.x,map.y,map.z));
                if(!oblique && floor) BbMapArt::FloorView(picture,id==map.map && map.has_me?map.y:cy,pixels.data());
                std::vector<BbMapArt::Vec3> reveal;
                if(explored.contains(id)) for(size_t i=0;i+1<explored[id].size();i+=2) reveal.push_back({(explored[id][i]+.5f)*float(BBCOOP_MAP_CELL),(explored[id][i+1]+.5f)*float(BBCOOP_MAP_CELL),40});
                for(int i=0;i<map.lamp_count;++i) {const auto& l=map.lamps[i];if(l.map==id && l.lit) reveal.push_back({l.x,l.z,60});}
                if(map.has_me && id==map.map) reveal.push_back({map.x,map.z,40});
                if(oblique) for(auto& disc:reveal) {auto p=BbMapArt::ToView(BbMapArt::ViewOf(projected),disc[0],cy,disc[1]);disc[0]=p[0];disc[1]=p[1];}
                if(fog && !cleared) BbMapView::Fog(picture,reveal,pixels);
                last=stamp;
            }
            void* texture=BbOverlay::MetalBackend::MapTexture(stamp,picture,pixels.data(),id|unsigned(projected));
            ImVec2 a,b;
            if(oblique) {auto o=BbMapView::AtlasOffset(id);auto shift=BbMapArt::ToView(BbMapArt::ViewOf(projected),o.dx,0,o.dz);auto center=BbMapArt::ToView(BbMapArt::ViewOf(projected),pan_x,cy,pan_z);a={middle.x+(picture.x0+shift[0]-center[0])*zoom,middle.y-(picture.z1+shift[1]-center[1])*zoom};b={a.x+picture.width/picture.ppm*zoom,a.y+picture.height/picture.ppm*zoom};}
            else {a=at(id,picture.x0,picture.z1,cy);b=at(id,picture.x0+picture.width/picture.ppm,picture.z1-picture.height/picture.ppm,cy);}
            if(live && id==shown) {const auto o=BbMapView::AtlasOffset(shown);texture=BbOverlay::MetalBackend::Map3d(shown,mesh,*chart,{pan_x-o.dx,cy,pan_z-o.dz,zoom,yaw,pitch,size.x,size.y});a=start;b=end;}
            if(texture) draw->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture))),a,b,ImVec2(0,0),ImVec2(1,1),id==shown?IM_COL32_WHITE:IM_COL32(175,175,175,255));
            if(zoom<1.2f && pictures.size()>1) BbCoopStyle::Label(draw,ImVec2((a.x+b.x)*.5f,(a.y+b.y)*.5f),20*scale,BbCoopStyle::Ink(.7f),area_name(id),.5f);
            if(zoom>=1) for(const auto& label:picture.labels) for(int i=0;i<s.label_count;++i) if(s.labels[i].id==label.name_id) {auto pos=at(id,label.x,label.z,floor_at(id,label.x,label.z));pos.y+=22*scale;BbCoopStyle::Label(draw,pos,17*scale,BbCoopStyle::Muted(),s.labels[i].name,.5f,240*scale);break;}
        }
        if(live) {
            // Other prepared areas share the depth target and their atlas-world offsets.
            const auto previews=BbMapCharts::Atlas();
            for(const auto& [id,other]:BbMapCharts::MeshAtlas()) if(id!=shown && previews.contains(id)) {
                const auto& p=*previews.at(id);const auto offset=BbMapView::AtlasOffset(id);
                // This area's latest fog texture was prepared above. Its stable slot retains it.
                const auto& [version,fogged_pixels]=fogged[id];
                BbOverlay::MetalBackend::MapTexture(version,p,fogged_pixels.data(),id);
                BbOverlay::MetalBackend::Map3d(id,other,p,{pan_x-offset.dx,cy,pan_z-offset.dz,zoom,yaw,pitch,size.x,size.y},true);
            }
        }
        if(!chart) for(float x=0;x<size.x;x+=40) draw->AddLine(ImVec2(start.x+x,start.y),ImVec2(start.x+x,end.y),IM_COL32(40,48,52,255));
        std::vector<int> lamps;for(int i=0;i<map.lamp_count;++i) if(map.lamps[i].map==shown && map.lamps[i].lit) lamps.push_back(i);
        bool moving=keys && (std::abs(analog(ImGuiKey_GamepadLStickRight)-analog(ImGuiKey_GamepadLStickLeft))+std::abs(analog(ImGuiKey_GamepadLStickDown)-analog(ImGuiKey_GamepadLStickUp))>.05f);
        if(keys && !lamps.empty() && Pressed({ImGuiKey_GamepadDpadUp,ImGuiKey_GamepadDpadDown,ImGuiKey_UpArrow,ImGuiKey_DownArrow},true)) {
            focused=std::clamp(focused+(Pressed({ImGuiKey_GamepadDpadDown,ImGuiKey_DownArrow},true)?1:-1),0,int(lamps.size())-1);
            const auto& l=map.lamps[lamps[focused]];const auto o=BbMapView::AtlasOffset(l.map);pan_x=l.x+o.dx;pan_z=l.z+o.dz;follow=false;
        }
        static double mouse_moved=-10;
        if(hovered && (io.MouseDelta.x || io.MouseDelta.y)) mouse_moved=ImGui::GetTime();
        const ImVec2 cursor=hovered && ImGui::GetTime()-mouse_moved<2?io.MousePos:middle;
        int hover_lamp=-1;float best=30*scale*30*scale;
        for(int i=0;i<map.lamp_count;++i) {
            const auto& l=map.lamps[i];if(!pictures.contains(l.map) && l.map!=shown) continue;
            auto p=at(l.map,l.x,l.z,l.y);float d=(p.x-cursor.x)*(p.x-cursor.x)+(p.y-cursor.y)*(p.y-cursor.y);
            if(l.lit && d<best) {best=d;hover_lamp=i;}
            draw->AddCircleFilled(p,l.lit?5*scale:3*scale,l.lit?IM_COL32(255,210,90,255):IM_COL32(100,100,100,255));
        }
        if(hover_lamp>=0) {
            const auto& l=map.lamps[hover_lamp];auto p=at(l.map,l.x,l.z,l.y);draw->AddCircle(p,12*scale,BbCoopStyle::Ink(),24,2*scale);
            BbCoopStyle::Label(draw,ImVec2(p.x,p.y-30*scale),20*scale,BbCoopStyle::Ink(),s.lamp_names[hover_lamp],.5f);
            if(!moving && !follow && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && cursor.x==middle.x && cursor.y==middle.y) {const auto o=BbMapView::AtlasOffset(l.map);float ease=std::min(1.f,dt*10);pan_x+=(l.x+o.dx-pan_x)*ease;pan_z+=(l.z+o.dz-pan_z)*ease;}
            if(map.block==BBCOOP_MAP_GO && ((keys && Pressed({ImGuiKey_GamepadFaceDown,ImGuiKey_Enter})) || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) travel_ask=l.warp_point;
        }
        for(int i=0;i<map.member_count;++i) {const auto& m=map.members[i];if(m.has_at && (pictures.contains(m.map) || m.map==shown)) {auto p=at(m.map,m.x,m.z,m.y);BbCoopStyle::Face(draw,s,m.board,m.name,p,28*scale,i,m.vital==BBCOOP_VITAL_UP);BbCoopStyle::Label(draw,ImVec2(p.x+17*scale,p.y),16*scale,BbCoopStyle::Ink(),Hunter(m.name));}}
        if(map.has_me) draw->AddCircleFilled(at(map.map,map.x,map.z,map.y),6*scale,IM_COL32(130,255,150,255));
        if(prefs.bloodstains && map.has_stain) draw->AddCircleFilled(at(map.stain.map,map.stain.x,map.stain.z,map.stain.y),6*scale,IM_COL32(230,60,70,255));
        if(prefs.team_pings) for(int i=0;i<s.pings.count;++i) {const auto& p=s.pings.pings[i];draw->AddCircle(at(p.map,p.x,p.z,p.y),9*scale,IM_COL32(255,240,130,255),0,2*scale);}
        draw->AddLine(ImVec2(middle.x-12*scale,middle.y),ImVec2(middle.x+12*scale,middle.y),BbCoopStyle::Ink(.8f));
        draw->AddLine(ImVec2(middle.x,middle.y-12*scale),ImVec2(middle.x,middle.y+12*scale),BbCoopStyle::Ink(.8f));
        const bool right_click=hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[1]<36*scale*scale;
        if(prefs.team_pings && (right_click || (keys && Pressed({ImGuiKey_GamepadFaceLeft})))) {
            auto pick=camera.Ground((right_click?io.MousePos.x:middle.x)-middle.x,(right_click?io.MousePos.y:middle.y)-middle.y);
            uint32_t target=shown;for(const auto& [id,p]:BbMapCharts::Atlas()) {const auto o=BbMapView::AtlasOffset(id);auto xy=p->PixelOf(pick[0]-o.dx,pick[1]-o.dz);if(xy[0]>=0 && xy[1]>=0 && xy[0]<p->width && xy[1]<p->height) {target=id;if(id==shown) break;}}
            const auto o=BbMapView::AtlasOffset(target);BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_MAP_PING;c.map=target;c.x=pick[0]-o.dx;c.z=pick[1]-o.dz;c.y=floor_at(target,c.x,c.z);Send(c);
        }
        draw->PopClipRect();
        // Source-style lamp list and confirmation retain keyboard and controller access.
        ImGui::SameLine();ImGui::BeginGroup();
        ImGui::TextWrapped(T("%s"),area_name(shown));
        if(ImGui::BeginChild("lit lamps",ImVec2(list_width,size.y))) for(int i=0;i<map.lamp_count;++i) {
            const auto& l=map.lamps[i];if(l.map!=shown) continue;ImGui::PushID(i);
            ImGui::BeginDisabled(!l.lit || map.block!=BBCOOP_MAP_GO);
            if(ImGui::Selectable(s.lamp_names[i],hover_lamp==i,ImGuiSelectableFlags_None,ImVec2(0,32*scale))) travel_ask=l.warp_point;
            ImGui::EndDisabled();ImGui::PopID();
        } ImGui::EndChild();ImGui::EndGroup();
        if(view && (!chart || (view==2 && !mesh))) ImGui::TextWrapped(T("%s"),BbMapCharts::Status(shown,chart && view==2).c_str());
        BbCoopStyle::Guide({{"LeftStick","W A S D","Move"},{"RightStick","Z X","Turn"},{"L2 R2","- =","Zoom"},{"L1 R1","Q E","Area"},{"Triangle","Tab","All areas"},{"Square",nullptr,"Ping"},{"L3","V","3D view"},{"R3","F","Floors"},{"Options","C","Your location"},{"Circle","Esc","Back"}});
        if(travel_ask) ImGui::OpenPopup(Popup("Travel to lamp?"));
        if(ImGui::BeginPopupModal(Popup("Travel to lamp?"),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            for(int i=0;i<map.lamp_count;++i) if(map.lamps[i].warp_point==travel_ask) ImGui::TextUnformatted(s.lamp_names[i]);
            if(BbCoopStyle::Button("Travel") || (asked_before && Pressed({ImGuiKey_GamepadFaceDown,ImGuiKey_Enter}))) {Send(BB_COOP_TRAVEL,travel_ask);travel_ask=0;ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(BbCoopStyle::Button("Cancel") || (asked_before && Pressed({ImGuiKey_GamepadFaceRight,ImGuiKey_Escape}))) {travel_ask=0;ImGui::CloseCurrentPopup();}
        }

    } ImGui::End();if(!open) {travel_ask=0;Send(BB_COOP_MAP_MENU,0);}
}

void InvasionStatus(const bbcoop_invasion& invasion) {
    const bool visitor=invasion.role==BBCOOP_INVASION_INVADER;
    switch(invasion.phase) {
    case BBCOOP_INVASION_SEARCHING:
        ImGui::TextUnformatted(T("Searching for a hunt to invade..."));break;
    case BBCOOP_INVASION_APPROACHING:
        ImGui::Text(T(visitor?"Invading in %d":"An invader approaches: %d"),invasion.seconds);break;
    case BBCOOP_INVASION_ACTIVE:
        ImGui::Text(T(visitor?"Invading the world of %s":"Invaded by %s"),Hunter(invasion.name));break;
    case BBCOOP_INVASION_RETURNING:
        ImGui::TextUnformatted(T("Returning to your world..."));break;
    case BBCOOP_INVASION_ENDED:
        switch(invasion.reason) {
        case BBCOOP_INVASION_INVADER_DIED:ImGui::TextUnformatted(T("Invader defeated"));break;
        case BBCOOP_INVASION_TARGET_KILLED:ImGui::TextUnformatted(T("The host was defeated"));break;
        case BBCOOP_INVASION_PARTY_WIPED:ImGui::TextUnformatted(T("The hunt was defeated"));break;
        case BBCOOP_INVASION_NOT_FOUND:ImGui::TextUnformatted(T("No hunt found for an invasion"));break;
        case BBCOOP_INVASION_REFUSED:ImGui::TextUnformatted(T("The server refused the invasion request"));break;
        default:ImGui::TextUnformatted(T("The invasion ended"));break;
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
            char text[64];std::snprintf(text,sizeof(text),"%s %d%%",T("Down"),int(share*100));
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
        char title[96];std::snprintf(title,sizeof(title),T(down?"You are down":"Spectating %s"),Hunter(s.panel.watching_name));
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
    const auto& prefs=BbSettings::Get();
    const float compass_x=stage.x+stage.w*.5f,compass_y=prefs.compass_position==1?stage.y+stage.h-95*scale:stage.y+35*scale,span=190*scale;
    const ImVec2 ring(compass_x,stage.y+stage.h*.5f);const float radius=190*scale;
    auto compass_at=[&](float x,float bearing) {return prefs.compass_position==2?ImVec2(ring.x+radius*std::sin(bearing),ring.y-radius*std::cos(bearing)):ImVec2(compass_x+std::clamp(x,-1.f,1.f)*span,compass_y);};
    auto arrow=[&](ImVec2 point,float angle,float r,ImU32 ink) {ImVec2 d(std::cos(angle),std::sin(angle)),side(-d.y,d.x);ImVec2 base(point.x+d.x*r,point.y+d.y*r);draw->AddTriangleFilled(ImVec2(base.x+side.x*5*scale,base.y+side.y*5*scale),ImVec2(base.x+d.x*9*scale,base.y+d.y*9*scale),ImVec2(base.x-side.x*5*scale,base.y-side.y*5*scale),ink);};
    if(s.has_compass && prefs.compass) {
        if(prefs.compass_position!=2) {
            draw->AddLine(ImVec2(compass_x-span,compass_y),ImVec2(compass_x+span,compass_y),BbCoopStyle::Muted(.45f),scale);
            for(int tick=-4;tick<=4;++tick) draw->AddLine(ImVec2(compass_x+tick*span/4,compass_y-3*scale),ImVec2(compass_x+tick*span/4,compass_y+(tick%2?3:6)*scale),BbCoopStyle::Muted(.5f),scale);
        }
        for(int i=0;i<std::clamp(s.compass.count,0,4);++i) {
            const auto& m=s.compass.markers[i];auto p=compass_at(m.x,m.bearing);if(prefs.compass_position!=2) p.y+=17*scale+(i%2)*38*scale;
            BbCoopStyle::Face(draw,s,m.board,m.name,p,32*scale,m.member,m.vital==BBCOOP_VITAL_UP);
            if(m.vital==BBCOOP_VITAL_DOWN) BbCoopStyle::Icon(draw,BbPartyIcons::Icon::Downed,ImVec2(p.x-14*scale,p.y-14*scale),28*scale);
            char text[64];std::snprintf(text,sizeof(text),"%s  %.0fm",Hunter(m.name),m.distance);BbCoopStyle::Label(draw,ImVec2(p.x,p.y+19*scale),12*scale,BbCoopStyle::Ink(),text,.5f,135*scale);
            if(m.pinned && prefs.compass_position!=2) arrow(p,m.x<0?Pi:0,18*scale,BbCoopStyle::Ink());
        }
        if(prefs.bloodstains && s.compass.has_stain) draw->AddCircleFilled(compass_at(s.compass.stain.x,s.compass.stain.bearing),4*scale,IM_COL32(200,45,38,255));
    }
    if(prefs.team_pings) for(int i=0;i<std::clamp(s.pings.count,0,4);++i) {
        const auto& p=s.pings.pings[i];if(!p.here) continue;
        const auto ink=BbCoopStyle::Ink(std::min(1.f,p.seconds_left/2.f));
        const float r=8*scale;char text[80];std::snprintf(text,sizeof(text),"%s  %.0fm",Hunter(p.name),p.distance);
        if(p.on_screen) {
            ImVec2 point(stage.x+p.screen_x*stage.w,stage.y+p.screen_y*stage.h);
            if(p.pinned) point.y=std::max(point.y,stage.y+115*scale);
            draw->AddCircle(point,r,ink,24,2*scale);draw->AddCircleFilled(point,r*.3f,ink);
            if(p.pinned) arrow(point,p.arrow,r+5*scale,ink);
            BbCoopStyle::Label(draw,ImVec2(point.x,point.y-r-24*scale),15*scale,ink,text,.5f,220*scale);
        }
        if(prefs.compass && s.has_compass) {
            const auto point=compass_at(p.strip_x,p.bearing);draw->AddCircle(point,r,ink,24,2*scale);
            BbCoopStyle::Label(draw,ImVec2(point.x,point.y-30*scale),13*scale,ink,text,.5f,200*scale);
            if(p.strip_pinned && prefs.compass_position!=2) arrow(point,p.strip_x<0?Pi:0,r+4*scale,ink);
        }
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
            if(!s.lobby.signed_in) ImGui::TextUnformatted(T("Connecting to the hunt..."));
            if(s.has_rot) ImGui::Text(T("Rot %d/%d"),s.rot.count,BBCOOP_ROT_MAX);
            if(s.camera_on) ImGui::TextUnformatted(T("Free camera  -  F7 to return"));
            if(s.skip_want) ImGui::Text(T("Skip cutscene: %d / %d votes"),s.skip_want,s.skip_watching);
            if(*s.notice) ImGui::TextWrapped(T("%s"),s.notice);
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
    if(ImGui::Begin("##hunters entering",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoBackground)) {
        auto* draw=ImGui::GetWindowDrawList();
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
            ImGui::TextWrapped(T("%s"),s.trade_question);if(BbCoopStyle::Button("Accept")) Send(BB_COOP_TRADE_ANSWER,1,"",s.trade_id);ImGui::SameLine();if(BbCoopStyle::Button("Decline")) Send(BB_COOP_TRADE_ANSWER,0,"",s.trade_id);
        } ImGui::End();
    }
    if(s.duel_id) { if(BbCoopStyle::Begin("Team duel invitation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text(T("%s proposes a duel."),Hunter(s.duel_name));if(BbCoopStyle::Button("Accept duel")) Send(BB_COOP_DUEL_ANSWER,1,"",s.duel_id);ImGui::SameLine();if(BbCoopStyle::Button("Decline duel")) Send(BB_COOP_DUEL_ANSWER,0,"",s.duel_id);}ImGui::End(); }
    if(s.has_travel && (s.travel.question || s.travel.waiting)) {if(BbCoopStyle::Begin("Travel with team",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {ImGui::Text(T("%s%s"),s.travel.from,s.travel.waiting?": waiting for the team":": travel together?");if(s.travel.question) {if(BbCoopStyle::Button("Yes")) Send(BB_COOP_TRAVEL_ANSWER,1);ImGui::SameLine();if(BbCoopStyle::Button("No")) Send(BB_COOP_TRAVEL_ANSWER,0);}if(s.travel.seconds_left>0) ImGui::Text(T("%.0f seconds left"),s.travel.seconds_left);}ImGui::End();}
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
    const int ping_key=BbSettings::Get().ping_key;
    if(e.key.scancode==SDL_GetScancodeFromName(BbSettings::PingKeyNames[ping_key]) && !CapturesInput() && BbSettings::Get().team_pings) {Send(BB_COOP_PING);return true;}
    switch(e.key.scancode) {
    case SDL_SCANCODE_F6:Send(BB_COOP_TEAM_MENU,!latest.team_screen);return true;
    case SDL_SCANCODE_F8:Send(BB_COOP_MAP_MENU,!latest.map_screen);return true;
    case SDL_SCANCODE_F7:if(BbSettings::Get().free_camera) Send(BB_COOP_CAMERA);return true;

    case SDL_SCANCODE_ESCAPE:
        if(ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId)) return false;
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
    auto& settings=BbSettings::Get();
    static int last_preferences=-1,last_language=-1;
    const int preferences=int(settings.free_camera)|(int(settings.team_pings)<<1),language=int(settings.menu_language.load());
    if(preferences!=last_preferences || language!=last_language) {BbCoopCommand c{};c.version=BB_COOP_UI_VERSION;c.action=BB_COOP_PREFERENCES;c.value=preferences;c.kind=language;Send(c);last_preferences=preferences;last_language=language;}
    BbCoopStyle::Scope theme;
    trade_capture=trade_open;
    if(!s.map_screen) map_was_open=false;
    if(s.team_screen || s.has_load)
        BbCoopStyle::HeroBackdrop(ImGui::GetBackgroundDrawList(),ImVec2(0,0),ImGui::GetIO().DisplaySize);
    Hud(s);if(s.team_screen) Team(s);if(s.map_screen) Map(s);Trade(s);Questions(s);
}
}
