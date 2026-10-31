# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lets a party guest finish boss defeat events (full boss loot for every party member).

Every boss defeat event (one that runs 2003:12, the boss kill) ends for a summoned guest at
1003:105 "end the event by multiplayer state" with state 1 (client), before the trophy
(2003:28), the boss item (2003:4 AwardItemLot) and the progress flags. This writes copies of
the map event files where those 1003:105 checks compare against a state that never happens, so
the guest runs the rest of the event too. Hosts and solo players are not affected (they are
never in state 1).

It also patches common.emevd's death events (9400: the first death wakes in the Hunter's Dream,
9404: the Dream after insight, 9421: a kidnapping wakes in the Gaol). They set where a death
wakes the player but end for a client (1003:6 state 1), so a party guest runs them too; the
emulator then takes the guest there (ChooseDeathRespawn). The Hunter's Dream's first visit
(m21_00 event 9401: the cutscene and the first Yharnam lamp, 12417810) ends for a client the
same way, which left a guest's headstone with no place to awaken. Vicar Amelia's skull (m24_00
event 12401803: its cutscene and the night) ended for a client the same way, so only the host saw
the cutscene and the night fell only in its world. The Dream's ritual altars warp into the
chalice dungeon picked at them (m21_00 event 12107100, a slot per altar) only for a non-client,
so a party guest who joined a teammate's dungeon could not enter it; it runs in the guest's own
game only (2000:2 0, below), as does the layer's warp after it (12107200: kept in step, the host's
warp took every teammate in the Dream along).
A chalice dungeon's multiplayer fog walls (m29 event 12907600, the common wall event 7600 again,
started per block) never show: their multiplayer checks never match (Kaniel 2026-10-19).
Its boss doors open for guests too: the SpEffect they ask for (9920, event 0) is no longer skipped
for a client. The Research Hall's door and device (m35) ask for the same effect, which its event 0
skipped for anyone not alive in their own world: no longer skipped either.

Boss fogs (open_boss_fog_for_guests): a white phantom's fog event waited for the host's
"entered" flag, so a party guest could not enter a boss fog first (nor after a world reset). It
no longer waits, and it sets the host's flag too, which starts the boss; the emulator posts that
flag to the host (IsBossFogEntryFlag). Boss entrances triggered by walking into the arena accept
a phantom too.

Items picked up with the action button at a glowing spot (ACTION_PICKUP_EVENTS: the Abandoned
Old Workshop's Small Hair Ornament, Third Umbilical Cord and Old Hunter Bone, and seven more) ended
for a party guest and gave a client nothing; each player now picks up its own copy.

Enemies that rise out of the mist as a player comes near (AMBUSH_EVENTS: the woods on the way
into Hemwick and four more) woke only for an alive player in its own world, the host; a guest's
approach wakes them too.

Rom's lake (LAKE_DIVE_EVENTS, Kaniel 2026-10-29): a white phantom's fog by the edge opened only
once the host had dived, and the drop's 5 s of no death waited for that too, so a guest going over
first died from the fall. Neither waits now, and a guest's dive starts Rom like the host's.

One-time enemies (keep_one_time_drop_checks: the Wandering Nightmares) leave for good once their
event ends, which waits for an alive player with the enemy's drop flag; a client skipped the flag
check, and a party guest reads alive, so the guest's game ended them all with no kill. The skip no
longer matches.

Events are kept in step between the players of a session unless they start with 2000:2 (network
sync) 0, as the first visit does. The Hunter's Dream messengers' gift events (m21_00
12101020-12101028) get that instruction, so one player's gift pick no longer finishes the other's
gift event (the emulator also keeps their flags local, KeepsLocalFlag).

Usage: py -3.12 party_boss_events.py <mods dvdroot_ps4\\event> <game event dir> [<update event dir>]
(a file in a later directory replaces the same file in an earlier one, as the update does)

Every output is checked against the game's file before anything is written (validate: only the
intended instructions changed, every parameter still feeds the same instruction and byte), and
manifest.json beside the outputs lists each file's input and output sha256 and its edits;
make-port-kit.ps1 builds a kit only from files the manifest lists.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import sys
import zlib

NEVER = 0x7F  # no multiplayer state has this value
MAIN_GROUP = 0
UNUSED_GROUP = 7  # AND group 7, which no boss event waits on
LOCAL_PLAYER = 10000
AWARD_ITEM_LOT_INCLUDING_CLIENTS = 36  # 2003:36, "クライアントでもアイテム取得"


def load_dcx(data: bytes) -> bytes:
    return zlib.decompress(data[0x4C:])


def save_dcx(original: bytes, payload: bytes) -> bytes:
    packed = zlib.compress(payload, 9)
    return (original[:0x1C] + struct.pack('>II', len(payload), len(packed)) +
            original[0x24:0x4C] + packed)


class EventPatchError(Exception):
    """A patch that cannot be made, or an output that moved something it was not meant to."""


def note(edits: list | None, event: int, instruction: int, bank: int, ident: int, why: str):
    """Records an instruction a patch changed (by its number in the game's event), for validate
    and the manifest."""
    if edits is not None:
        edits.append({'event': event, 'action': 'change', 'instruction': instruction,
                      'bank': bank, 'id': ident, 'why': why})


def patch_events(emevd: bytes, edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, _, instruction_offset = fields[0], fields[1], fields[2], fields[3]
    args_offset = fields[13]
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        instructions = [struct.unpack_from('<iiqqq', data, instruction_offset + first + k * 32)
                        for k in range(count)]
        if not any(bank == 2003 and ident == 12 for bank, ident, _, _, _ in instructions):
            continue
        for number, (bank, ident, length, offset, _) in enumerate(instructions):
            at = args_offset + offset
            # 2003:4 "award item lot" gives nothing to a client; 2003:36 is the same award
            # "even for a client" (names in common.emedf.dcx), so each member gets the boss item.
            if bank == 2003 and ident == 4:
                struct.pack_into('<i', data, instruction_offset + first + number * 32 + 4,
                                 AWARD_ITEM_LOT_INCLUDING_CLIENTS)
                note(edits, event, number, bank, AWARD_ITEM_LOT_INCLUDING_CLIENTS,
                     'boss item for clients too')
                patched += 1
            if bank == 1003 and ident == 105 and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, number, bank, ident, 'client end never matches')
                patched += 1
            # 4:3 "wait until the player (10000) is character type 0 (alive)" in the main
            # condition group blocks a white phantom forever. Moved into a condition group no
            # instruction waits on, it no longer blocks anyone (a host passes it at once anyway).
            if (bank == 4 and ident == 3 and length >= 12 and data[at] == MAIN_GROUP and
                    struct.unpack_from('<i', data, at + 4)[0] == LOCAL_PLAYER):
                data[at] = UNUSED_GROUP
                note(edits, event, number, bank, ident, 'alive check no longer waited on')
                patched += 1
    return bytes(data), patched


