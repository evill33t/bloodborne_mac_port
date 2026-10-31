/* SPDX-License-Identifier: GPL-2.0-or-later */
/* bbport co-op: the emulator's seamless co-op (../emulator/src/core/bloodborne_*.cpp) in the
 * native port. What the loader and the C runtime call; gpu/bbcoop/ implements it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A segment of the game's image (its address, size, flags: 1 execute, 2 write, 4 read), for the
 * co-op code's memory checks. Called for each segment before bbcoop_install. */
void bbcoop_note_segment(uint64_t address, uint64_t size, unsigned flags);

/* The co-op patches and hooks go into the eboot (image offset 0, eboot_size bytes) when
 * SHADPS4_BLOODBORNE_SEAMLESS_COOP=1 (SHADPS4_BLOODBORNE_RE_TRACE=1: the trace hooks). Called
 * after relocation and the image patches, while the image is still writable. The hooks'
 * trampolines go to `code`, executable memory within 2 GiB of the image. */
void bbcoop_install(unsigned char *image, uint64_t eboot_size, void *code, uint64_t code_size,
                    const char *app0);

/* A fatal exception (Windows: its EXCEPTION_POINTERS) is ending the process: the crash report
 * (user/log/crash-*.txt and .dmp). */
void bbcoop_crash_report(void *exception_pointers);

/* The window is closing: a game whose thread has stopped ticking writes a freeze report
 * (user/log/freeze-*.txt and .dmp) first. */
void bbcoop_window_closing(void);

/* Autoplay (SHADPS4_BLOODBORNE_AUTOPLAY, the rig's scripted tests): the PS4 pad buttons and the
 * sticks (lx, ly, rx, ry; 128 is centred) it holds now; 0 when it is off. */
int bbcoop_autoplay_pad(uint32_t *buttons, uint8_t sticks[4]);

/* Party death (always on in the port): sees what the game will
 * read from the pad (the give-up hold, attacks that revive a downed member) and, while the player
 * is down, lets go of all but Options and the camera stick. */
void bbcoop_party_death_pad(uint32_t *buttons, uint8_t sticks[4], uint8_t *l2, uint8_t *r2);
/* Kaniel 2026-10-25: the free camera (an opt-in setting, bbport.ini free_camera): the overlay says
 * every frame whether the setting allows it, its F7 toggles it, and the pad reader hands it the
 * pad after party death's (Square with L3 held toggles it; while it flies the game gets nothing). */
void bbcoop_set_free_camera(int allowed);
void bbcoop_free_camera_toggle(void);
int bbcoop_free_camera_on(void);
void bbcoop_free_camera_pad(uint32_t *buttons, uint8_t sticks[4], uint8_t *l2, uint8_t *r2);
/* The mouse's motion (counts) while a co-op camera drives the view (spectating, the free camera):
 * 1 when it took it (the game's mouse camera turns nothing then). */
int bbcoop_camera_mouse(float dx, float dy);

/* Party death's panel, for the overlay: the other party members playing it (online id, state,
 * HP, revive progress while down; `here` on this player's map), whom an attack would revive now
 * (an index into members, or -1) and this player's own down. */
enum { BBCOOP_VITAL_OFF, BBCOOP_VITAL_UP, BBCOOP_VITAL_DOWN, BBCOOP_VITAL_DEAD,
       BBCOOP_VITAL_SPECTATE /* Kaniel 2026-10-25: out of the fight, watching its teammates */ };
enum { BBCOOP_PANEL_MEMBERS = 4 };
typedef struct bbcoop_panel_member {
    char name[20]; /* "" when unknown */
    char board[40]; /* native split overlay's portrait key */
    int vital;     /* BBCOOP_VITAL_* */
    int here;
    int32_t hp, hp_max;
    uint32_t landed, needed;
    uint32_t vials, bullets; /* Kaniel 2026-10-18: beside the name */
} bbcoop_panel_member;
typedef struct bbcoop_panel {
    int member_count;
    bbcoop_panel_member members[BBCOOP_PANEL_MEMBERS];
    int revive_target;
    int vital; /* this player's */
    uint32_t landed, needed;
    float give_up; /* the give-up hold, 0 to 1 */
    int native_list; /* the game's HUD shows the party list (the party HUD's CoopParty tags) */
    int native_prompt; /* the game's HUD shows the prompt (CoopPrompt): the overlay draws its glyph only */
    int native_supplies; /* the game's HUD shows the vials and bullets (CoopSupply): the overlay draws none */
    /* Kaniel 2026-10-25: while this player spectates (vital BBCOOP_VITAL_SPECTATE), the teammate
     * it watches (UTF-8; "" a Hunter of unknown name), `watching` 0 while nobody stands to watch. */
    int watching;
    char watching_name[40];
} bbcoop_panel;
/* Fills `panel`; 0 when there is nothing to draw (party death off, no party session, nobody else
 * on the vitals board and this player not down). */
