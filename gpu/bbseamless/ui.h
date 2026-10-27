/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed-width copies only. The arm64 renderer never calls into the x86 game module. */
#pragma once
#include "bbcoop.h"
#define BB_COOP_UI_VERSION 4u
#define BB_COOP_INVENTORY_MAX 512
#define BB_COOP_MODEL_WIDTH 560u
#define BB_COOP_MODEL_HEIGHT 768u
#define BB_COOP_MODEL_BYTES (BB_COOP_MODEL_WIDTH * BB_COOP_MODEL_HEIGHT * 4u)
enum BbCoopAction {
    BB_COOP_LOBBY = 1, BB_COOP_CHOOSE, BB_COOP_TEAM_MENU, BB_COOP_JOIN_HUNT,
    BB_COOP_MAP_MENU, BB_COOP_TRAVEL, BB_COOP_PING, BB_COOP_MAP_PING,
    BB_COOP_TRADE_OFFER, BB_COOP_TRADE_ANSWER, BB_COOP_TRAVEL_ANSWER,
    BB_COOP_DUEL_PROPOSE, BB_COOP_DUEL_CANCEL, BB_COOP_DUEL_ANSWER,
    BB_COOP_INVASION, BB_COOP_ALLOW_INVADERS, BB_COOP_CAMERA, BB_COOP_PINGS, BB_COOP_MAP_AREA, BB_COOP_PREFERENCES,
};
typedef struct BbCoopCommand {
    uint32_t version, action;
    uint64_t id;
    int32_t value;
    uint32_t kind, item, count, handle, map;
    float x, y, z;
    char text[128];
} BbCoopCommand;
typedef struct BbCoopInventoryItem {
    uint32_t kind, id, count, handle;
    char name[96];
} BbCoopInventoryItem;
typedef struct BbCoopUi {
    uint32_t version, enabled;
    uint64_t sequence;
    int32_t team_screen, map_screen, has_panel, has_compass, has_map, has_pings;
    int32_t has_load, has_rot, has_travel, has_invasion, skip_want, skip_watching;
    bbcoop_lobby lobby;
    bbcoop_panel panel;
    bbcoop_compass compass;
    bbcoop_map map;
    uint32_t explored_map;
    int32_t explored_count;
    int16_t explored[8192 * 2];
    uint32_t explored_other_map;
    int32_t explored_other_count;
    int16_t explored_other[8192 * 2];
    char lamp_names[BBCOOP_MAP_LAMPS][64];
    int32_t label_count;
    struct { int32_t id; char name[96]; } labels[256];
    int32_t area_count;
    struct { uint32_t map; char name[64]; } areas[48];
    bbcoop_pings pings;
    bbcoop_team_load load;
    bbcoop_rot rot;
    bbcoop_team_travel travel;
    bbcoop_invasion invasion;
    bbcoop_duel duel;
    uint64_t trade_id;
    uint32_t duel_id;
    char trade_question[512], duel_name[40], notice[256];
    int32_t trade_on, partner_count, inventory_count, camera_on;
    struct { char id[40], name[40]; } partners[4];
    BbCoopInventoryItem inventory[BB_COOP_INVENTORY_MAX];
    int32_t want_portrait, portrait_count;
    struct { char board[40]; uint32_t id; uint8_t rgba[128*128*4]; } portraits[4];
} BbCoopUi;