# Events that end for a client (1003:6 state 1) but must run for a party guest, per file.
# 9350 awards insight (SpEffect 4680 once per point; see let_guests_earn_insight).
# m21_00 12105043 wakes the insight messenger and sets the flag its shop opens on (12105045).
# m24_00 12401803 is Vicar Amelia's skull: the host's press plays the cutscene and brings the night
# (2002:6, time of day 2); a guest's copy, kept in step, now follows it (its own press still waits
# for an alive host, 4:3).
# Story cutscenes that ended for a client before they played (audit 2026-10-25, Kaniel: "everyone
# is moved but only the host sees the cutscene"): common 9422 the Hypogean Gaol's arrival after a
# kidnapping (and its point of insight), m25_00 12500000 Cainhurst's arrival, m26_00 12601854 the
# one after the Nightmare of Mensis' boss. The Dream's endings are ENDING_EVENTS.
# m28_00 12800430 is the mummy at the end of Yahar'gul's hall (the action button on its warp
# object 2801500, then 2003:49 to the Lecture Building, m32's 3202959): only the host could
# inspect it and only the host travelled (Provi's day 4, v1.3.1). It is each player's own trip
# (LOCAL_EVENTS).
CLIENT_EVENTS = {
    'common.emevd.dcx': {9350, 9400, 9404, 9421, 9422},
    'm21_00_00_00.emevd.dcx': {9401, 12105043, 12107100},
    'm24_00_00_00.emevd.dcx': {12401803},
    'm25_00_00_00.emevd.dcx': {12500000},
    'm26_00_00_00.emevd.dcx': {12601854},
    'm28_00_00_00.emevd.dcx': {12800430},
}
INSIGHT_EVENT = 9350
INITIALIZE_EVENT = (2000, 0)


def let_guests_earn_insight(emevd: bytes, edits: list | None = None) -> tuple[bytes, int]:
    """Every insight award (a boss found or killed, a place reached) is a map event starting
    common event 9350 with the points (2000:0). Some of those end for a client (1003:6 state 1)
    or unless the local player is alive (4:3 player 10000 type 0, in a group an END tests or in
    the main group), so a party guest, a white phantom, never got the insight (Kaniel
    2026-10-11). In those events the client end never matches and the alive check moves to a
    condition group nothing tests (an empty group passes)."""
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset = fields[0], fields[1], fields[3]
    args_offset = fields[13]
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        instructions = [struct.unpack_from('<iiqqq', data, instruction_offset + first + k * 32)
                        for k in range(count)]
        if not any((bank, ident) == INITIALIZE_EVENT and length >= 8 and
                   struct.unpack_from('<i', data, args_offset + offset + 4)[0] == INSIGHT_EVENT
                   for bank, ident, length, offset, _ in instructions):
            continue
        for k, (bank, ident, length, offset, _) in enumerate(instructions):
            at = args_offset + offset
            if bank == 1003 and ident == 6 and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'client end never matches')
                patched += 1
            if (bank == 4 and ident == 3 and length >= 9 and data[at] != UNUSED_GROUP and
                    struct.unpack_from('<i', data, at + 4)[0] == LOCAL_PLAYER and
                    data[at + 8] == ALIVE):
                data[at] = UNUSED_GROUP
                note(edits, event, k, bank, ident, 'alive check no longer waited on')
                patched += 1
    return bytes(data), patched


# The chalice maps' multiplayer wall: m29's event 12907600 is common event 7600 again (walls off,
# then on while 3:6 sees multiplayer state 3 or 2 in condition group -1, off again for group -2).
# The emulator turns 7600's walls off by the main maps' objects; a chalice dungeon's come from its
# blocks (2000:0 per block with object and fog), so the "show" checks never match instead.
CHALICE_WALL_EVENTS = {'m29.emevd.dcx': 12907600}
IF_MULTIPLAYER_STATE = (3, 6)


def patch_chalice_walls(emevd: bytes, edits: list | None = None,
                        event_id: int = 12907600) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event != event_id:
            continue
        for k in range(count):
            bank, ident, length, offset, _ = struct.unpack_from(
                '<iiqqq', data, instruction_offset + first + k * 32)
            at = args_offset + offset
            if (bank, ident) == IF_MULTIPLAYER_STATE and length >= 2 and data[at] == 0xFF:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'chalice multiplayer wall never shows')
                patched += 1
    return bytes(data), patched


# The only doors and devices in the game that say "Only the beckoner can open this door" (or
# "operate this device"): a chalice dungeon's boss door (ObjActParam 2901900-2901920) and the
# Research Hall's door and device (3500020, 3500110). Each asks for SpEffect 9920, which the map's
# event 0 puts on the player (2004:8 10000 9920) after a skip: m29's skips that line for a client
# (1003:5 lines 1, state 1), m35's unless the player is alive in its own world (1000:1 lines 1 on
# a condition group holding 4:3 10000 0). The skip never matches (state NEVER) or skips nothing
# (0 lines): every party member gets the effect.
HOST_DOOR_EFFECT = {'m29.emevd.dcx': 9920, 'm35_00_00_00.emevd.dcx': 9920}
SKIP_IF_MULTIPLAYER_STATE = (1003, 5)
SKIP_IF_CONDITION_GROUP = (1000, 1)
SET_SP_EFFECT = (2004, 8)


def patch_host_door_effect(emevd: bytes, edits: list | None = None,
                           effect: int = 9920) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event != 0:
            continue
        records = [struct.unpack_from('<iiqqq', data, instruction_offset + first + k * 32)
                   for k in range(count)]
        alive_groups = {data[args_offset + offset] for bank, ident, length, offset, _ in records
                        if (bank, ident) == (4, 3) and length >= 9 and
                        struct.unpack_from('<i', data, args_offset + offset + 4)[0] ==
                        LOCAL_PLAYER and data[args_offset + offset + 8] == ALIVE}
        for k in range(count - 1):
            bank, ident, length, offset, _ = records[k]
            next_bank, next_ident, next_length, next_offset, _ = records[k + 1]
            at = args_offset + offset
            if (next_bank, next_ident) != SET_SP_EFFECT or next_length < 8 or length < 3:
                continue
            if struct.unpack_from('<ii', data, args_offset + next_offset) != (LOCAL_PLAYER,
                                                                             effect):
                continue
            if ((bank, ident) == SKIP_IF_MULTIPLAYER_STATE and data[at] == 1 and
                    data[at + 1] == 1):
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'host door effect for a client too')
                patched += 1
            elif ((bank, ident) == SKIP_IF_CONDITION_GROUP and data[at] == 1 and
                  data[at + 2] in alive_groups):
                data[at] = 0
                note(edits, event, k, bank, ident, 'host door effect for a phantom too')
                patched += 1
    return bytes(data), patched


# A one-time enemy (the Wandering Nightmares: m24_00 12400200, m23 12300220, m28 12800500 and
# the like, a slot per enemy with its drop's flag as a parameter) goes for good once its event ends:
# the event's own slot flag then switches it off (2004:5) or hides it (2004:29) at every load. The
# event ends when an alive player (4:3 10000 0) has the drop's flag on (3:0), but a client skips
# that flag check (1003:5 lines 1, state 1). A phantom never reads alive, a party guest does (after
# a trip it is type 0), so the guest's game ended every one of them on the map with no kill, and
# the session's sync and the team's world took them off for everybody (rig nightmare-flags
# 2026-10-24: all six of Cathedral Ward's slot flags on, no drop flag, in both games; Fran's and
# Kaniel's report). The skip never matches: the drop's flag counts for every player.
ONE_TIME_ENEMY_HIDES = {(2004, 5), (2004, 29)}
GOTO_IF_EVENT_FLAG = (1003, 101)
IF_EVENT_FLAG = (3, 0)
OWN_SLOT_FLAG = 2  # 1003:101's flag type: the event's id plus its slot