int bbcoop_party_panel(bbcoop_panel *panel);
/* Kaniel 2026-10-17: the teammate compass. The party members on this player's map as the camera
 * sees them: bearing in radians (0 ahead, positive to the right), distance in metres, `x` the
 * place on the strip (-1 its left end, 1 its right end; `pinned`: past the strip's span, at its
 * end) and `member` the index into the party panel's members (its colour). 0 when there is
 * nothing to draw: no party session, nobody here, the camera not found, the game's HUD not up (a
 * load, a cutscene). */
enum { BBCOOP_COMPASS_MARKERS = 4 };
typedef struct bbcoop_compass_marker {
    char name[20]; /* the panel's name, "" when unknown */
    int member;
    float bearing, distance, x;
    int pinned;
    /* Kaniel 2026-10-17: its board name (bbcoop_member_portrait's key), BBCOOP_VITAL_* and its
     * revive's progress (0 to 1) while down. Downed markers come last (drawn over the others). */
    char board[40];
    int vital;
    float revive;
} bbcoop_compass_marker;
/* Kaniel 2026-10-20: this player's own bloodstain on this map, placed as the markers (only its
 * own: Kaniel after v1.0.2). */
typedef struct bbcoop_compass_stain {
    float bearing, distance, x;
    int pinned;
} bbcoop_compass_stain;
typedef struct bbcoop_compass {
    float heading; /* the camera's, for the strip's ticks */
    int count;
    bbcoop_compass_marker markers[BBCOOP_COMPASS_MARKERS];
    int has_stain;
    bbcoop_compass_stain stain;
} bbcoop_compass;
int bbcoop_compass_view(bbcoop_compass *compass);
/* Kaniel 2026-10-17: the map screen (an Elden Ring style map). What it shows as the game thread
 * read it last while `open` (the screen is up; the game is read only then): this player's map and
 * where it stands (the game's metres on that map: x east, y up, z north; the camera's heading as
 * the compass's), the party members (on this map: where they stand), every lamp (`lit` for this
 * character; names by the game's text ids: menu FMG 30, the area's item FMG 19) and what would
 * stop a trip now (BBCOOP_MAP_*: GO when nothing would). 0 before the first reading. */
enum { BBCOOP_MAP_LAMPS = 80, BBCOOP_MAP_MEMBERS = 4 };
enum { BBCOOP_MAP_GO, BBCOOP_MAP_NOT_LIT, BBCOOP_MAP_NOT_IN_WORLD, BBCOOP_MAP_DOWN, BBCOOP_MAP_BOSS,
       BBCOOP_MAP_TRAVELLING };
typedef struct bbcoop_map_lamp {
    int32_t warp_point;
    uint32_t map;
    float x, y, z;
    int lit;
    int32_t name_id;
} bbcoop_map_lamp;
typedef struct bbcoop_map_member {
    char name[20];  /* the panel's name */
    char board[40]; /* bbcoop_member_portrait's key */
    uint32_t map;   /* 0 when not known */
    int has_at;
    float x, y, z;
    int vital; /* BBCOOP_VITAL_* */
} bbcoop_map_member;
/* Kaniel 2026-10-20: this player's own bloodstain on its map (any map), with its echoes (only its
 * own: Kaniel after v1.0.2). */
