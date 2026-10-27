// SPDX-License-Identifier: GPL-2.0-or-later
#include "client.h"
#include "touch.h"
#include "../shim/bbport_language.h"
#include "core/bloodborne_re_internal.h"
#include "core/bloodborne_trade.h"
#include "text/bbport_game_art.h"
#include "text/bbport_save_editor.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
namespace Core::Bloodborne { bool NativeSeamlessReady(); }
namespace {
BbSeamlessHost host{};
std::mutex ui_mutex;
std::deque<BbCoopCommand> trades;
BbCoopUi view{};
uint32_t explored_map{};
BbLanguage::MenuLanguage language=BbLanguage::GameLanguage();
std::string notice;
std::chrono::steady_clock::time_point notice_until;
std::vector<uint8_t> ReadText(const char* relative) {
    char path[4096];
    if (!bbcoop_game_file(relative,path,sizeof(path))) return {};
    std::ifstream file(path,std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    const auto unpacked=BbGameArt::InflateDcx(bytes);
    return unpacked ? *unpacked : std::vector<uint8_t>{};
}
std::vector<uint8_t> TextFile(const char* book) {
    const auto path=std::string("dvdroot_ps4/msg/")+BbLanguage::GameFolder(language)+"/"+book+".msgbnd.dcx";
    auto bytes=ReadText(path.c_str());
    if(bytes.empty()) bytes=ReadText((std::string("dvdroot_ps4/msg/engus/")+book+".msgbnd.dcx").c_str());
    return bytes;
}
const std::map<uint32_t,std::string>& Names() {
    static std::map<int,std::map<uint32_t,std::string>> cache;
    auto [it,added]=cache.try_emplace(int(language));
    if(added) it->second=BbSaveEditor::ItemNamesFromGameText(TextFile("item"),TextFile("menu"));
    return it->second;
}
const std::map<uint32_t,std::string>& GameNames(int table) {
    static std::map<std::pair<int,int>,std::map<uint32_t,std::string>> cache;
    auto [it,added]=cache.try_emplace(std::pair{int(language),table});
    if(added) it->second=BbSaveEditor::GameText(TextFile(table==30?"menu":"item"),table);
    return it->second;
}
const auto& LampNames() {return GameNames(30);}
const auto& AreaNames() {return GameNames(19);}
void MapNames() {
    view.area_count=0;view.label_count=0;
    for(const auto& [id,name]:AreaNames()) {
        if(view.label_count==256) break;
        auto& label=view.labels[view.label_count++];label.id=id;
        std::snprintf(label.name,sizeof(label.name),"%s",name.c_str());
    }
    const auto area=[&](uint32_t map) {
        if(!map || view.area_count==48) return;
        for(int i=0;i<view.area_count;++i) if(view.areas[i].map==map) return;
        auto& row=view.areas[view.area_count++];row.map=map;
        const auto found=AreaNames().find(bbcoop_map_area_name_id(map));
        if(found!=AreaNames().end()) std::snprintf(row.name,sizeof(row.name),"%s",found->second.c_str());
        else std::snprintf(row.name,sizeof(row.name),"Area %08x",map);
    };
    area(view.map.map);
    for(int i=0;i<std::clamp<int>(view.map.lamp_count,0,BBCOOP_MAP_LAMPS);++i) {
        const auto& lamp=view.map.lamps[i];area(lamp.map);
        const auto found=LampNames().find(lamp.name_id);
        if(found!=LampNames().end()) std::snprintf(view.lamp_names[i],64,"%s",found->second.c_str());
        else std::snprintf(view.lamp_names[i],64,"Lamp %d",lamp.warp_point);
    }
}
int ItemName(uint32_t id,char* out,size_t size) {
    const auto found=Names().find(id);
    if(found==Names().end()) return 0;
    std::snprintf(out,size,"%s",found->second.c_str());return 1;
}
void Notice(const char* text) {
    notice=text;notice_until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
}
void TradeTick() {
    // Called by the co-op tick on the game thread, outside its trade mutex.
    std::scoped_lock lock{ui_mutex};
    for (const auto& command:trades) {
        if(command.action==BB_COOP_TRADE_OFFER) {
            if (!bbcoop_trade_offer_to(command.text,command.kind,command.item,command.count,command.handle))
                Notice("The trade could not be sent. Check the quantity and teammate's location.");
            else Notice("Trade offered. Waiting for your teammate.");
        } else if (command.action==BB_COOP_TRADE_ANSWER) {
            if (!bbcoop_trade_ask_open(command.id)) Notice("That trade has expired.");
            else bbcoop_trade_answer(command.id,command.value!=0);
        }
    }
    trades.clear();
    view.trade_on=bbcoop_trade_on();
    view.partner_count=std::clamp(bbcoop_trade_partner_count(),0,4);
    for(int i=0;i<view.partner_count;++i)
        bbcoop_trade_partner(i,view.partners[i].id,sizeof(view.partners[i].id),
                              view.partners[i].name,sizeof(view.partners[i].name));
    view.inventory_count=0;
    for(const auto& row:Core::Bloodborne::Coop::HeldTradeInventory()) {
        if(!bbcoop_trade_tradeable(row.kind,row.id)) continue;
        auto& out=view.inventory[view.inventory_count++];
        out={row.kind,row.id,row.count,row.handle,{}};
        if(row.kind==1) std::snprintf(out.name,sizeof(out.name),"Blood Echoes");
        else if(!ItemName(row.kind==2?0x40000000u|row.id:row.id,out.name,sizeof(out.name)))
            std::snprintf(out.name,sizeof(out.name),"Item %08x",row.id);
        if(view.inventory_count==BB_COOP_INVENTORY_MAX) break;
    }
    view.trade_id=0;view.trade_question[0]=0;
    bbcoop_trade_ask(&view.trade_id,view.trade_question,sizeof(view.trade_question));
    char text[256];
    if(bbcoop_trade_notice(text,sizeof(text))) Notice(text);
}
void Command(const BbCoopCommand& c) {
    switch(c.action) {
    case BB_COOP_LOBBY: bbcoop_lobby_request(c.value,c.text);break;
    case BB_COOP_CHOOSE: bbcoop_team_choose(c.value);break;
    case BB_COOP_TEAM_MENU: bbcoop_team_menu(c.value?2:0);break;
    case BB_COOP_JOIN_HUNT: bbcoop_team_join_hunt(c.text);break;
    case BB_COOP_MAP_MENU: bbcoop_map_menu(c.value?2:0);break;
    case BB_COOP_TRAVEL: bbcoop_map_travel(c.value);break;
    case BB_COOP_PING: bbcoop_ping_aim();break;
    case BB_COOP_MAP_PING: {
        const bbcoop_ping_area area{c.map,-1e6f,1e6f,-1e6f,1e6f,0,0};
        bbcoop_ping_map(&area,1,c.map,c.x,c.z,c.y);break;
    }
    case BB_COOP_TRADE_OFFER: case BB_COOP_TRADE_ANSWER:
        if(trades.size()<16) trades.push_back(c);break;
    case BB_COOP_TRAVEL_ANSWER: bbcoop_team_travel_answer(c.value!=0);break;
    case BB_COOP_DUEL_PROPOSE: bbcoop_duel_propose();break;
    case BB_COOP_DUEL_CANCEL: bbcoop_duel_cancel();break;
    case BB_COOP_DUEL_ANSWER: bbcoop_duel_answer(static_cast<uint32_t>(c.id),c.value!=0);break;
    case BB_COOP_INVASION: bbcoop_invade(c.value!=0);break;
    case BB_COOP_ALLOW_INVADERS: bbcoop_team_allow_invaders(c.value!=0);break;
    case BB_COOP_CAMERA: bbcoop_free_camera_toggle();break;
    case BB_COOP_PINGS: bbcoop_set_team_pings(c.value!=0);break;
    case BB_COOP_MAP_AREA: explored_map=c.map;break;
    case BB_COOP_PREFERENCES:
        bbcoop_set_free_camera((c.value&1)!=0);bbcoop_set_team_pings((c.value&2)!=0);
        language=static_cast<BbLanguage::MenuLanguage>(std::clamp<int>(c.kind,0,BbLanguage::MenuLanguageCount-1));break;
    default: break;
    }
}
}
void BbSeamlessUiInit(const BbSeamlessHost& supplied) {
    host=supplied;
    bbcoop_set_free_camera(0);
}
extern "C" int bbseamless_install(unsigned char* image,uint64_t size,void* code,
                                   uint64_t code_size,const char* app0) {
    bbcoop_install(image,size,code,code_size,app0);
    if(!Core::Bloodborne::NativeSeamlessReady()) return -1;
    bbcoop_trade_set_namer(ItemName);
    (void)Names();
    (void)LampNames();(void)AreaNames();
    bbcoop_trade_set_ui_tick(TradeTick);
    return 0;
}
extern "C" void bbseamless_frame(uint32_t* buttons,uint8_t sticks[4],uint8_t* l2,uint8_t* r2,int32_t* touch_side) {
    std::scoped_lock lock{ui_mutex};
    if(host.next_command) {
        BbCoopCommand c{};
        for(int n=0;n<64 && host.next_command(&c);++n) {
            c.text[sizeof(c.text)-1]=0;
            if(c.version==BB_COOP_UI_VERSION) Command(c);
        }
    }
    uint32_t auto_buttons=0;uint8_t auto_sticks[4];
    if(bbcoop_autoplay_pad(&auto_buttons,auto_sticks)) {
        *buttons=auto_buttons;std::memcpy(sticks,auto_sticks,4);*l2=*r2=0;
    }
    static BbSeamless::TouchHold touch;
    int click=-1;
    const auto microseconds=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int action=0;
    if(!bbcoop_map_screen() && !bbcoop_team_screen())
        action=touch.Step((*buttons&0x100000)!=0,*touch_side,microseconds,click);
    *buttons&=~0x100000u;
    if(click>=0) {*buttons|=0x100000;*touch_side=click;}
    if(action==1) bbcoop_map_from_pad();
    else if(action==2) bbcoop_ping_aim();
    if(bbcoop_team_menu_back()) *buttons|=0x2000; // Circle
    if(bbcoop_cutscene_skip()) *buttons|=8; // Options
    else if(const int vote=bbcoop_cutscene_vote_pad((*buttons&8)!=0)) {
        *buttons&=~8u;if(vote==2) *buttons|=8;
    }
    bbcoop_party_death_pad(buttons,sticks,l2,r2);
    bbcoop_free_camera_pad(buttons,sticks,l2,r2);
    static auto next=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now<next) return;
    static std::array<uint8_t,BB_COOP_MODEL_BYTES> model;
    if(host.take_portrait && host.take_portrait(model.data(),model.size()))
        bbcoop_portrait_model(model.data(),BB_COOP_MODEL_WIDTH,BB_COOP_MODEL_HEIGHT);
    next=now+std::chrono::milliseconds(100);
    view.version=BB_COOP_UI_VERSION;view.enabled=1;++view.sequence;
    bbcoop_lobby_view(&view.lobby);
    view.team_screen=bbcoop_team_screen();view.map_screen=bbcoop_map_screen();
    view.has_panel=bbcoop_party_panel(&view.panel);
    view.has_compass=bbcoop_compass_view(&view.compass);
    view.has_map=bbcoop_map_view(&view.map,view.map_screen);
    if(view.map_screen) {
        MapNames();
        view.explored_map=explored_map?explored_map:view.map.map;
        view.explored_count=bbcoop_map_explored(view.explored_map,view.explored,8192);
        static unsigned next_area=0;view.explored_other_map=view.area_count?view.areas[next_area++%view.area_count].map:0;
        view.explored_other_count=bbcoop_map_explored(view.explored_other_map,view.explored_other,8192);
    }
    view.has_pings=bbcoop_pings_view(&view.pings);
    view.has_load=bbcoop_team_load_view(&view.load);
    view.has_rot=bbcoop_rot_view(&view.rot);
    view.has_travel=bbcoop_team_travel_view(&view.travel);
    view.has_invasion=bbcoop_invasion_view(&view.invasion);
    bbcoop_duel_view(&view.duel);
    view.duel_id=0;view.duel_name[0]=0;
    bbcoop_duel_ask(&view.duel_id,view.duel_name,sizeof(view.duel_name));
    view.skip_want=view.skip_watching=0;
    bbcoop_cutscene_vote(&view.skip_want,&view.skip_watching);
    view.camera_on=bbcoop_free_camera_on();
    view.want_portrait=bbcoop_portrait_offscreen_wanted();view.portrait_count=0;
    for(int i=0;i<std::clamp(view.lobby.member_count,0,4);++i) {
        const auto& member=view.lobby.members[i];
        if(!std::strcmp(member.id,view.lobby.me)) continue;
        auto& portrait=view.portraits[view.portrait_count];
        if(bbcoop_member_portrait(member.id,portrait.rgba,sizeof(portrait.rgba),&portrait.id)) {
            std::snprintf(portrait.board,sizeof(portrait.board),"%s",member.id);++view.portrait_count;
        }
    }
    int kind=0;char name[40];
    if(bbcoop_duel_notice(&kind,name,sizeof(name))) {
        const char* reasons[]={"busy","nobody nearby","declined","cancelled","left","moved away"};
        char text[128];std::snprintf(text,sizeof(text),"Duel: %s %s",name,reasons[std::clamp(kind,0,5)]);Notice(text);
    }
    std::snprintf(view.notice,sizeof(view.notice),"%s",now<notice_until?notice.c_str():"");
    view.trade_on=bbcoop_trade_on();
    if(!view.lobby.in_hunt || !view.trade_on) {
        view.inventory_count=0;view.partner_count=0;view.trade_id=0;trades.clear();
    }
    if(host.publish_ui) host.publish_ui(&view);
}