def keep_one_time_drop_checks(emevd: bytes, edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        records = [struct.unpack_from('<iiqqq', data, instruction_offset + first + k * 32)
                   for k in range(count)]
        if count < 5:
            continue
        bank, ident, length, offset, _ = records[0]
        at = args_offset + offset
        if ((bank, ident) != GOTO_IF_EVENT_FLAG or length < 4 or data[at + 1] != 0 or
                data[at + 2] != OWN_SLOT_FLAG or (records[1][0], records[1][1])
                not in ONE_TIME_ENEMY_HIDES):
            continue
        for k in range(1, count - 1):
            bank, ident, length, offset, _ = records[k]
            before, after = records[k - 1], records[k + 1]
            at = args_offset + offset
            if (bank, ident) != SKIP_IF_MULTIPLAYER_STATE or length < 2 or data[at:at + 2] != \
                    bytes([1, 1]):
                continue
            alive = ((before[0], before[1]) == (4, 3) and before[2] >= 9 and
                     struct.unpack_from('<i', data, args_offset + before[3] + 4)[0] ==
                     LOCAL_PLAYER and data[args_offset + before[3] + 8] == ALIVE)
            drop = (after[0], after[1]) == IF_EVENT_FLAG and after[2] >= 8
            if (alive and drop and data[args_offset + before[3]] ==
                    data[args_offset + after[3]]):
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, "one-time enemy's drop check for a client too")
                patched += 1
    return bytes(data), patched


# A chalice dungeon's last boss reward (m29 12901693, started by the layer's own event file with
# the lots as parameters, Kaniel 2026-10-20): every player's gem (2003:36), then for the host only
# an extra lot (2003:4: a gem, or the next Root Chalice, by the dungeon's flags), which a client
# skips (1003:105 to a label, state 1) and could not be given anyway. The skips never match and the
# extra lot reaches a client too, so each party member rolls its own (loot is per player). The
# event already runs in each player's own game (2000:2 0).
CHALICE_REWARD_EVENTS = {'m29.emevd.dcx': 12901693}
GOTO_IF_MULTIPLAYER_STATE = (1003, 105)


# The game's banner 15 (TextEffect_YouWin, UNLOCKED) is party death's YOU ARE DOWN in the HUD patch
# (bbport/scripts/hud_patch.py): every banner is in use and this one has NIGHTMARE SLAIN's timeline
# and layers. Its only event shows none; the message box after it still says what happened.
# SetTextEffect takes ids 1 to 17, so id 0 queues nothing.
DROPPED_BANNER_EVENTS = {'m29.emevd.dcx': {12900192}}
SET_TEXT_EFFECT = (2007, 2)
DOWNED_BANNER_ID = 15


def drop_banners(emevd: bytes, wanted: set[int],
                 edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event not in wanted:
            continue
        for k in range(count):
            bank, ident, length, offset, _ = struct.unpack_from(
                '<iiqqq', data, instruction_offset + first + k * INSTRUCTION_SIZE)
            at = args_offset + offset
            if ((bank, ident) == SET_TEXT_EFFECT and length >= 4 and
                    struct.unpack_from('<i', data, at)[0] == DOWNED_BANNER_ID):
                struct.pack_into('<i', data, at, 0)
                note(edits, event, k, bank, ident, 'its banner is the downed banner now')
                patched += 1
    return bytes(data), patched


def patch_chalice_rewards(emevd: bytes, edits: list | None = None,
                          event_id: int = 12901693) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event != event_id:
            continue
        for k in range(count):
            record = instruction_offset + first + k * INSTRUCTION_SIZE
            bank, ident, length, offset, _ = struct.unpack_from('<iiqqq', data, record)
            at = args_offset + offset
            if (bank, ident) == GOTO_IF_MULTIPLAYER_STATE and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'client skip never matches')
                patched += 1
            if (bank, ident) == AWARD_ITEM_LOT:
                struct.pack_into('<i', data, record + 4, AWARD_ITEM_LOT_INCLUDING_CLIENTS)
                note(edits, event, k, bank, AWARD_ITEM_LOT_INCLUDING_CLIENTS,
                     'chalice reward for clients too')
                patched += 1
    return bytes(data), patched


def patch_client_events(emevd: bytes, wanted: set[int],
                        edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, _, instruction_offset = fields[0], fields[1], fields[2], fields[3]
    args_offset = fields[13]
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event not in wanted:
            continue
        for k in range(count):
            bank, ident, length, offset, _ = struct.unpack_from(
                '<iiqqq', data, instruction_offset + first + k * 32)
            at = args_offset + offset
            # 1003:6 "end the event by multiplayer state" (end type, state): state 1 is a client.
            if bank == 1003 and ident == 6 and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'client end never matches')
                patched += 1
    return bytes(data), patched


# The Dream's endings (m21_00 12100000 after Gehrman, 12100002 after the Moon Presence, 12100180
# Gehrman's offer taken): the cutscene, the ending's trophy, then the next playthrough (2003:21).
# Each ended for a client (1003:6 state 1), 12100000 also unless the local player is alive (4:3
# player 10000 type 0, a white phantom is type 1): the guests saw nothing, their session ended
# under them as the host's game left for the next playthrough, and they stayed in the old one
# (Kaniel's players 2026-10-28/29). Patched, a guest's game plays the ending too and starts its
# own next playthrough; the runtime keeps each save's new world (PlaythroughWatch).
ENDING_EVENTS = {'m21_00_00_00.emevd.dcx': {12100000, 12100002, 12100180}}


def let_guests_see_the_endings(emevd: bytes, wanted: set[int],
                               edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset = fields[0], fields[1], fields[3]
    args_offset = fields[13]
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event not in wanted:
            continue
        for k in range(count):
            bank, ident, length, offset, _ = struct.unpack_from(
                '<iiqqq', data, instruction_offset + first + k * 32)
            at = args_offset + offset
            if bank == 1003 and ident == 6 and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'client end never matches')
                patched += 1
            if (bank == 4 and ident == 3 and length >= 9 and data[at] != UNUSED_GROUP and
                    struct.unpack_from('<i', data, at + 4)[0] == LOCAL_PLAYER and
                    data[at + 8] == ALIVE):
                data[at] = UNUSED_GROUP
                note(edits, event, k, bank, ident, 'alive check no longer waited on')
                patched += 1
    return bytes(data), patched