typedef struct bbcoop_map_stain {
    uint32_t map;
    float x, y, z;
    int32_t echoes;
} bbcoop_map_stain;
typedef struct bbcoop_map {
    int in_world;
    uint32_t map;
    int32_t area_name_id;
    int has_me;
    float x, y, z;
    int has_heading;
    float heading;
    int block; /* BBCOOP_MAP_*, for a lit lamp */
    int member_count;
    bbcoop_map_member members[BBCOOP_MAP_MEMBERS];
    int lamp_count;
    bbcoop_map_lamp lamps[BBCOOP_MAP_LAMPS];
    /* Kaniel 2026-10-18: the areas whose main bosses are down (the map shows all of them). */
    int cleared_count;
    uint32_t cleared[24];
    int has_stain;
    bbcoop_map_stain stain;
} bbcoop_map;
int bbcoop_map_view(bbcoop_map *map, int open);
/* Kaniel 2026-10-24: team pings. The pings shown (one per team member, 20 s each) as the game
 * thread read them last: the sender's character name ("" when unknown: Hunter), `own` this
 * player's, its spot (the game's metres on `map`), `enemy` 1 while it follows an enemy,
 * `from_map` placed on the world map. On this player's map (`here`): its distance, its bearing
 * from the camera (as the compass's markers: `strip_x` / `strip_pinned` its place on the strip)
 * and, while the game's HUD is up, where it shows on the screen (`screen_x`, `screen_y`: fractions
 * of the game's picture; `pinned` at its edge, `arrow` the way it lies, radians on the screen, 0
 * right). `has_heading`: the camera's heading is known (the compass strip's ticks). 0 when there
 * is none. */
enum { BBCOOP_PINGS = 4 };
typedef struct bbcoop_ping {
    char name[40];
    int own;
    uint32_t map;
    float x, y, z;
    int enemy, from_map;
    int here;
    float distance, bearing, strip_x;
    int strip_pinned;
    int on_screen;
    float screen_x, screen_y;
    int pinned;
    float arrow;
    float seconds_left;
} bbcoop_ping;
typedef struct bbcoop_pings {
    int has_heading;
    float heading;
    int count;
    bbcoop_ping pings[BBCOOP_PINGS];
} bbcoop_pings;
int bbcoop_pings_view(bbcoop_pings *pings);
/* A ping where the camera aims (the locked-on enemy while locked on: it follows it), placed by the
 * game thread (the pad's touchpad double tap, the ping key). */
void bbcoop_ping_aim(void);
/* A ping on the map screen: the spot under its cursor on the atlas (`atlas_x`, `atlas_z`) is on
 * the area drawn there (`areas`: each area's picture on the atlas and its world's place there;
 * `shown` wins where two overlap), at height `y`. 1 when it was asked for (0: on no area). */
typedef struct bbcoop_ping_area {
    uint32_t map;
    float x0, x1, z0, z1, dx, dz;
} bbcoop_ping_area;
int bbcoop_ping_map(const bbcoop_ping_area *areas, int count, uint32_t shown, float atlas_x,
                    float atlas_z, float y);
/* The Insert menu's Team pings: off, none is placed, shown or heard. */
void bbcoop_set_team_pings(int on);
/* A ping's words beside its marker: the name and its distance (`has_distance`), else the area it
 * is in (`area`, may be ""). */
void bbcoop_ping_label(const char *name, int has_distance, float distance, const char *area,
                       char *out, size_t size);
/* Where a map ping's words go (screen pixels): under its mark at (`x`, `y`) of radius `r`, else
 * over, right or left of it, off the place names drawn there (`taken`: `count` boxes of x0, y0, x1,
 * y1); the words' box into `out` (x0, y0, x1, y1). */
void bbcoop_ping_label_place(float x, float y, float r, float width, float height,
                             const float *taken, int count, float gap, float out[4]);
/* A trip from the map to the lamp (a warp point of the view's lamps), started by the game thread
 * if nothing stops it. */
void bbcoop_map_travel(int32_t warp_point);
/* Kaniel 2026-10-18 (fog of war): the cells this character has walked on a map, BBCOOP_MAP_CELL
 * metres square, as (x, z) cell pairs into `xz` (2 * `max` values); returns how many. */
enum { BBCOOP_MAP_CELL = 8 };
int bbcoop_map_explored(uint32_t map, int16_t *xz, int max);
/* An area's name id (item FMG 19) by its packed map. */
int32_t bbcoop_map_area_name_id(uint32_t map);
/* The map screen opens (1 from the game's System menu, 2 from the port's Insert menu or its key)
 * and closes (0); opened from the System menu, its page closes with it (bbcoop_team_menu_back
 * presses Circle a moment later). bbcoop_map_screen: 1 while it is open. */
void bbcoop_map_menu(int open);
int bbcoop_map_screen(void);
/* Kaniel 2026-10-24: how far the HUD's groups are moved toward the screen's edges on another aspect
 * ratio than 16:9 (BbGuestHooks::HudEdgeShift, the HUD stage's pixels): the native party list
 * follows the player's bars. Any thread. */
void bbcoop_set_hud_shift(float dx, float dy);
/* Kaniel 2026-10-18: the pad's touchpad held: the map opens (as from the Insert menu) while the
 * player is in the world. */
void bbcoop_map_from_pad(void);
/* A member's portrait by its board name: round, BBCOOP_PORTRAIT_SIDE square RGBA into `rgba`
 * (`size` bytes). Its id (changes with the picture, 0 none) into `id`; 1 when it has one. */
enum { BBCOOP_PORTRAIT_SIDE = 128 };
int bbcoop_member_portrait(const char *member, uint8_t *rgba, size_t size, uint32_t *id);
/* A revive's progress, 0 to 1: the share of the damage it needs that has landed (`landed`
 * and `needed` from the panel). */
float bbcoop_revive_share(uint32_t landed, uint32_t needed);
/* Where a file of the player's game folder (app0) is on this PC, `relative` like
 * "dvdroot_ps4/menu/common.tpf.dcx"; 0 before the game is installed or when `out` is too small. */
int bbcoop_game_file(const char *relative, char *out, size_t size);

/* Party portraits (the co-op code's bloodborne_portrait.h). The texture cache read back the status
 * page's model (its render target, `width` x `height` RGBA rows): this character's portrait once
 * the page closes. */
void bbcoop_portrait_model(const uint8_t *rgba, int width, int height);
/* The party list's portrait slot `slot` (0-2, the party HUD's CoopPartyFace): its version, which
 * changes whenever its picture does; with the picture (64x64 RGBA into `rgba`, `size` bytes) 1, 0
 * when it shows none. */
uint32_t bbcoop_portrait_version(int slot);
/* The off-screen status model is drawn (TickPortraits): read the 560x768 model target back as it
 * is drawn into, not only when the status page samples it. */
int bbcoop_portrait_offscreen_wanted(void);
int bbcoop_portrait_slot(int slot, uint8_t *rgba, size_t size, uint32_t *version);

/* The game opens a file (its guest path): the party code notes the character's save slot. */
void bbcoop_file_opened(const char *guest_path);

/* The game unlocked a trophy itself; party members get it too. */
void bbcoop_trophy_unlocked(int32_t trophy_id);

/* Phase 5, the team lobby (always on with co-op), for the game's own menu
 * (the team screen, bbport_overlay.cpp). Names are UTF-8; `id` is what a request names (a
 * player's lobby member, a team's id). */
enum { BBCOOP_LOBBY_TEAM = 4, BBCOOP_LOBBY_PLAYERS = 16, BBCOOP_LOBBY_INVITES = 8,
       BBCOOP_LOBBY_WORLDS = 16, BBCOOP_LOBBY_FRIENDS = 32, BBCOOP_LOBBY_REQUESTS = 8,
       BBCOOP_LOBBY_SEARCH = 20, BBCOOP_LOBBY_OPEN = 12, BBCOOP_LOBBY_ASKS = 8 };
enum { BBCOOP_LOBBY_OFFLINE, BBCOOP_LOBBY_TITLE, BBCOOP_LOBBY_PLAYING };
/* Amanises 2026-10-25: what a listed player is to this one (LobbyPlayer.Relation). */
enum { BBCOOP_FRIEND_NONE, BBCOOP_FRIEND_YES, BBCOOP_FRIEND_SENT, BBCOOP_FRIEND_RECEIVED,
       BBCOOP_FRIEND_SELF };
typedef struct bbcoop_lobby_entry {
    char name[40];
    char id[40];
    int state;    /* BBCOOP_LOBBY_*: a player's or a member's */
    int ready;    /* a member's */
    int relation; /* BBCOOP_FRIEND_*: a player's, a friend's, a search result's */
    int in_my_team; /* a friend's or a search result's: in this player's team already */
} bbcoop_lobby_entry;
/* Amanises 2026-10-25: an open team as a player in no team sees it (`id` its team, `name` its
 * leader's). */
typedef struct bbcoop_lobby_open {
    char name[40];
    char id[40];
    int members;
    int started; /* its hunt is on */
    int asked;   /* this player asked to join it */
    int open;    /* anyone joins at once (else its leader takes the ones who ask) */
} bbcoop_lobby_open;
/* Kaniel 2026-10-17: one of the server's worlds (each kept apart on the server; any team plays
 * any of them). */