# Items picked up with the action button at a glowing spot (2006:4 its sparkle, 3:24 the button,
# 2003:4 the item lot), per file: event -> (lot, the lot's pickup flag, ItemLotParam
# getItemFlagId). Each ends for a client (1003:6 state 1) or unless the local player is alive
# (4:3 player 10000 type 0, a white phantom is type 1), awards nothing to a client (2003:4) and
# ends once its own event flag is on, a map flag a guest takes from the host's world. So a party
# guest never saw them (Kaniel 2026-10-19: the Abandoned Old Workshop's Small Hair Ornament, Third
# Umbilical Cord and Old Hunter Bone). Patched, the event runs for a guest (the client end never
# matches, the alive check moves to a condition group nothing tests), awards the lot to a client
# too (2003:36), ends once the player's own pickup flag is on instead of its event flag (the
# pickups are each character's own list, IsMapPickupFlag; m32's 13200400 already ends so) and
# runs in each player's own game (2000:2 0).
ACTION_PICKUP_EVENTS = {
    'm21_01_00_00.emevd.dcx': {12110300: (2110800, 52110800),   # Small Hair Ornament
                               12110301: (2110810, 52110810),   # Third Umbilical Cord
                               12110302: (2110000, 52110000)},  # Old Hunter Bone
    'm23_00_00_00.emevd.dcx': {12300130: (2300170, 52300170)},
    'm24_01_00_00.emevd.dcx': {12410510: (2410990, 52410990)},
    'm25_00_00_00.emevd.dcx': {12500600: (2500910, 52500910), 12501803: (2502000, 52502000)},
    # Its lot (2600570) is the parameter of its only start, slot 0.
    'm26_00_00_00.emevd.dcx': {12600125: (0, 52600570)},
    'm32_00_00_00.emevd.dcx': {13200400: (3200810, 53200810)},
    'm35_00_00_00.emevd.dcx': {13501803: (3501800, 53501800)},
}
END_IF_FLAG = (1003, 2)
THIS_EVENT = 1
THIS_EVENT_SLOT = 2
EXPLICIT_FLAG = 0
AWARD_ITEM_LOT = (2003, 4)
# Instructions a patch may swap for one with the same arguments (a started event's parameter may
# still write them).
SAME_ARGUMENTS = {((2003, 4), (2003, AWARD_ITEM_LOT_INCLUDING_CLIENTS))}