typedef struct bbcoop_lobby_world {
    char id[40];
    char name[64]; /* "Kaniel's world" */
    char when[32]; /* when it was last played, local time ("16 Oct 19:12"), "" when not known */
    int lamps;     /* its lamps lit (how far it is) */
    /* Kaniel 2026-10-17 (the list as the game's Load Game): when it was made ("14 Oct 2026", ""
     * when not known), the characters that played in it ("Kaniel, Ada"), and 1 while another team
     * plays it (it cannot be picked then). */
    char created[32];
    char characters[160];
    int busy;
} bbcoop_lobby_world;
typedef struct bbcoop_lobby {
    int signed_in; /* the server answered */
    int in_team, leader, started;
    int member_count, player_count, invite_count;
    bbcoop_lobby_entry members[BBCOOP_LOBBY_TEAM];       /* the leader first */
    bbcoop_lobby_entry players[BBCOOP_LOBBY_PLAYERS];    /* others signed in, not in this team */
    bbcoop_lobby_entry invites[BBCOOP_LOBBY_INVITES];    /* name: who invited, id: the team */
    char notice[96];                                      /* the server's last refusal */
    char me[40];                                          /* this player's id */
    /* Kaniel 2026-10-16: the screen opened from the game's System menu (not the title's load),
     * a save playing, and playing in the team's world. */
    int menu, playing, in_hunt;
    /* Kaniel 2026-10-17: the server's worlds, the last played first, and the one the team's next
     * Start plays (before a team: the one its team will take): an index into them, -1 a new
     * world. */
    int world_count, world;
    bbcoop_lobby_world worlds[BBCOOP_LOBBY_WORLDS];
    /* The team's code ("K7M 2QX"), for its leader to hand out to a friend the lobby does not list
     * (a hosted server lists 32 players); "" for the others. */
    char code[16];
    /* Invasions (docs/INVASIONS.md): the team's leader allows invaders into its world. */
    int allow_invaders;
    /* Kaniel 2026-10-25: the host rules, the team's (its leader's launcher's); before a team,
     * this player's own launcher's. */
    int rule_rot, rule_travel, rule_vote, rule_scale;
    /* Kaniel 2026-10-28: the boss tempo (0 Vanilla, 1 Aggressive, 2 Relentless). */
    int rule_tempo;
    /* Kaniel 2026-10-25: the team's death mode, the leader's pick (BBCOOP_DEATH_*). */
    int death_mode;
    /* Amanises 2026-10-25: friends (online first), the friend requests waiting for an answer, the
     * ones sent, and the last player search (its text and what it found, online or not). */
    int friend_count, request_count, sent_count, search_count;
    bbcoop_lobby_entry friends[BBCOOP_LOBBY_FRIENDS];
    bbcoop_lobby_entry requests[BBCOOP_LOBBY_REQUESTS];
    bbcoop_lobby_entry sent[BBCOOP_LOBBY_REQUESTS];
    bbcoop_lobby_entry search[BBCOOP_LOBBY_SEARCH];
    char search_query[40];
    /* Amanises 2026-10-25, open teams: the most a team holds (the server's), the leader's team
     * listed to all, the open teams (to a player in no team), who asked to join (to a leader). */
    int team_size;
    int join; /* BBCOOP_JOIN_*: how the team is joined (its leader says) */
    int open_count, ask_count;
    bbcoop_lobby_open open_teams[BBCOOP_LOBBY_OPEN];
    bbcoop_lobby_entry asks[BBCOOP_LOBBY_ASKS];
} bbcoop_lobby;
enum { BBCOOP_DEATH_DOWN = 0, BBCOOP_DEATH_SPECTATE = 1 };
/* Fills `lobby`; 0 when the team lobby is off. */
int bbcoop_lobby_view(bbcoop_lobby *lobby);
/* Kaniel 2026-10-25, the Rot host rule: this player's rot for the party HUD, each penalty's kind
 * (BBCOOP_ROT_*), the oldest first; 0 while the rule is off or there is none. */
enum {
    BBCOOP_ROT_MAX = 5,
    BBCOOP_ROT_MAX_HP = 1,
    BBCOOP_ROT_MAX_STAMINA = 2,
    BBCOOP_ROT_STAMINA_USE = 3,
    BBCOOP_ROT_RALLY = 4,
    BBCOOP_ROT_DISCOVERY = 5
};
typedef struct bbcoop_rot {
    int count;
    int kinds[BBCOOP_ROT_MAX];
} bbcoop_rot;
int bbcoop_rot_view(bbcoop_rot *rot);
/* Kaniel 2026-10-25, the travel together host rule: the question over the game. `question` 1:
 * this player's trip, with the team or alone; 2: `from`'s trip for the team (the vote), Yes or
 * No within `seconds_left`. `waiting`: this player's trip waits for the team's vote. `point` the
 * trip's warp point, `lamp_name_id` its lamp's name (item FMG; -1 none). `notice` 1: `from` said
 * No to this player's trip; 2: `from`'s trip is off. 0 when there is nothing to show. */
typedef struct bbcoop_team_travel {
    int question;
    int32_t point;
    int32_t lamp_name_id;
    char from[40];
    float seconds_left;
    int waiting;
    int notice;
} bbcoop_team_travel;
int bbcoop_team_travel_view(bbcoop_team_travel *travel);
/* The answer to the question shown: 1 Yes (with the team), 0 No (alone). */
void bbcoop_team_travel_answer(int yes);
/* The menu's requests (TeamRequest actions): create, leave, invite a player (`id`), accept or
 * decline an invite (`id`: its team), remove a member (`id`). */
enum { BBCOOP_TEAM_CREATE = 1, BBCOOP_TEAM_INVITE = 2, BBCOOP_TEAM_ACCEPT = 3,
       BBCOOP_TEAM_DECLINE = 4, BBCOOP_TEAM_LEAVE = 5, BBCOOP_TEAM_REMOVE = 6,
       BBCOOP_TEAM_WORLD = 10 /* the leader picks the world (`id`; "" a new one) */,
       BBCOOP_TEAM_FORGET_WORLD = 11 /* the leader forgets a world (`id`) */,
       BBCOOP_TEAM_JOIN_CODE = 12 /* join the team of a typed code (`id`) */,
       BBCOOP_TEAM_NEW_CODE = 13 /* the leader: a fresh code, the old one joins no more */,
       BBCOOP_TEAM_INVITE_NAME = 14 /* invite the player of a typed name (`id`) */,
       BBCOOP_TEAM_DEATH_MODE = 18 /* the leader picks the team's death mode (`id`: "0" or "1") */,
       /* Amanises 2026-10-25, friends: `id` a player's id; SEARCH's `id` is the text to find
        * ("" clears the results). */
       BBCOOP_TEAM_FRIEND_ADD = 19 /* ask, or accept its request */,
       BBCOOP_TEAM_FRIEND_REMOVE = 20 /* unfriend, or take a sent request back */,
       BBCOOP_TEAM_FRIEND_DECLINE = 21, BBCOOP_TEAM_SEARCH = 22,

       /* Amanises 2026-10-25, open teams: OPEN's `id` is a BBCOOP_JOIN_* ("0" invite only,
        * "1" ask, "2" open); ASK_JOIN (an open team: joins it) and ASK_CANCEL name a team,
        * ASK_ACCEPT and ASK_DECLINE the one who asked. */
       BBCOOP_TEAM_OPEN = 23, BBCOOP_TEAM_ASK_JOIN = 24, BBCOOP_TEAM_ASK_CANCEL = 25,
       BBCOOP_TEAM_ASK_ACCEPT = 26, BBCOOP_TEAM_ASK_DECLINE = 27,
       /* Provi 2026-10-29: the leader puts out every lamp of a world (`id`) not played now. */
       BBCOOP_TEAM_RESET_LAMPS = 28 };
enum { BBCOOP_JOIN_INVITE_ONLY = 0, BBCOOP_JOIN_ASK = 1, BBCOOP_JOIN_OPEN = 2 };
void bbcoop_lobby_request(int action, const char *id);
/* The title holds a save's load for the team: the team screen is up (bbport_overlay.cpp). */
int bbcoop_team_screen(void);
/* Invasions (docs/INVASIONS.md; the server's Invasions setting, off by default): a player in no
 * team asks to invade a team's world (1) or leaves the search or the invasion (0); a team's leader
 * allows invaders into its world (1) or not (0). The notice as the overlay shows it: 0 when there
 * is nothing to show. */
enum { BBCOOP_INVASION_NONE, BBCOOP_INVASION_TARGET, BBCOOP_INVASION_INVADER };
enum { BBCOOP_INVASION_SEARCHING = 1, BBCOOP_INVASION_APPROACHING, BBCOOP_INVASION_ACTIVE,
       BBCOOP_INVASION_RETURNING, BBCOOP_INVASION_ENDED };