def let_guests_pick_up_items(emevd: bytes, wanted: dict[int, tuple[int, int]],
                             edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event not in wanted:
            continue
        expected_lot, pickup = wanted[event]
        for k in range(count):
            record = instruction_offset + first + k * INSTRUCTION_SIZE
            bank, ident, length, offset, _ = struct.unpack_from('<iiqqq', data, record)
            at = args_offset + offset
            if bank == 1003 and ident == 6 and length >= 2 and data[at + 1] == 1:
                data[at + 1] = NEVER
                note(edits, event, k, bank, ident, 'client end never matches')
                patched += 1
            if (bank == 4 and ident == 3 and length >= 9 and data[at] != UNUSED_GROUP and
                    struct.unpack_from('<i', data, at + 4)[0] == LOCAL_PLAYER and
                    data[at + 8] == ALIVE):
                data[at] = UNUSED_GROUP
                note(edits, event, k, bank, ident, 'alive check no longer waited on')
                patched += 1
            # 1003:2 (end type, state, flag type, flag): flag type 1 is this event's own flag, 2
            # its slot's.
            if ((bank, ident) == END_IF_FLAG and length >= 8 and data[at + 1] == 1 and
                    data[at + 2] in (THIS_EVENT, THIS_EVENT_SLOT)):
                data[at + 2] = EXPLICIT_FLAG
                struct.pack_into('<i', data, at + 4, pickup)
                note(edits, event, k, bank, ident, 'ends once the own pickup flag is on')
                patched += 1
            if (bank, ident) == AWARD_ITEM_LOT and length >= 4:
                lot = struct.unpack_from('<i', data, at)[0]
                if lot != expected_lot:
                    raise EventPatchError(f'event {event} awards lot {lot}, not {expected_lot}')
                struct.pack_into('<i', data, record + 4, AWARD_ITEM_LOT_INCLUDING_CLIENTS)
                note(edits, event, k, bank, AWARD_ITEM_LOT_INCLUDING_CLIENTS,
                     'item for clients too')
                patched += 1
    return bytes(data), patched


# Enemies that rise out of the mist when a player comes near (2004:5 enable, 2003:18 animation
# 6200), per file: one event slot per enemy. Each waits for the local player to be within reach
# (3:3 player 10000 near the enemy) and alive in its own world (4:3 player 10000 type 0) in the
# same condition group, so only the host's approach woke them; a guest, a white phantom, saw them
# rise only when the host came near (Kaniel 2026-10-20: the woods on the way into Hemwick, m22_00
# 12204000, nine slots). The alive check moves to a condition group nothing waits on, so any party
# member's approach wakes the enemy, and the event (kept in step) wakes it in every member's game.
# The same template (without the slot flag path m22's has) stands in m24_01, m25_00 and m26_00 and
# in the chalice dungeons (m29 12905406).
AMBUSH_EVENTS = {
    'm22_00_00_00.emevd.dcx': {12204000},
    'm24_01_00_00.emevd.dcx': {12414000},
    'm25_00_00_00.emevd.dcx': {12504000},
    'm26_00_00_00.emevd.dcx': {12604000},
    'm29.emevd.dcx': {12905406},
}
IF_NEAR_ENTITY = (3, 3)
APPEAR_ANIMATION = 6200


def let_guests_wake_ambushes(emevd: bytes, wanted: set[int],
                             edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = struct.unpack_from('<16q', data, 16)
    event_count, event_offset, instruction_offset, args_offset = (fields[0], fields[1], fields[3],
                                                                  fields[13])
    patched = 0
    for index in range(event_count):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        if event not in wanted:
            continue
        records = [struct.unpack_from('<iiqqq', data, instruction_offset + first + k * 32)
                   for k in range(count)]
        if not any((bank, ident) == (2003, 18) and length >= 8 and
                   struct.unpack_from('<i', data, args_offset + offset + 4)[0] == APPEAR_ANIMATION
                   for bank, ident, length, offset, _ in records):
            raise EventPatchError(f'event {event} plays no appear animation {APPEAR_ANIMATION}')
        near_groups = {data[args_offset + offset] for bank, ident, length, offset, _ in records
                       if (bank, ident) == IF_NEAR_ENTITY and length >= 8 and
                       struct.unpack_from('<i', data, args_offset + offset + 4)[0] ==
                       LOCAL_PLAYER}
        moved = 0
        for k, (bank, ident, length, offset, _) in enumerate(records):
            at = args_offset + offset
            if ((bank, ident) == (4, 3) and length >= 9 and data[at] in near_groups and
                    struct.unpack_from('<i', data, at + 4)[0] == LOCAL_PLAYER and
                    data[at + 8] == ALIVE):
                data[at] = UNUSED_GROUP
                note(edits, event, k, bank, ident, 'alive check no longer waited on')
                moved += 1
        if not moved and UNUSED_GROUP not in {data[args_offset + offset]
                                              for bank, ident, length, offset, _ in records
                                              if (bank, ident) == (4, 3)}:
            raise EventPatchError(f'event {event} has no alive check beside its approach')
        patched += moved
    return bytes(data), patched


# Events that get 2000:2 0 (no network sync) as their first instruction, per file.
# m21_00 12107100 (a slot per ritual altar) warps into the chalice dungeon picked at that altar.
# m21_00 12107200 (a slot per layer) then warps to the layer's respawn point: kept in step, the
# host's warp ran in every teammate's game and took each one in the Dream along (2026-10-20).
# m21_00 12101000 (slots 0-9) unlocks the messengers' shop lines from the player's own items
# (12101004: the Hunter Chief Emblem); kept in step, a guest's Dream waited on the host's.
# 12105040 rises the shop messengers in the bath (2100211-2100213) while this player's shop menu
# is open (12105041, their talk) and sinks them after; kept in step, another player's restart
# of it rose them and played their tune again and again (Kaniel 2026-10-21).
# m29 12906824 (started by the story layouts m29_20_90_00 and m29_31_90_00) sets a Bath shop flag
# (5090 Bone Ash, 5091 Madman) once the layer 2 boss is dead; the shop flags are each player's
# own, so kept in step the first game to run it ended it in the other's, which never set its own
# (seixas 2026-10-27: no Bone Ash set after the Keeper of the Old Lords).
# m28_00 12800430 (Yahar'gul's mummy, see CLIENT_EVENTS) warps to the Lecture Building: kept in
# step, one player's inspection would take every teammate in Yahar'gul along.
LOCAL_EVENTS = {
    'm21_00_00_00.emevd.dcx': {12101000, 12101020, 12101021, 12101022, 12101024, 12101026,
                               12101028, 12105040, 12107100, 12107200},
    'm28_00_00_00.emevd.dcx': {12800430},
    'm29.emevd.dcx': {12906824},
}
for _name, _events in ACTION_PICKUP_EVENTS.items():
    LOCAL_EVENTS[_name] = LOCAL_EVENTS.get(_name, set()) | set(_events)
NETWORK_SYNC = (2000, 2)
INSTRUCTION_SIZE = 32
PARAMETER_SIZE = 32  # instruction index, target byte, source byte, length (q q q i i)
# Header offset fields after the instruction table (DS3 layout): unknown, layers, params, linked
# files, arguments, strings.
SHIFTED_OFFSET_FIELDS = (5, 7, 9, 11, 13, 15)


def make_events_local(emevd: bytes, wanted: set[int],
                      edits: list | None = None) -> tuple[bytes, int]:
    data = bytearray(emevd)
    fields = list(struct.unpack_from('<16q', data, 16))
    event_count, event_offset = fields[0], fields[1]
    instruction_count, instruction_offset = fields[2], fields[3]
    args_offset, args_size = fields[13], fields[12]
    events = [list(struct.unpack_from('<qqqqqii', data, event_offset + index * 48))
              for index in range(event_count)]
    targets = sorted((event[2] for event in events
                      if event[0] in wanted and event[1] > 0 and
                      not already_local(data, instruction_offset, args_offset, event)),
                     reverse=True)
    if not targets:
        return emevd, 0
    # The instruction's argument (4 bytes of 0) can share any aligned zero word of the table.
    zero = next((at for at in range(0, args_size - 3, 4)
                 if data[args_offset + at:args_offset + at + 4] == bytes(4)), None)
    if zero is None:
        raise EventPatchError('no 4 zero bytes in the argument table for 2000:2 0 to use')
    record = struct.pack('<iiqqq', NETWORK_SYNC[0], NETWORK_SYNC[1], 4, zero, -1)
    instructions_end = instruction_offset + instruction_count * INSTRUCTION_SIZE
    table = bytearray(data[instruction_offset:instructions_end])
    for first in targets:
        table[first:first] = record
    inserted = set()
    for index, event in enumerate(events):
        moved = sum(1 for first in targets if event[2] > first)
        if event[0] in wanted and event[2] in targets:
            inserted.add(index)
            event[1] += 1
            if edits is not None:
                edits.append({'event': event[0], 'action': 'insert', 'before': 0,
                              'bank': NETWORK_SYNC[0], 'id': NETWORK_SYNC[1],
                              'why': 'no network sync'})
        event[2] += moved * INSTRUCTION_SIZE
    delta = len(targets) * INSTRUCTION_SIZE
    out = data[:instruction_offset] + table + data[instructions_end:]
    for index, event in enumerate(events):
        struct.pack_into('<qqqqqii', out, event_offset + index * 48, *event)
    fields[2] += len(targets)
    for field in SHIFTED_OFFSET_FIELDS:
        if fields[field] >= instructions_end:
            fields[field] += delta
    struct.pack_into('<16q', out, 16, *fields)
    # A started event's parameters copy its arguments into instructions by index: those follow
    # their instructions one down (Kaniel 2026-10-12: 12101000's item check took nothing), also
    # in an event an earlier insert moved.
    for index in inserted:
        event = events[index]
        for k in range(event[3]):
            at = fields[9] + event[4] + k * PARAMETER_SIZE
            struct.pack_into('<q', out, at, struct.unpack_from('<q', out, at)[0] + 1)
    struct.pack_into('<I', out, 0x0C, struct.unpack_from('<I', out, 0x0C)[0] + delta)
    return bytes(out), len(targets)


SET_EVENT_FLAG = (2003, 2)
BATCH_SET_EVENT_FLAGS = (2003, 22)
ALIVE = 0
WHITE_PHANTOM = 1
ARGS_ALIGNMENT = 16


def open_boss_fog_for_guests(emevd: bytes, edits: list | None = None) -> tuple[bytes, int]:
    """A white phantom's boss fog event (e.g. m24_01 12414731) waits for the host's "entered
    the fog" flag H (12414700), which the host's own fog event sets, and then sets H + 1. A
    party guest could therefore never enter first, and after a world reset it was stuck behind
    the fog until the host went in again. The wait on H moves to an unused condition group, and
    the final "set H + 1" becomes "set H..H + 1", so a guest entering starts the boss (the boss
    start event waits on H) like a host entering does. The host's fog event (which sets H) also
    opens the fog for a client until the boss's entrance has run. A boss entrance that sets H when a
    player walks into the arena (m24_01 12411702) waits for an alive player; a white phantom
    passes too."""
    data = bytearray(emevd)
    fields = list(struct.unpack_from('<16q', data, 16))
    event_count, event_offset, instruction_offset = fields[0], fields[1], fields[3]
    args_offset = fields[13]
    new_args = bytearray()
    patched = 0
    host_entered_flags = set()

    def read_event(index):
        event, count, first, _, _, _, _ = struct.unpack_from('<qqqqqii', data,
                                                             event_offset + index * 48)
        at = [instruction_offset + first + k * INSTRUCTION_SIZE for k in range(count)]
        instructions = [struct.unpack_from('<iiqqq', data, place) for place in at]
        args = [bytes(data[args_offset + offset:args_offset + offset + length])
                for _, _, length, offset, _ in instructions]
        return event, at, instructions, args

    def player_type_checks(instructions, args, player_type):
        return [k for k, (bank, ident, *_) in enumerate(instructions)
                if (bank, ident) == (4, 3) and len(args[k]) >= 9 and
                struct.unpack_from('<i', args[k], 4)[0] == LOCAL_PLAYER and
                args[k][8] == player_type]

    for index in range(event_count):
        event, at, instructions, args = read_event(index)
        phantom = player_type_checks(instructions, args, WHITE_PHANTOM)
        button = any((bank, ident) == (3, 24) for bank, ident, *_ in instructions)
        if not (phantom and button):
            continue
        sets = [k for k, (bank, ident, *_) in enumerate(instructions)
                if (bank, ident) == SET_EVENT_FLAG and args[k][4] == 1]
        if len(sets) != 1:
            continue
        entered = struct.unpack_from('<i', args[sets[0]], 0)[0]
        host_entered = entered - 1
        waits = [k for k, (bank, ident, *_) in enumerate(instructions)
                 if (bank, ident) == (3, 0) and args[k][1] == 1 and args[k][0] != UNUSED_GROUP and
                 struct.unpack_from('<i', args[k], 4)[0] == host_entered]
        if not waits:
            continue
        host_entered_flags.add(host_entered)
        for k in waits:
            data[args_offset + instructions[k][3]] = UNUSED_GROUP
            note(edits, event, k, 3, 0, 'no longer waits for the host flag')
            patched += 1
        offset = fields[12] + len(new_args)
        new_args += struct.pack('<iiBxxx', host_entered, entered, 1)
        struct.pack_into('<iiqqq', data, at[sets[0]], *BATCH_SET_EVENT_FLAGS, 12, offset,
                         instructions[sets[0]][4])
        note(edits, event, sets[0], *BATCH_SET_EVENT_FLAGS,
             'a guest entering sets the host flag too')
        patched += 1
    for index in range(event_count):
        event, at, instructions, args = read_event(index)
        button = any((bank, ident) == (3, 24) for bank, ident, *_ in instructions)
        area = any((bank, ident) == (3, 2) for bank, ident, *_ in instructions)
        starts_boss = any((bank, ident) == SET_EVENT_FLAG and args[k][4] == 1 and
                          struct.unpack_from('<i', args[k], 0)[0] in host_entered_flags
                          for k, (bank, ident, *_) in enumerate(instructions))
        if button and starts_boss and player_type_checks(instructions, args, ALIVE):
            # The host's fog event: it opens the fog until the boss's entrance has run, but
            # skips that for a client (1003:5 state 1), so a guest arriving first faced a shut
            # fog with no way in.
            for k, (bank, ident, length, offset, _) in enumerate(instructions):
                if (bank, ident) == (1003, 5) and length >= 2 and args[k][1] == 1:
                    data[args_offset + offset + 1] = NEVER
                    note(edits, event, k, bank, ident, 'client skip never matches')
                    patched += 1
            continue
        if button or not area or not starts_boss:
            continue
        for k in player_type_checks(instructions, args, ALIVE):
            if 0 < args[k][0] < UNUSED_GROUP:
                data[args_offset + instructions[k][3]] = UNUSED_GROUP
                note(edits, event, k, 4, 3, 'alive check no longer waited on')
                patched += 1
    if not new_args:
        return bytes(data), patched
    return with_new_arguments(data, fields, new_args), patched


# Rom's lake (m32_00, Kaniel 2026-10-29): a white phantom's fog by the edge (13204831) waited for
# the boss's entrance (13201802) and the host's "dived" flag (13204800), and the drop's 5 s of
# no-death for anyone but the host (13204821) waited for that flag too. 13204820 gives it to an
# alive player (the host) walking into the drop's area 3202800 and then sets 13204800, which
# starts Rom. So a guest could not pass the fog before the host did, and a guest who went over
# the edge first died from the 96 m fall.
LAKE_DIVE_EVENTS = {'m32_00_00_00.emevd.dcx': {'fog': 13204831, 'dive': 13204821}}


def let_guests_dive_first(emevd: bytes, wanted: dict[str, int],
                          edits: list | None = None) -> tuple[bytes, int]:
    """The phantom fog stops waiting for flags the host sets (its condition groups keep the
    boss-dead check, the phantom check and the button). The dive's no-death stops waiting for
    the host's flag H, and its "set H + 1" becomes "set H..H + 1", so a guest diving first is
    kept alive by the fall and starts the boss like the host does (H is shared with the party,
    IsBossFogEntryFlag)."""
    data = bytearray(emevd)
    fields = list(struct.unpack_from('<16q', data, 16))
    event_count, event_offset, instruction_offset = fields[0], fields[1], fields[3]
    args_offset = fields[13]
    new_args = bytearray()
    patched = 0
    for index in range(event_count):
        event, count, first, *_ = struct.unpack_from('<qqqqqii', data, event_offset + index * 48)
        if event not in wanted.values():
            continue
        at = [instruction_offset + first + k * INSTRUCTION_SIZE for k in range(count)]
        instructions = [struct.unpack_from('<iiqqq', data, place) for place in at]
        args = [bytes(data[args_offset + offset:args_offset + offset + length])
                for _, _, length, offset, _ in instructions]
        waits = [k for k, (bank, ident, *_) in enumerate(instructions)
                 if (bank, ident) == IF_EVENT_FLAG and args[k][1] == 1 and
                 0 < args[k][0] < UNUSED_GROUP]
        if not waits:
            continue  # patched already
        if event == wanted['dive']:
            sets = [k for k, (bank, ident, *_) in enumerate(instructions)
                    if (bank, ident) == SET_EVENT_FLAG and args[k][4] == 1]
            if len(waits) != 1 or len(sets) != 1:
                raise EventPatchError(f'event {event}: not the dive this patch knows')
            entered = struct.unpack_from('<i', args[sets[0]], 0)[0]
            if struct.unpack_from('<i', args[waits[0]], 4)[0] != entered - 1:
                raise EventPatchError(f'event {event}: it does not wait for the flag before '
                                      f'{entered}')
            offset = fields[12] + len(new_args)
            new_args += struct.pack('<iiBxxx', entered - 1, entered, 1)
            struct.pack_into('<iiqqq', data, at[sets[0]], *BATCH_SET_EVENT_FLAGS, 12, offset,
                             instructions[sets[0]][4])
            note(edits, event, sets[0], *BATCH_SET_EVENT_FLAGS,
                 'a guest diving sets the host flag too')
            patched += 1
        for k in waits:
            data[args_offset + instructions[k][3]] = UNUSED_GROUP
            note(edits, event, k, *IF_EVENT_FLAG, 'no longer waits for the host flag')
            patched += 1
    if not new_args:
        return bytes(data), patched
    return with_new_arguments(data, fields, new_args), patched


def with_new_arguments(data: bytearray, fields: list, new_args: bytearray) -> bytes:
    """Appends arguments to the argument table, moving the tables after it."""
    new_args += bytes(-(fields[12] + len(new_args)) % ARGS_ALIGNMENT)
    args_end = fields[13] + fields[12]
    out = data[:args_end] + new_args + data[args_end:]
    for field in SHIFTED_OFFSET_FIELDS:
        if field != 13 and fields[field] >= args_end:
            fields[field] += len(new_args)
    fields[12] += len(new_args)
    struct.pack_into('<16q', out, 16, *fields)
    struct.pack_into('<I', out, 0x0C, struct.unpack_from('<I', out, 0x0C)[0] + len(new_args))
    return bytes(out)


def already_local(data: bytes, instruction_offset: int, args_offset: int, event: list) -> bool:
    bank, ident, _, offset, _ = struct.unpack_from('<iiqqq', data, instruction_offset + event[2])
    return ((bank, ident) == NETWORK_SYNC and
            data[args_offset + offset:args_offset + offset + 4] == bytes(4))


HEADER_SIZE = 0x90
EVENT_SIZE = 48
# Header fields (count or byte length, offset, entry size) of each table; the unknown table is
# always empty.
SECTIONS = {'events': (0, 1, EVENT_SIZE), 'instructions': (2, 3, INSTRUCTION_SIZE),
            'unknown table': (4, 5, 0), 'layers': (6, 7, 32), 'parameters': (8, 9, PARAMETER_SIZE),
            'linked files': (10, 11, 8), 'arguments': (12, 13, 1), 'strings': (14, 15, 1)}
MANIFEST = 'manifest.json'


def parse(emevd: bytes) -> dict:
    """Reads an event file's tables, checking every offset and count against the file."""
    if emevd[:4] == b'DCX\0':
        size = struct.unpack_from('>I', emevd, 0x1C)[0]
        try:
            emevd = load_dcx(emevd)
        except zlib.error as error:
            raise EventPatchError(f'the DCX does not unpack ({error})') from error
        if size != len(emevd):
            raise EventPatchError(f'the DCX says it holds {size} bytes, it holds {len(emevd)}')
    if len(emevd) < HEADER_SIZE or emevd[:4] != b'EVD\0':
        raise EventPatchError('not an event file')
    size = struct.unpack_from('<I', emevd, 0x0C)[0]
    if size != len(emevd):
        raise EventPatchError(f'the header says the file size is {size}, it is {len(emevd)}')
    fields = struct.unpack_from('<16q', emevd, 16)
    for section, (count, offset, entry) in SECTIONS.items():
        end = fields[offset] + fields[count] * entry
        if fields[count] < 0 or fields[offset] < HEADER_SIZE or end > len(emevd):
            raise EventPatchError(f'the {section} table ({fields[count]} at {fields[offset]}) is '
                                  f'outside the file ({len(emevd)} bytes)')
    if fields[4]:
        raise EventPatchError(f'the unknown table has {fields[4]} entries')

    def table(section: str) -> bytes:
        count, offset, entry = SECTIONS[section]
        return emevd[fields[offset]:fields[offset] + fields[count] * max(entry, 1)]

    events = []
    for index in range(fields[0]):
        event, count, first, param_count, param_offset, rest, unknown = struct.unpack_from(
            '<qqqqqii', emevd, fields[1] + index * EVENT_SIZE)
        if (count < 0 or first < 0 or first % INSTRUCTION_SIZE or
                first + count * INSTRUCTION_SIZE > fields[2] * INSTRUCTION_SIZE):
            raise EventPatchError(f'event {event}: its {count} instructions at {first} are '
                                  f'outside the instruction table')
        instructions = []
        for k in range(count):
            bank, ident, length, offset, layer = struct.unpack_from(
                '<iiqqq', emevd, fields[3] + first + k * INSTRUCTION_SIZE)
            # An instruction without arguments has offset -1 (in the low 4 bytes).
            if length < 0 or length and (offset < 0 or offset + length > fields[12]):
                raise EventPatchError(f'event {event} instruction {k} ({bank}:{ident}): its '
                                      f'{length} argument bytes at {offset} are outside the '
                                      f'argument table')
            # No layer is -1 (the game's files keep it in the low 4 bytes only).
            if layer not in (-1, 0xFFFFFFFF) and not 0 <= layer < fields[6] * 32:
                raise EventPatchError(f'event {event} instruction {k}: layer {layer} is outside '
                                      f'the layer table')
            instructions.append((bank, ident, length, layer,
                                 emevd[fields[13] + offset:fields[13] + offset + length]))
        parameters = []
        if param_count:
            if (param_count < 0 or param_offset < 0 or
                    param_offset + param_count * PARAMETER_SIZE > fields[8] * PARAMETER_SIZE):
                raise EventPatchError(f'event {event}: its {param_count} parameters at '
                                      f'{param_offset} are outside the parameter table')
            for j in range(param_count):
                parameter = struct.unpack_from('<qqqii', emevd,
                                               fields[9] + param_offset + j * PARAMETER_SIZE)
                if not 0 <= parameter[0] < count:
                    raise EventPatchError(f'event {event} parameter {j} names instruction '
                                          f'{parameter[0]}, the event has {count}')
                parameters.append(parameter)
        events.append({'id': event, 'rest': (rest, unknown), 'instructions': instructions,
                       'parameters': parameters})
    return {'events': events, 'tables': {section: table(section) for section in
                                         ('layers', 'linked files', 'strings')}}


def name_of(instruction: tuple) -> str:
    return f'{instruction[0]}:{instruction[1]}'


def validate(original: bytes, patched: bytes, intended_edits: list, name: str = 'event file'):
    """Checks a patched event file against the game's before it is written: it must parse with
    every offset in bounds, and each event must be the game's except for the intended edits
    (inserted instructions, changed instructions by their number in the game's event). A started
    event's parameters copy its arguments into instructions by number and byte, so each must
    still name the same instruction and byte (2026-10-12: an insert moved the shop event's badge
    check away from its parameter and every weapon line opened). Raises EventPatchError."""

    def fail(message: str):
        raise EventPatchError(f'{name}: {message}')

    try:
        before, after = parse(original), parse(patched)
    except EventPatchError as error:
        fail(str(error))
    for section, data in before['tables'].items():
        if after['tables'][section] != data:
            fail(f'the {section} changed (a header offset was not moved with the data)')
    if [event['id'] for event in before['events']] != [event['id'] for event in after['events']]:
        fail('the list of events changed')
    by_id = {event['id']: (event, after['events'][index])
             for index, event in enumerate(before['events'])}
    inserts: dict[int, list] = {}
    changes: dict[int, dict] = {}
    for edit in intended_edits:
        if edit['event'] not in by_id:
            fail(f'an edit names event {edit["event"]}, which the file does not have')
        count = len(by_id[edit['event']][0]['instructions'])
        if edit['action'] == 'insert' and 0 <= edit['before'] <= count:
            inserts.setdefault(edit['event'], []).append(edit)
        elif edit['action'] == 'change' and 0 <= edit['instruction'] < count:
            changes.setdefault(edit['event'], {})[edit['instruction']] = edit
        else:
            fail(f'event {edit["event"]}: edit {edit} names no instruction of the event')
    for event_id, (old, new) in by_id.items():
        where = f'event {event_id}'
        own = sorted(inserts.get(event_id, []), key=lambda edit: edit['before'])
        changed = changes.get(event_id, {})
        old_list, new_list = old['instructions'], new['instructions']
        if len(new_list) != len(old_list) + len(own):
            fail(f'{where} has {len(new_list)} instructions, the game\'s has {len(old_list)} and '
                 f'{len(own)} were to be inserted')
        if new['rest'] != old['rest']:
            fail(f'{where}: its restart behaviour changed')
        # Where each of the game's instructions is now: one further down per insert above it.
        moved = [k + sum(1 for edit in own if edit['before'] <= k) for k in range(len(old_list))]
        for number, edit in enumerate(own):
            inserted = new_list[edit['before'] + number]
            if (inserted[0], inserted[1]) != (edit['bank'], edit['id']):
                fail(f'{where}: instruction {edit["before"] + number} is {name_of(inserted)}, '
                     f'the insert was {edit["bank"]}:{edit["id"]}')
        for k, instruction in enumerate(old_list):
            now = new_list[moved[k]]
            label = f'{where} instruction {k} ({name_of(instruction)})'
            if k in changed:
                edit = changed[k]
                if (now[0], now[1]) != (edit['bank'], edit['id']):
                    fail(f'{label} became {name_of(now)}, the edit made it '
                         f'{edit["bank"]}:{edit["id"]}')
                continue
            if now[:4] != instruction[:4]:
                fail(f'{label} became {name_of(now)} ({now[2]} argument bytes, layer {now[3]}) '
                     f'without an edit')
            if now[4] != instruction[4]:
                fail(f'{label}: its argument bytes changed without an edit '
                     f'({instruction[4].hex()} -> {now[4].hex()})')
        if len(new['parameters']) != len(old['parameters']):
            fail(f'{where} has {len(new["parameters"])} parameters, the game\'s has '
                 f'{len(old["parameters"])}')
        wrong = []
        for j, (parameter, now) in enumerate(zip(old['parameters'], new['parameters'])):
            k, target, source, length, unknown = parameter
            should = (moved[k], target, source, length, unknown)
            if now != should:
                wrong.append(f'parameter {j} (argument bytes {source}-{source + length - 1}) '
                             f'now goes to instruction {now[0]} ({name_of(new_list[now[0]])}) '
                             f'byte {now[1]}, it belongs to instruction {moved[k]} '
                             f'({name_of(old_list[k])}, the game\'s instruction {k}) byte {target}')
                continue
            if k in changed:
                # A started event copying its argument over the patched bytes would undo the
                # patch, and over a rewritten instruction would corrupt it.
                new_args, old_args = new_list[moved[k]][4], old_list[k][4]
                old, new = old_list[k], new_list[moved[k]]
                same_arguments = (((old[0], old[1]), (new[0], new[1])) in SAME_ARGUMENTS and
                                  new[2] == old[2])
                rewritten = ((new[:3] != old[:3] and not same_arguments) or
                             any(new_args[at] != old_args[at]
                                 for at in range(target, target + length)))
                if rewritten:
                    wrong.append(f'parameter {j} writes instruction {k} '
                                 f'({name_of(old_list[k])}) byte {target}, which was patched')
        if wrong:
            fail(f'{where}: ' + '; '.join(wrong))


def patch_file(name: str, original: bytes) -> tuple[bytes | None, list, dict]:
    """Patches one event file (DCX): the output (None if nothing changed), the edits and the
    counts per patch. The output is checked with validate."""
    emevd = load_dcx(original)
    edits: list = []
    counts = {'boss event checks': 0, 'boss fog checks': 0, 'insight event checks': 0,
              'client event ends': 0, 'local events': 0, 'chalice wall checks': 0,
              'item pickup checks': 0, 'host door effects': 0, 'ambush checks': 0,
              'chalice reward checks': 0, 'one-time enemy checks': 0, 'lake dive checks': 0,
              'ending checks': 0, 'dropped banners': 0}
    try:
        if name.startswith('m'):
            emevd, counts['boss event checks'] = patch_events(emevd, edits)
            emevd, counts['boss fog checks'] = open_boss_fog_for_guests(emevd, edits)
            emevd, counts['insight event checks'] = let_guests_earn_insight(emevd, edits)
            emevd, counts['one-time enemy checks'] = keep_one_time_drop_checks(emevd, edits)
        if name in CLIENT_EVENTS:
            emevd, counts['client event ends'] = patch_client_events(emevd, CLIENT_EVENTS[name],
                                                                     edits)
        if name in ENDING_EVENTS:
            emevd, counts['ending checks'] = let_guests_see_the_endings(emevd,
                                                                        ENDING_EVENTS[name], edits)
        if name in ACTION_PICKUP_EVENTS:
            emevd, counts['item pickup checks'] = let_guests_pick_up_items(
                emevd, ACTION_PICKUP_EVENTS[name], edits)
        if name in AMBUSH_EVENTS:
            emevd, counts['ambush checks'] = let_guests_wake_ambushes(emevd, AMBUSH_EVENTS[name],
                                                                      edits)
        if name in CHALICE_WALL_EVENTS:
            emevd, counts['chalice wall checks'] = patch_chalice_walls(
                emevd, edits, CHALICE_WALL_EVENTS[name])
        if name in HOST_DOOR_EFFECT:
            emevd, counts['host door effects'] = patch_host_door_effect(
                emevd, edits, HOST_DOOR_EFFECT[name])
        if name in LAKE_DIVE_EVENTS:
            emevd, counts['lake dive checks'] = let_guests_dive_first(
                emevd, LAKE_DIVE_EVENTS[name], edits)
        if name in CHALICE_REWARD_EVENTS:
            emevd, counts['chalice reward checks'] = patch_chalice_rewards(
                emevd, edits, CHALICE_REWARD_EVENTS[name])
        if name in DROPPED_BANNER_EVENTS:
            emevd, counts['dropped banners'] = drop_banners(emevd, DROPPED_BANNER_EVENTS[name],
                                                            edits)
        # Last: validate names changed instructions by their number in the game's event, which
        # only inserts move.
        if name in LOCAL_EVENTS:
            emevd, counts['local events'] = make_events_local(emevd, LOCAL_EVENTS[name], edits)
    except EventPatchError as error:
        raise EventPatchError(f'{name}: {error}') from error
    if not any(counts.values()):
        return None, [], counts
    output = save_dcx(original, emevd)
    validate(original, output, edits, name)
    return output, edits, counts


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    target = pathlib.Path(argv[0])
    files: dict[str, pathlib.Path] = {}
    for source in argv[1:]:
        for path in pathlib.Path(source).glob('*.emevd.dcx'):
            files[path.name] = path
    # Every file is patched and checked before any is written, so a bad patch leaves the
    # folder as it was.
    outputs = {}
    # The launcher makes the files again when its patcher's hash differs (events_current).
    manifest = {'tool': 'party_boss_events.py',
                'tool_sha256': hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
                'files': {}}
    total = 0
    for name, path in sorted(files.items()):
        original = path.read_bytes()
        output, edits, counts = patch_file(name, original)
        if output is None:
            continue
        outputs[name] = output
        manifest['files'][name] = {'input_sha256': hashlib.sha256(original).hexdigest(),
                                   'output_sha256': hashlib.sha256(output).hexdigest(),
                                   'edits': edits}
        print(f'{name}: ' + ', '.join(f'{count} {what}' for what, count in counts.items()))
        total += sum(counts.values())
    target.mkdir(parents=True, exist_ok=True)
    for name, output in outputs.items():
        (target / name).write_bytes(output)
    (target / MANIFEST).write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8',
                                   newline='\n')
    print(f'total: {total}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except EventPatchError as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