/* How it ended (bbcoop::InvasionEnd::Reason). */
enum { BBCOOP_INVASION_INVADER_DIED = 1, BBCOOP_INVASION_TARGET_KILLED = 2,
       BBCOOP_INVASION_PARTY_WIPED = 3, BBCOOP_INVASION_NOT_FOUND = 9,
       BBCOOP_INVASION_REFUSED = 10 };
typedef struct bbcoop_invasion {
    int role;    /* BBCOOP_INVASION_TARGET (this team's world) or _INVADER */
    int phase;   /* BBCOOP_INVASION_SEARCHING .. _ENDED */
    int seconds; /* the countdown while approaching */
    int reason;  /* once ended */
    char name[40]; /* the other side: the invader's name, or the hunted host's */
} bbcoop_invasion;
void bbcoop_invade(int on);
void bbcoop_team_allow_invaders(int allow);
int bbcoop_invasion_view(bbcoop_invasion *view);
/* Kaniel 2026-10-16: the team menu in game. The System menu's "Co-op Team" page opens the team
 * screen over the game (1) and its buttons close it (0); a moment after it closed the pad presses
 * Circle for the game (1 while held) so the page under it closes too. The port's Insert menu opens
 * it with 2: no page of the game under it, no Circle. "Join the hunt": this save
 * into its team's world where it stands (`team`: an invitation accepted first, or "" in a team). */
void bbcoop_team_menu(int open);
int bbcoop_team_menu_back(void);
/* Kaniel 2026-10-17: the rig's team-worlds verb asks the team screen to open its world list: 1
 * once. */
int bbcoop_team_world_list_asked(void);
void bbcoop_team_join_hunt(const char *team);
/* Kaniel 2026-10-14: the team loading screen (League of Legends): each hunter's stage and bar,
 * up from the team's Start until every member is in the team's session. 0 when it is not up. */
enum { BBCOOP_LOAD_WAITING, BBCOOP_LOAD_LOADING, BBCOOP_LOAD_JOINING, BBCOOP_LOAD_READY };
typedef struct bbcoop_team_load_row {
    char name[40];
    int stage; /* BBCOOP_LOAD_* */
    int percent;
    int self;
} bbcoop_team_load_row;
typedef struct bbcoop_team_load {
    int count;
    bbcoop_team_load_row rows[4];
} bbcoop_team_load;
int bbcoop_team_load_view(bbcoop_team_load *view);
/* Kaniel 2026-10-15: while that screen is up the game's sound is held (runtime_audio.c reads this
 * every buffer): 1 held. */
int bbcoop_sound_held(void);
/* And a cutscene under it is skipped: 1 while Options is to be held down for the game's skip
 * (runtime_pad.c, even while the screen takes the pad). */
int bbcoop_cutscene_skip(void);
/* Kaniel 2026-10-17: under a cutscene every party member on the map sees, Options is a vote
 * (runtime_pad.c, `options`: the player holds it): 0 the player's own, 1 held back (others still
 * watch), 2 pressed for the game's skip (everyone watching voted). */
int bbcoop_cutscene_vote_pad(int options);
/* The vote as the overlay shows it: 1 while some of those watching want to skip. */
int bbcoop_cutscene_vote(int *want, int *watching);
/* Its buttons: the leader's start (a member is ready by itself, Kaniel 2026-10-20), or, in no
 * team, this save plays alone. */
enum { BBCOOP_TEAM_GO_START = 1, BBCOOP_TEAM_GO_ALONE = 2 };
void bbcoop_team_choose(int what);

/* Kaniel 2026-10-25: team duels (the co-op code's bloodborne_duel.h). The team screen proposes one
 * (to the members on this map) or ends the one this player is in; the asked members answer a yes/no
 * box in the world; the overlay shows the countdown. Any thread. */
enum { BBCOOP_DUEL_NONE, BBCOOP_DUEL_ASKED, BBCOOP_DUEL_ANSWERED, BBCOOP_DUEL_COUNTDOWN,
       BBCOOP_DUEL_FIGHTING, BBCOOP_DUEL_OUT };
typedef struct bbcoop_duel {
    int part;           /* this player's: BBCOOP_DUEL_* */
    int may_propose;    /* someone else of the party is on this map and no duel is under way */
    int may_cancel;     /* this player is in the duel under way */
    float seconds_left; /* the question's or the countdown's */
    int fighters;       /* duelists still standing */
    /* Kaniel 2026-10-25: the countdown as shown: up, its number (3, 2, 1), the bar's fill (0 to 1)
     * and the fight's start once it is over. */
    int countdown_shown, countdown_number, countdown_fight;
    float countdown_fill;
} bbcoop_duel;
void bbcoop_duel_view(bbcoop_duel *duel);
void bbcoop_duel_propose(void);
void bbcoop_duel_cancel(void);
/* The duel waiting for this player's answer and who proposed it (a character name); 0 when none. */
int bbcoop_duel_ask(uint32_t *id, char *name, size_t size);
int bbcoop_duel_ask_open(uint32_t id);
void bbcoop_duel_answer(uint32_t id, int accept);
/* The next thing to say once: its kind and whose name goes in; 0 when none. */
enum { BBCOOP_DUEL_NOTICE_BUSY, BBCOOP_DUEL_NOTICE_ALONE, BBCOOP_DUEL_NOTICE_DECLINED,
       BBCOOP_DUEL_NOTICE_CANCELLED, BBCOOP_DUEL_NOTICE_LEFT, BBCOOP_DUEL_NOTICE_MOVED };
int bbcoop_duel_notice(int *kind, char *name, size_t size);

/* Trading between party members (the co-op code's bloodborne_trade.h, with
 * always on in the port with the server world): what the port's trade menus (shim/bbport_trade_menu.cpp) need. An
 * item is a kind (1 Blood Echoes with id 0, 2 goods by EquipParamGoods id, 3 an item of its own:
 * a weapon, armor, gem or rune by its full item id, category | id); texts are UTF-8 and `size`
 * bytes at most. Calls from the game's thread. */
int bbcoop_trade_on(void);
/* Whether the item may be given (Kaniel 2026-10-15: any item; not the bell items). */
int bbcoop_trade_tradeable(uint32_t kind, uint32_t id);
uint32_t bbcoop_trade_held(uint32_t kind, uint32_t id);
/* The quantity picker's line for giving the item to the nearest member on this map; 0 when
 * nobody is there to give to. */
int bbcoop_trade_give_question(uint32_t kind, uint32_t id, char *text, size_t size);
/* The Blood Echoes picker's steps for what the player holds now: Up/Down (also its first and
 * lowest number) and the game's other step. */
void bbcoop_trade_echoes_picker(uint32_t *step, uint32_t *big_step);
/* Offers `count` of the item to the nearest member (taken out at once; an item of its own: the
 * inventory entry with `handle`, else the first): 1 when sent. */
int bbcoop_trade_offer(uint32_t kind, uint32_t id, uint32_t count, uint32_t handle);
/* Kaniel 2026-10-16: the giver picks who gets it. The members a trade may go to now (on this map,
 * standing, the nearest first): their count, and member `index`'s id (board name) and name; the
 * question and the offer for one of them (`member`: its id). */
int bbcoop_trade_partner_count(void);
int bbcoop_trade_partner(int index, char *member, size_t member_size, char *name,
                         size_t name_size);
int bbcoop_trade_give_question_to(const char *member, uint32_t kind, uint32_t id, char *text,
                                  size_t size);
int bbcoop_trade_offer_to(const char *member, uint32_t kind, uint32_t id, uint32_t count,
                          uint32_t handle);
/* The oldest offer to this player waiting for an answer: its id and question; 0 when none. */
int bbcoop_trade_ask(uint64_t *id, char *text, size_t size);
/* Whether offer `id` still waits for this player's answer. */
int bbcoop_trade_ask_open(uint64_t id);
/* Accepting takes what was asked for out first: 1 when it was a yes that went. */
int bbcoop_trade_answer(uint64_t id, int accept);
/* The next notice to show, skipping those with nothing to say; 0 when none. */
int bbcoop_trade_notice(char *text, size_t size);
/* Kaniel 2026-10-16 (the save editor's Give): items of the player's own (weapons, armor, gems,
 * runes: full item id, `count` of them) put in as a pickup does, with the game's popup (the
 * game's thread). 0 when trading's game functions are not known. */
int bbcoop_give_own_item(uint32_t item_id, uint32_t count);
/* The menus' tick: the co-op code runs it on the game's thread with its trades. */
void bbcoop_trade_set_ui_tick(void (*tick)(void));
/* Items' names for the boxes (the save editor's, from the game's text): by full item id (goods
 * 0x40000000 | id), 1 when it knows the item. */
void bbcoop_trade_set_namer(int (*namer)(uint32_t item_id, char *text, size_t size));

#ifdef __cplusplus
}
#endif
