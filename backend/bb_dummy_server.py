#!/usr/bin/env python3
"""Self-hosted stand-in for Bloodborne's retail backend.

Replaces thehuntersdream.com entirely. Serves everything on one port:

  GET  /bb-eu/ss.info   the online-config document the game gates on (each region asks
                        its own path: /bb-us/ss.info, ...; all get the same). Rebuilt
                        from ss_info_template.xml with every service URL
                        pointed at this machine instead of the community server.
  POST /<service>       every game service.

Two modes:

  default        every service answers with empty content. The game is online
                 and co-op works, but the world has no messages or bloodstains.
  --store FILE   messages, bloodstains, wandering ghosts, lantern counts and
                 chalice dungeons are kept in a SQLite file and served back to
                 the other players. Content blobs are stored exactly as the
                 game sends them; this server never interprets them.

Summon traffic is NOT handled here -- ss.info points those four URLs straight
at shadNet, which already implements them. Co-op is unaffected either way.

Run it alongside shadNet and leave it running: the game re-fetches ss.info
every 300s (ReloadServerStatusInfoInterval2), so if this dies mid-session the
game drops to offline a few minutes later.

Then host_overrides.json only needs:

    {"https://ss4.scej-network.jp:20443": "http://<this machine>:20443"}

Every request and response shape below was taken from live captures of the
community server. The few shapes that were never observed are marked INFERRED.
"""

import argparse
import base64
import binascii
import datetime
import ipaddress
import json
import os
import re
import secrets
import sqlite3
import struct
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

SS_INFO_PATH = "/bb-eu/ss.info"
# Each region's game asks its own path (/bb-us/ss.info from a US game, 2026-10-22); every one gets
# the same document.
SS_INFO_PATTERN = re.compile(r"/bb-[a-z]{2,8}/ss\.info")


def is_ss_info_path(path):
    """Whether a request path is a region's ss.info ("/bb-eu/ss.info", "/bb-us/ss.info", ...)."""
    return bool(SS_INFO_PATTERN.fullmatch(path or ""))

# The game sends these when it means "any area" (first ghost query at boot).
WILDCARD_AREA = {-1, 4294967295}

# Messenger shell shown on a player's messages/bloodstains until they upload
# their own via MessengerShellUpload. Value observed as the common default.
DEFAULT_SHELL = ("AQAAAAAAAAA=", 2)

# Chalice glyph alphabet: digits 2-9 and lowercase minus l/o, matching the
# observed community glyphs (no 0, 1, l, o anywhere in ~200 samples).
GLYPH_ALPHABET = "23456789abcdefghijkmnpqrstuvwxyz"

SHOW_OWN_CHOICES = ("deaths", "ghosts", "lanterns")

# Standard chalice dungeons. A standard (not root) chalice's ritual asks for the one dungeon the
# server keeps for that chalice: ChannelSearch with FixedOrGeneral 2 and GetCount 1, then its
# details by ChannelId. Without one the game says "Failed to contribute ritual materials" and
# returns to the title. The server makes them itself. FormData is the dungeon as the game uploads
# one (ChannelUpload after a root chalice's ritual, format 5, little endian): u32 5, u32 140;
# layout, rites, depth*10 + area (0 Pthumeru, 1 Hintertomb, 2 Loran, 3 Isz), 0x1D; u32 the
# chalice's goods id; 9 feature slots of {u32 id, s32 code} (-1 for none); the conductor's name,
# 16 bytes ASCII, and 32 bytes UTF-16; zeros to 140. A ritual takes the chalice from the player's
# hands but its layout from the record's rites byte: 90 is a standard chalice's story layout,
# m29_<key>_90_00 (0 gave the story Pthumeru Chalice the root layout m29_10_00_00 and its boss the
# root loot, rig BB_FILE_TRACE 2026-10-21); a join through Chalice Dungeon Search builds it from
# FormData alone. The slots are the game's own for the chalice (HolygrailExParam row <goods>, each
# id's one code from DungeonSubFeatLotParam): the ritual sets the altar's flag 5300 + each code's
# feature flag, and the last bosses give the next chalice only with 5378 or 5385 on (m29 event
# 12901693; the root slots these had before gave none, Kaniel's playtest 2026-10-25). Keyed by
# RitualLevel * 10 + HolyGrailTypeId: (rites, goods id, slots of (id, code), None for none).
_S90, _S91, _S93, _S94, _S45 = ((995010110, 90), (995010120, 91), (995010140, 93),
                                (995010220, 94), (994500000, 45))
STANDARD_DUNGEONS = {
    10: (90, 6109, (None, None, _S90, None, None, _S45)),
    20: (90, 6209, (None, None, _S90, _S91, None, _S45)),
    21: (90, 6219, (None, None, _S94, (991000000, 10), None, _S45)),
    30: (90, 6309, ((996100000, 61), (998500000, 85), _S90, _S91, None, _S45)),
    31: (90, 6319, (None, (998600000, 86), _S94, (995010230, 95), (991000000, 10), _S45)),
    40: (90, 6409, ((996200000, 62), (997000000, 70), _S90, _S91, _S93, _S45)),
    42: (90, 6429, (None, None, (995010340, 96), None, None, _S45)),
    50: (90, 6509, ((996200000, 62), (998700000, 87), _S90, _S91, _S93, _S45)),
    52: (90, 6529, (None, (998800000, 88), (995010340, 96), (995010350, 97), None, _S45)),
    53: (90, 6539, (_S90, (998900000, 89), (995010450, 98), _S91, _S94, _S45)),
}
# Their ChannelIds: this base plus the key, far above any stored dungeon's.
STANDARD_CHANNEL_BASE = 2000000000
STANDARD_CONDUCTOR = "Hunter"


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------

def now_epoch():
    return int(time.time())


def now_iso():
    # Matches the community server's CreateDate/LastPlayDate format.
    return datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")


def b64text(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def read_text(path):
    """Contents of a UTF-8 text file with normalised newlines, or ''."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            return f.read().replace("\r\n", "\n").replace("\r", "\n")
    except OSError:
        return ""


def load_broadcast(path):
    """broadcast.txt -> one emergency notice, or None when the file is empty.

    Same layout as notice.txt: first line the title, the rest the body. The
    entry shape is INFERRED -- the community server always returned an empty
    NoticeList, so this mirrors the normal notice, which uses the same field.
    """
    text = read_text(path).strip("\n")
    if not text.strip():
        return None
    title, _, body = text.partition("\n")
    digest = binascii.crc32(text.encode("utf-8"))
    return {"Title": b64text(title.strip()), "Notice": b64text(body.strip("\n")),
            "Id": digest % 1_000_000_000 or 1}, digest


def load_notice(path):
    """notice.txt -> NoticeList. First line is the title, the rest the body.

    Shown on the sign-in screen. The game asks for this once per login and
    drops a notice whose Id it has already displayed, so the Id is a hash of
    the text: editing the message makes it appear again, leaving it alone does
    not. There is no way to force a repeat -- the suppression is client-side.
    """
    text = read_text(path).strip("\n")
    if not text.strip():
        return []
    title, _, body = text.partition("\n")
    notice_id = binascii.crc32(text.encode("utf-8")) % 1_000_000_000
    return [{"Title": b64text(title.strip()), "Notice": b64text(body.strip("\n")), "Id": notice_id or 1}]


def stable_user_id(account):
    """Deterministic per-account id for the no-store mode."""
    return 1000 + (binascii.crc32(account.encode("utf-8")) % 9_000_000)


def as_int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def id_list(items, key):
    """[{'BloodMessId': 4833}, ...] -> [4833, ...], tolerating bare ints."""
    out = []
    for item in items or []:
        value = item.get(key) if isinstance(item, dict) else item
        if value is not None:
            out.append(as_int(value))
    return out


# How often (seconds) the game re-posts its summon sign and polls for signs. Retail sends 30/35,
# which makes every seamless (re)join wait a minute or more; a private server can afford 5.
# Every element comes once per region number (REGION_NUMBERS), the game reads its own.
REGION_NUMBERS = (1, 2, 3, 4)
SUMMON_POLL_TAGS = tuple("{}{}".format(tag, number) for tag in ("SummonDataCreateInterval", "SummonDataGetListInterval")
                         for number in REGION_NUMBERS)
# After a co-op session ends the host waits out these guard timers (retail 60 s) before it takes
# a new sign, which stalls every rejoin after a death. Same value as the poll interval.
COOP_GUARD_TAGS = tuple("{}{}".format(tag, number)
                        for tag in ("SummonDataGetListGuardTimer", "SummonDataGetListCoopGuardTimerClampTime")
                        for number in REGION_NUMBERS)


def build_ss_info(template_path, services_url, summon_url, summon_poll=5):
    with open(template_path, encoding="utf-8") as f:
        xml = f.read()
    missing = [p for p in ("{SERVICES}", "{SUMMON}") if p not in xml]
    if missing:
        sys.exit("template {} has no {} placeholder".format(template_path, " or ".join(missing)))
    xml = xml.replace("{SERVICES}", services_url).replace("{SUMMON}", summon_url)
    for tag in SUMMON_POLL_TAGS + COOP_GUARD_TAGS:
        xml = re.sub(r"<{0}>[^<]*</{0}>".format(tag), "<{0}>{1}</{0}>".format(tag, summon_poll), xml)
    if "thehuntersdream" in xml:
        sys.exit("template still references thehuntersdream.com; regenerate it")
    return base64.b64encode(xml.encode("utf-8"))


# --------------------------------------------------------------------------
# Content store (only used with --store)
# --------------------------------------------------------------------------

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    account       TEXT NOT NULL UNIQUE,       -- PlatformAccountId, lower-cased
    display       TEXT NOT NULL,              -- PlatformAccountId as sent
    shell_data    TEXT,
    shell_version INTEGER,
    created_at    INTEGER NOT NULL,
    last_login    INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS blood_messages (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    area_id            INTEGER NOT NULL,
    area_region_id     INTEGER NOT NULL,
    channel_id         INTEGER NOT NULL,
    blood_data         TEXT NOT NULL,
    blood_data_version INTEGER,
    chara_data         INTEGER,
    chara_data_version INTEGER,
    evaluate_plus      INTEGER NOT NULL DEFAULT 0,
    evaluate_minus     INTEGER NOT NULL DEFAULT 0,
    created_at         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_blood_area ON blood_messages(area_id, area_region_id, channel_id, created_at);
CREATE TABLE IF NOT EXISTS ratings (
    blood_mess_id INTEGER NOT NULL,
    user_id       INTEGER NOT NULL,
    kind          INTEGER NOT NULL,
    created_at    INTEGER NOT NULL,
    PRIMARY KEY (blood_mess_id, user_id)
);
CREATE TABLE IF NOT EXISTS tombstones (
    id                        INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id                   INTEGER NOT NULL,
    chara_id                  TEXT,
    area_id                   INTEGER NOT NULL,
    area_region_id            INTEGER NOT NULL,
    channel_id                INTEGER NOT NULL,
    tomb_data                 TEXT NOT NULL,
    tomb_data_version         INTEGER,
    death_vision_data         TEXT,
    death_vision_data_version INTEGER,
    created_at                INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_tomb_area ON tombstones(area_id, area_region_id, channel_id, created_at);
CREATE TABLE IF NOT EXISTS ghosts (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    area_id            INTEGER NOT NULL,
    area_region_id     INTEGER NOT NULL,
    channel_id         INTEGER NOT NULL,
    matching_level     INTEGER,
    ghost_data         TEXT NOT NULL,
    ghost_data_version INTEGER,
    created_at         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_ghost_area ON ghosts(area_id, area_region_id, channel_id, created_at);
CREATE TABLE IF NOT EXISTS lanterns (
    warp_info_id INTEGER NOT NULL,
    user_id      INTEGER NOT NULL,
    channel_id   INTEGER NOT NULL,
    seen_at      INTEGER NOT NULL,
    PRIMARY KEY (warp_info_id, user_id)
);
CREATE TABLE IF NOT EXISTS chalice_dungeons (
    channel_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id            INTEGER NOT NULL,
    chara_id           TEXT,
    form_data          TEXT NOT NULL,
    form_data_version  INTEGER,
    discernment_word   TEXT NOT NULL UNIQUE,
    holy_grail_type_id INTEGER,
    ritual_level       INTEGER,
    fixed_or_general   INTEGER,
    share_level        INTEGER NOT NULL DEFAULT 0,
    status             INTEGER,
    sub_feature_flag   INTEGER,
    turnout_level      INTEGER NOT NULL DEFAULT 0,
    unlock_flags       TEXT NOT NULL DEFAULT '[]',   -- JSON, as sent
    wish_materials     TEXT NOT NULL DEFAULT '[]',   -- JSON, as sent
    create_date        TEXT NOT NULL,
    last_play_date     TEXT NOT NULL
);
"""

LIST_SQL = ("SELECT t.*, u.shell_data, u.shell_version FROM {table} t "
            "LEFT JOIN users u ON u.user_id = t.user_id WHERE {where} "
            "ORDER BY t.created_at DESC, t.id DESC LIMIT ?")


def area_clause(area):
    """WHERE fragment for one AreaInfoList/AreaList entry (wildcards honoured)."""
    clauses, params = [], []
    area_id = as_int(area.get("AreaId"), -1)
    if area_id not in WILDCARD_AREA:
        clauses.append("t.area_id = ?")
        params.append(area_id)
    region = as_int(area.get("AreaRegionId"), -1)
    if region != -1:
        clauses.append("t.area_region_id = ?")
        params.append(region)
    channel = as_int(area.get("ChannelId"), -1)
    if channel != -1:
        clauses.append("t.channel_id = ?")
        params.append(channel)
    return (" AND ".join(clauses) or "1=1"), params


def shell_of(row):
    if row["shell_data"]:
        return row["shell_data"], row["shell_version"]
    return DEFAULT_SHELL


class Store:
    def __init__(self, path, show_own, ghost_ttl_s, lantern_ttl_s, tomb_cap):
        self.path = path
        self.show_own = set(show_own)
        self.ghost_ttl_s = ghost_ttl_s
        self.lantern_ttl_s = lantern_ttl_s
        self.tomb_cap = tomb_cap
        self.lock = threading.Lock()
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        with self.conn:
            self.conn.executescript(SCHEMA)

    def hide_own(self, kind):
        return kind not in self.show_own

    def counts(self):
        with self.lock:
            return {t: self.conn.execute("SELECT COUNT(*) FROM " + t).fetchone()[0]
                    for t in ("users", "blood_messages", "tombstones", "ghosts", "chalice_dungeons")}

    # -- users -------------------------------------------------------------

    def login(self, account):
        now = now_epoch()
        with self.lock, self.conn:
            self.conn.execute(
                "INSERT INTO users (account, display, created_at, last_login) VALUES (?,?,?,?) "
                "ON CONFLICT(account) DO UPDATE SET display=excluded.display, last_login=excluded.last_login",
                (account.lower(), account, now, now))
            return self.conn.execute("SELECT user_id FROM users WHERE account=?",
                                     (account.lower(),)).fetchone()[0]

    def set_shell(self, user_id, data, version):
        with self.lock, self.conn:
            self.conn.execute("UPDATE users SET shell_data=?, shell_version=? WHERE user_id=?",
                              (data, version, user_id))

    # -- shared area query -------------------------------------------------

    def _list_by_areas(self, table, areas, user_id, hide_own, get_max):
        out, seen = [], set()
        get_max = as_int(get_max, 0)
        with self.lock:
            for area in areas:
                if not isinstance(area, dict):
                    continue
                if get_max and len(out) >= get_max:
                    break
                where, params = area_clause(area)
                if hide_own:
                    where += " AND t.user_id != ?"
                    params.append(user_id)
                limit = as_int(area.get("GetCount"), 30) or 30
                if get_max:
                    limit = min(limit, get_max - len(out))
                for row in self.conn.execute(LIST_SQL.format(table=table, where=where), params + [limit]):
                    if row["id"] not in seen:  # overlapping/wildcard entries
                        seen.add(row["id"])
                        out.append(row)
        return out

    # -- blood messages ----------------------------------------------------

    @staticmethod
    def message_entry(row):
        shell, version = shell_of(row)
        return {"BloodMessId": row["id"], "BloodData": row["blood_data"],
                "BloodDataVersion": row["blood_data_version"],
                "EvaluatePlus": row["evaluate_plus"], "EvaluateMinus": row["evaluate_minus"],
                "MessShellInfo": shell, "MessShellInfoVersion": version}

    def create_messages(self, user_id, chara_id, entries):
        now, pairs = now_epoch(), []
        with self.lock, self.conn:
            for e in entries:
                if not isinstance(e, dict):
                    continue
                cur = self.conn.execute(
                    "INSERT INTO blood_messages (user_id, chara_id, area_id, area_region_id, channel_id, "
                    "blood_data, blood_data_version, chara_data, chara_data_version, created_at) "
                    "VALUES (?,?,?,?,?,?,?,?,?,?)",
                    (user_id, chara_id, as_int(e.get("AreaId"), -1), as_int(e.get("AreaRegionId"), -1),
                     as_int(e.get("ChannelId")), e.get("BloodData", ""), e.get("BloodDataVersion"),
                     1 if e.get("CharaData") else 0, e.get("CharaDataVersion"), now))
                pairs.append((cur.lastrowid, e.get("PrevBloodMessId")))
        return pairs

    def list_messages(self, user_id, areas, get_max):
        # Players see their own notes in the world, so never hide these.
        rows = self._list_by_areas("blood_messages", areas, user_id, False, get_max)
        return [self.message_entry(r) for r in rows]

    def evaluate(self, user_id, mess_id, kind):
        """EvaluateKind 1 = Fine (+), -1 = Foul (-). One vote per player."""
        column = {1: "evaluate_plus", -1: "evaluate_minus"}.get(as_int(kind))
        with self.lock, self.conn:
            if column:
                cur = self.conn.execute(
                    "INSERT OR IGNORE INTO ratings (blood_mess_id, user_id, kind, created_at) VALUES (?,?,?,?)",
                    (mess_id, user_id, as_int(kind), now_epoch()))
                if cur.rowcount:
                    self.conn.execute("UPDATE blood_messages SET {} = {} + 1 WHERE id=?".format(column, column),
                                      (mess_id,))
            row = self.conn.execute(LIST_SQL.format(table="blood_messages", where="t.id = ?"),
                                    (mess_id, 1)).fetchone()
        return self.message_entry(row) if row else None

    def delete_message(self, user_id, mess_id):
        with self.lock, self.conn:
            cur = self.conn.execute("DELETE FROM blood_messages WHERE id=? AND user_id=?", (mess_id, user_id))
            self.conn.execute("DELETE FROM ratings WHERE blood_mess_id=?", (mess_id,))
            return cur.rowcount > 0

    def lost_messages(self, ids):
        if not ids:
            return []
        with self.lock:
            marks = ",".join("?" * len(ids))
            present = {r[0] for r in self.conn.execute(
                "SELECT id FROM blood_messages WHERE id IN ({})".format(marks), ids)}
        return [i for i in ids if i not in present]

    def evaluations_for(self, ids):
        # INFERRED: never observed with content. Shape mirrors the message
        # entry's rating fields, for messages that have any rating at all.
        if not ids:
            return []
        with self.lock:
            marks = ",".join("?" * len(ids))
            rows = self.conn.execute(
                "SELECT id, evaluate_plus, evaluate_minus FROM blood_messages "
                "WHERE id IN ({}) AND evaluate_plus + evaluate_minus > 0".format(marks), ids).fetchall()
        return [{"BloodMessId": r["id"], "EvaluatePlus": r["evaluate_plus"],
                 "EvaluateMinus": r["evaluate_minus"]} for r in rows]

    # -- tombstones ----------------------------------------------------------

    def create_tomb(self, user_id, chara_id, req):
        key = (as_int(req.get("AreaId"), -1), as_int(req.get("AreaRegionId"), -1), as_int(req.get("ChannelId")))
        with self.lock, self.conn:
            cur = self.conn.execute(
                "INSERT INTO tombstones (user_id, chara_id, area_id, area_region_id, channel_id, tomb_data, "
                "tomb_data_version, death_vision_data, death_vision_data_version, created_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?)",
                (user_id, chara_id, *key, req.get("TombData", ""), req.get("TombDataVersion"),
                 req.get("DeathVisionData", ""), req.get("DeathVisionDataVersion"), now_epoch()))
            # Keep only the newest N bloodstains per area, like the game's own cap.
            self.conn.execute(
                "DELETE FROM tombstones WHERE area_id=? AND area_region_id=? AND channel_id=? AND id NOT IN ("
                "SELECT id FROM tombstones WHERE area_id=? AND area_region_id=? AND channel_id=? "
                "ORDER BY created_at DESC, id DESC LIMIT ?)", (*key, *key, self.tomb_cap))
            return cur.lastrowid

    def list_tombs(self, user_id, areas, get_max):
        rows = self._list_by_areas("tombstones", areas, user_id, self.hide_own("deaths"), get_max)
        return [{"TombMessId": r["id"], "TombData": r["tomb_data"], "MessShellInfo": shell_of(r)[0]}
                for r in rows]

    def death_vision(self, tomb_id):
        with self.lock:
            row = self.conn.execute(
                "SELECT death_vision_data, death_vision_data_version FROM tombstones WHERE id=?",
                (tomb_id,)).fetchone()
        return (row[0], row[1]) if row else None

    # -- wandering ghosts ----------------------------------------------------

    def create_ghost(self, user_id, chara_id, req):
        key = (as_int(req.get("AreaId"), -1), as_int(req.get("AreaRegionId"), -1), as_int(req.get("ChannelId")))
        now = now_epoch()
        with self.lock, self.conn:
            self.conn.execute("DELETE FROM ghosts WHERE created_at < ?", (now - self.ghost_ttl_s,))
            # One ghost per player per area: the game uploads a new snippet
            # every ~60s, and a friend should appear once, not 20 times.
            self.conn.execute("DELETE FROM ghosts WHERE user_id=? AND area_id=? AND area_region_id=? AND channel_id=?",
                              (user_id, *key))
            cur = self.conn.execute(
                "INSERT INTO ghosts (user_id, chara_id, area_id, area_region_id, channel_id, matching_level, "
                "ghost_data, ghost_data_version, created_at) VALUES (?,?,?,?,?,?,?,?,?)",
                (user_id, chara_id, *key, req.get("MatchingLevel"), req.get("WanderingGhostData", ""),
                 req.get("WanderingGhostDataVersion"), now))
            return cur.lastrowid

    def list_ghosts(self, user_id, areas, get_max):
        rows = self._list_by_areas("ghosts", areas, user_id, self.hide_own("ghosts"), get_max)
        cutoff = now_epoch() - self.ghost_ttl_s
        return [{"WanderingGhostId": r["id"], "WanderingGhostData": r["ghost_data"],
                 "WanderingGhostDataVersion": r["ghost_data_version"]}
                for r in rows if r["created_at"] >= cutoff]

    # -- lanterns ------------------------------------------------------------

    def touch_lantern(self, user_id, warp_id, channel_id):
        with self.lock, self.conn:
            self.conn.execute(
                "INSERT INTO lanterns (warp_info_id, user_id, channel_id, seen_at) VALUES (?,?,?,?) "
                "ON CONFLICT(warp_info_id, user_id) DO UPDATE SET seen_at=excluded.seen_at, channel_id=excluded.channel_id",
                (warp_id, user_id, channel_id, now_epoch()))

    def lantern_counts(self, user_id):
        # The community server answered with every lantern that had players,
        # not just the ones asked about, so this does the same.
        where, params = "seen_at >= ?", [now_epoch() - self.lantern_ttl_s]
        if self.hide_own("lanterns"):
            where += " AND user_id != ?"
            params.append(user_id)
        with self.lock:
            rows = self.conn.execute(
                "SELECT warp_info_id, COUNT(*) AS c FROM lanterns WHERE {} GROUP BY warp_info_id "
                "ORDER BY warp_info_id".format(where), params).fetchall()
        return [{"WarpInfoId": r["warp_info_id"], "ChairMessCount": r["c"]} for r in rows]

    # -- chalice dungeons ----------------------------------------------------

    @staticmethod
    def dungeon_record(row):
        chara = row["chara_id"]
        return {"ChannelId": row["channel_id"], "ShareLevel": row["share_level"], "Status": row["status"],
                "TurnoutLevel": row["turnout_level"], "FormDataVersion": row["form_data_version"],
                "FormData": row["form_data"], "DiscernmentWord": row["discernment_word"],
                "RitualLevel": row["ritual_level"], "HolyGrailTypeId": row["holy_grail_type_id"],
                "FixedOrGeneral": row["fixed_or_general"], "SubFeatureFlag": row["sub_feature_flag"],
                "CreateUserId": row["user_id"],
                "CreateCharaId": int(chara) if chara and chara.isdigit() else 0,
                "CreateDate": row["create_date"], "LastPlayDate": row["last_play_date"],
                "WishMaterialList": json.loads(row["wish_materials"]),
                "UnlockFlagList": json.loads(row["unlock_flags"])}

    def upload_dungeon(self, user_id, chara_id, req):
        stamp = now_iso()
        with self.lock, self.conn:
            for _ in range(50):
                glyph = "".join(secrets.choice(GLYPH_ALPHABET) for _ in range(8))
                if not self.conn.execute("SELECT 1 FROM chalice_dungeons WHERE discernment_word=?",
                                         (glyph,)).fetchone():
                    break
            cur = self.conn.execute(
                "INSERT INTO chalice_dungeons (user_id, chara_id, form_data, form_data_version, discernment_word, "
                "holy_grail_type_id, ritual_level, fixed_or_general, share_level, status, sub_feature_flag, "
                "unlock_flags, wish_materials, create_date, last_play_date) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (user_id, chara_id, req.get("FormData", ""), req.get("FormDataVersion"), glyph,
                 req.get("HolyGrailTypeId"), req.get("RitualLevel"), req.get("FixedOrGeneral"),
                 as_int(req.get("ShareLevel")), req.get("Status"), req.get("SubFeatureFlag"),
                 json.dumps(req.get("UnlockFlagList") or []), json.dumps(req.get("WishMaterialList") or []),
                 stamp, stamp))
            return cur.lastrowid, glyph

    def share_dungeon(self, user_id, channel_id, level):
        with self.lock, self.conn:
            cur = self.conn.execute("UPDATE chalice_dungeons SET share_level=? WHERE channel_id=? AND user_id=?",
                                    (as_int(level), channel_id, user_id))
            return cur.rowcount > 0

    def search_dungeons(self, req):
        """Shared dungeons matching the game's filters.

        FixedOrGeneralList is an "in" filter; HolyGrailTypeId, RitualLevel,
        SubFeatureFlag and Status match exactly when given and mean "any" when
        null (0 is a real value). UnlockedFlagList is deliberately ignored: its
        bitmask rule isn't fully known, and over-returning beats hiding a
        friend's dungeon.
        """
        where, params = ["share_level >= 1"], []
        kinds = id_list(req.get("FixedOrGeneralList"), "FixedOrGeneral")
        if kinds:
            where.append("fixed_or_general IN ({})".format(",".join("?" * len(kinds))))
            params.extend(kinds)
        for key, column in (("HolyGrailTypeId", "holy_grail_type_id"), ("RitualLevel", "ritual_level"),
                            ("SubFeatureFlag", "sub_feature_flag"), ("Status", "status")):
            if req.get(key) is not None:
                where.append(column + " = ?")
                params.append(as_int(req[key]))
        limit = as_int(req.get("GetCount"), 100) or 100
        with self.lock:
            rows = self.conn.execute(
                "SELECT * FROM chalice_dungeons WHERE {} ORDER BY channel_id DESC LIMIT ?".format(" AND ".join(where)),
                params + [limit]).fetchall()
        return [self.dungeon_record(r) for r in rows]

    def all_dungeons(self, level=None, area=None):
        """Every dungeon uploaded, newest first (of that depth and area when given)."""
        where, params = ["1"], []
        for value, column in ((level, "ritual_level"), (area, "holy_grail_type_id")):
            if value is not None:
                where.append(column + " = ?")
                params.append(as_int(value))
        with self.lock:
            rows = self.conn.execute(
                "SELECT * FROM chalice_dungeons WHERE {} ORDER BY channel_id DESC".format(" AND ".join(where)),
                params).fetchall()
        return [self.dungeon_record(r) for r in rows]

    def word_search(self, word):
        if not word:
            return None
        with self.lock:
            row = self.conn.execute("SELECT * FROM chalice_dungeons WHERE lower(discernment_word)=lower(?)",
                                    (str(word).strip(),)).fetchone()
        return self.dungeon_record(row) if row else None

    def dungeons_by_ids(self, ids):
        if not ids:
            return [], []
        with self.lock:
            marks = ",".join("?" * len(ids))
            rows = self.conn.execute("SELECT * FROM chalice_dungeons WHERE channel_id IN ({})".format(marks),
                                     ids).fetchall()
        found = {r["channel_id"]: self.dungeon_record(r) for r in rows}
        return [found[i] for i in ids if i in found], [i for i in ids if i not in found]


def standard_dungeon(key):
    """The record of the standard chalice dungeon `key` (RitualLevel * 10 + HolyGrailTypeId)."""
    if key not in STANDARD_DUNGEONS:
        return None
    rites, goods, slots = STANDARD_DUNGEONS[key]
    form = struct.pack("<II4BI", 5, 140, 0, rites, key, 0x1D, goods)
    for slot in range(9):
        feature = slots[slot] if slot < len(slots) else None
        form += struct.pack("<Ii", *feature) if feature else struct.pack("<ii", -1, -1)
    form += STANDARD_CONDUCTOR.encode("ascii").ljust(16, b"\0")
    form += STANDARD_CONDUCTOR.encode("utf-16-le").ljust(32, b"\0")
    form = form.ljust(140, b"\0")
    glyph = "chs" + GLYPH_ALPHABET[key // 32] + GLYPH_ALPHABET[key % 32]
    stamp = "2015-03-24T00:00:00"
    return {"ChannelId": STANDARD_CHANNEL_BASE + key, "ShareLevel": 2, "Status": 1,
            "TurnoutLevel": 0, "FormDataVersion": 0,
            "FormData": base64.b64encode(form).decode(), "DiscernmentWord": glyph,
            "RitualLevel": key // 10, "HolyGrailTypeId": key % 10, "FixedOrGeneral": 2,
            "SubFeatureFlag": 256, "CreateUserId": 0, "CreateCharaId": 0, "CreateDate": stamp,
            "LastPlayDate": stamp, "WishMaterialList": [], "UnlockFlagList": []}


def standard_search(req):
    """The standard chalice dungeon a ritual's ChannelSearch asks for, or None."""
    if id_list(req.get("FixedOrGeneralList"), "FixedOrGeneral") != [2]:
        return None
    level, area = req.get("RitualLevel"), req.get("HolyGrailTypeId")
    if type(level) is not int or type(area) is not int or not 0 <= area <= 9:
        return None
    return standard_dungeon(level * 10 + area)


def standard_details(req, payload, list_key):
    """Adds the standard dungeons among a request's ChannelIds to an answer's list."""
    ids = _ids(req, "ChannelIdList", "ChannelId")
    found = [standard_dungeon(i - STANDARD_CHANNEL_BASE) for i in ids]
    found = [d for d in found if d]
    if not found:
        return
    taken = {d["ChannelId"] for d in found}
    payload[list_key] = [d for d in payload.get(list_key, []) if d.get("ChannelId") not in taken] + found
    payload["LostChannelIdList"] = [e for e in payload.get("LostChannelIdList", [])
                                    if e.get("ChannelId") not in taken]


def is_dungeon_search(req):
    """Chalice Dungeon Search (FixedOrGeneral 0 among those asked): what dungeons are in use."""
    return 0 in id_list(req.get("FixedOrGeneralList"), "FixedOrGeneral")


def dungeon_search(req, used, store):
    """The dungeons in use on this server, newest first: the standard ones a ritual asked for
    (`used`, keys) and every one uploaded (shared or not: a team's dungeons are for the team),
    of the depth and area asked for when given."""
    level, area = req.get("RitualLevel"), req.get("HolyGrailTypeId")
    found = [standard_dungeon(key) for key in reversed(used)
             if (level is None or key // 10 == as_int(level)) and (area is None or key % 10 == as_int(area))]
    found += store.all_dungeons(level, area)
    return found[:as_int(req.get("GetCount"), 100) or 100]


# --------------------------------------------------------------------------
# Service handlers backed by the store. Each returns the response fields to
# add next to ResKind/MessageId. Names match the game's api_<Name> entries.
# --------------------------------------------------------------------------

def _ids(req, list_key, item_key):
    return id_list(req.get(list_key), item_key)


def h_MessengerShellUpload(store, req, uid, chara):
    store.set_shell(uid, req.get("ShellData"), req.get("ShellDataVersion"))
    return {}


def h_BloodMessCreate(store, req, uid, chara):
    pairs = store.create_messages(uid, chara, req.get("BloodMessList") or [])
    return {"BloodMessIdList": [{"BloodMessId": new_id, "PrevBloodMessId": prev} for new_id, prev in pairs]}


def h_BloodMessGetList(store, req, uid, chara):
    return {"BloodMessList": store.list_messages(uid, req.get("AreaInfoList") or [], req.get("GetMaxCount"))}


def h_BloodMessEvaluate(store, req, uid, chara):
    entry = store.evaluate(uid, as_int(req.get("BloodMessId"), -1), req.get("EvaluateKind"))
    return {"BloodMessList": [entry] if entry else []}


def h_BloodMessRemove(store, req, uid, chara):
    store.delete_message(uid, as_int(req.get("BloodMessId"), -1))
    return {}


def h_BloodMessSearchAdd(store, req, uid, chara):
    ids = _ids(req, "BloodMessIdList", "BloodMessId")
    # INFERRED: LostBloodMessIdList element shape mirrors the request's list.
    return {"BloodMessEvaluationList": store.evaluations_for(ids),
            "LostBloodMessIdList": [{"BloodMessId": i} for i in store.lost_messages(ids)]}


def h_BloodMessGetEvaluate(store, req, uid, chara):
    return {"BloodMessEvaluationList": store.evaluations_for(_ids(req, "BloodMessIdList", "BloodMessId"))}


def h_TombMessCreate(store, req, uid, chara):
    return {"TombMessId": store.create_tomb(uid, chara, req)}


def h_TombMessGetList(store, req, uid, chara):
    return {"TombMessList": store.list_tombs(uid, req.get("AreaInfoList") or [], req.get("GetMaxCount"))}


def h_DeathVisionGet(store, req, uid, chara):
    vision = store.death_vision(as_int(req.get("TombMessId"), -1))
    if vision:
        return {"DeathVisionData": vision[0], "DeathVisionDataVersion": vision[1]}
    return {"DeathVisionData": "", "DeathVisionDataVersion": req.get("DeathVisionDataVersion", 4)}


def h_WanderingGhostCreate(store, req, uid, chara):
    return {"WanderingGhostId": store.create_ghost(uid, chara, req)}


def h_WanderingGhostGet(store, req, uid, chara):
    return {"WanderingGhostList": store.list_ghosts(uid, req.get("AreaList") or [], req.get("GetMaxCount"))}


def h_ChairMessRespawnPointNotice(store, req, uid, chara):
    store.touch_lantern(uid, as_int(req.get("WarpInfoId"), -1), as_int(req.get("ChannelId")))
    return {}


def h_ChairMessGetList(store, req, uid, chara):
    return {"ChairMessList": store.lantern_counts(uid)}


def h_ChannelUpload(store, req, uid, chara):
    channel_id, glyph = store.upload_dungeon(uid, chara, req)
    return {"ChannelId": channel_id, "DiscernmentWord": glyph}


def h_ChannelShare(store, req, uid, chara):
    store.share_dungeon(uid, as_int(req.get("ChannelId"), -1), req.get("ShareLevel"))
    return {}


def h_ChannelSearch(store, req, uid, chara):
    return {"ChannelList": store.search_dungeons(req)}


def h_ChannelWordSearch(store, req, uid, chara):
    # The community server omitted ChannelList when nothing matched.
    # INFERRED: a hit uses the same record as ChannelSearch.
    record = store.word_search(req.get("SearchWord"))
    return {"ChannelList": [record]} if record else {}


def h_ChannelGetInfo(store, req, uid, chara):
    # INFERRED: ChannelInfoList entries never observed with content; full records used.
    found, lost = store.dungeons_by_ids(_ids(req, "ChannelIdList", "ChannelId"))
    return {"ChannelInfoList": found, "LostChannelIdList": [{"ChannelId": i} for i in lost]}


def h_ChannelGetDetailsInfo(store, req, uid, chara):
    found, lost = store.dungeons_by_ids(_ids(req, "ChannelIdList", "ChannelId"))
    return {"ChannelList": found, "LostChannelIdList": [{"ChannelId": i} for i in lost]}


STORE_HANDLERS = {name[2:]: fn for name, fn in globals().items() if name.startswith("h_")}

# Empty-content answers for the no-store mode. Names and shapes verbatim from
# the community server; the lists are simply empty here.
RESPONSE_FIELDS = {
    "UserPropertiesMoveCountCheckResponse": {},
    "MessengerShellUploadResponse": {},
    "ChannelGetInfoResponse": {"ChannelInfoList": [], "LostChannelIdList": []},
    "BloodMessGetListResponse": {"BloodMessList": []},
    "BloodMessSearchAddResponse": {"BloodMessEvaluationList": [], "LostBloodMessIdList": []},
    "BloodMessGetEvaluateResponse": {"BloodMessEvaluationList": []},
    "TombMessGetListResponse": {"TombMessList": []},
    "DeathVisionGetResponse": {"DeathVisionData": ""},
    "ChairMessGetListResponse": {"ChairMessList": []},
    "WanderingGhostGetResponse": {"WanderingGhostList": []},
}

# Create-type services in no-store mode still hand back an id so the game's
# own bookkeeping stays consistent. Field names taken from the captures.
CREATE_ID_FIELDS = {
    "BloodMessCreateResponse": "BloodMessId",
    "TombMessCreateResponse": "TombMessId",
    "WanderingGhostCreateResponse": "WanderingGhostId",
}

_next_created_id = 1000
_unknown_seen = set()

# (user_id, broadcast digest) already delivered. Kept in memory on purpose:
# CheckTime is in the *client's* local time, so comparing it against the host's
# clock breaks the moment a player is in another timezone. Tracking delivery by
# message content needs no clock at all, and a server restart re-sends -- which
# is what you want after "the server is back up".
_broadcast_sent = set()
_broadcast_lock = threading.Lock()


def take_broadcast(path, user_id):
    """The pending emergency notice for this player, or None if already sent."""
    loaded = load_broadcast(path)
    if not loaded:
        return None
    entry, digest = loaded
    with _broadcast_lock:
        if (user_id, digest) in _broadcast_sent:
            return None
        _broadcast_sent.add((user_id, digest))
    return entry


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

def address_from_host(value):
    """The IPv4 address a player reached this server at, from the request's Host header
    ("203.0.113.7:20443"); None for anything else (no header, a name, IPv6). ss.info sends each
    game back to that address: without a VPN the host's own address is its LAN one, which the
    guests can't reach (2026-10-21)."""
    match = re.fullmatch(r"(\d{1,3}(?:\.\d{1,3}){3})(?::\d{1,5})?", (value or "").strip())
    if not match:
        return None
    try:
        return str(ipaddress.IPv4Address(match.group(1)))
    except ValueError:
        return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "bb-dummy/2.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("[{}] {}\n".format(datetime.datetime.now().strftime("%H:%M:%S"), fmt % args))
        sys.stdout.flush()

    def _send(self, body, content_type, extra_headers=()):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra_headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, payload):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        # The retail service sets sp-res-kind alongside the in-body ResKind.
        self._send(body, "application/json", [("sp-res-kind", "0")])

    def _read_json(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length else b""
        try:
            parsed = json.loads(raw.decode("utf-8")) if raw else {}
        except (UnicodeDecodeError, ValueError):
            parsed = {}
        return parsed if isinstance(parsed, dict) else {}

    def do_GET(self):
        path = urlparse(self.path).path
        if is_ss_info_path(path):
            address = address_from_host(self.headers.get("Host")) or self.server.address
            ss_info = self.server.ss_info_for(address)
            self.log_message("ss.info for %s -> %d bytes", address, len(ss_info))
            self._send(ss_info, "text/plain; charset=utf-8")
            return
        if path == "/status":
            store = self.server.store
            self._send_json({"ok": True, "service": "bb-dummy",
                             "content": store.counts() if store else "off"})
            return
        self.log_message("unhandled GET %s", path)
        self.send_error(404)

    def do_POST(self):
        global _next_created_id
        path = urlparse(self.path).path
        request = self._read_json()
        store = self.server.store

        # Every service follows <Name>Request -> <Name>Response, so this also
        # covers the endpoints that were never exercised during capture.
        message_id = request.get("MessageId", "")
        name = message_id[:-7] if message_id.endswith("Request") else ""
        response_id = name + "Response" if name else ""

        if name == "Login":
            self._login(request)
            return

        payload = {"ResKind": 0, "MessageId": response_id}
        uid = as_int(request.get("UserId"), 0)
        chara = str(request.get("CharaId", ""))

        if name == "NoticeNormalGet":
            payload["NoticeList"] = load_notice(self.server.notice_path)
        elif name == "NoticeEmergencyGet":
            entry = take_broadcast(self.server.broadcast_path, uid)
            payload["NoticeList"] = [entry] if entry else []
            # The community server echoed the client's own CheckTime back
            # untouched, so do the same rather than moving its watermark.
            payload["CheckTime"] = request.get("CheckTime") or now_iso()
            if entry:
                self.log_message("broadcast delivered to UserId %s", uid)
        elif name == "SyncCharaId":
            # A save that meets a new account asks for CharaIdNum fresh character ids; without
            # them the game loops on "Failed to reset online character data". INFERRED shape
            # (field names from the eboot), confirmed working in game. Ids are derived from the
            # UserId so they are stable across restarts and never collide between users.
            count = max(1, min(as_int(request.get("CharaIdNum"), 1), 16))
            payload["PublishCharacterIdList"] = [{"PublishCharaId": uid * 16 + i + 1} for i in range(count)]
        elif name == "ChannelSearch" and standard_search(request):
            standard = standard_search(request)
            key = standard["ChannelId"] - STANDARD_CHANNEL_BASE
            with self.server.chalice_lock:
                if key in self.server.standard_used:
                    self.server.standard_used.remove(key)
                self.server.standard_used.append(key)
            payload["ChannelList"] = [standard]
        elif name == "ChannelSearch" and is_dungeon_search(request):
            with self.server.chalice_lock:
                used = list(self.server.standard_used)
            payload["ChannelList"] = dungeon_search(request, used, self.server.chalice_store)
        elif name.startswith("Channel") and name in STORE_HANDLERS:
            # Chalice dungeons are kept even without a store (in memory then): teammates join them.
            try:
                payload.update(STORE_HANDLERS[name](self.server.chalice_store, request, uid, chara))
            except Exception as exc:  # a store bug must never knock the game offline
                self.log_message("ERROR in %s: %r -- answered without content", name, exc)
        elif store and name in STORE_HANDLERS:
            try:
                payload.update(STORE_HANDLERS[name](store, request, uid, chara))
            except Exception as exc:  # a store bug must never knock the game offline
                self.log_message("ERROR in %s: %r -- answered without content", name, exc)
        elif response_id in RESPONSE_FIELDS:
            payload.update(RESPONSE_FIELDS[response_id])
        elif response_id in CREATE_ID_FIELDS:
            _next_created_id += 1
            payload[CREATE_ID_FIELDS[response_id]] = _next_created_id
        elif response_id:
            # Answer generically rather than 404, so an unexercised service can
            # never knock the game offline. Flagged once so it can be added.
            if response_id not in _unknown_seen:
                _unknown_seen.add(response_id)
                self.log_message("NOTE: unmapped service %s at %s -- answered generically", response_id, path)
        else:
            self.log_message("POST %s with no MessageId; answering ResKind 0", path)

        if name == "ChannelGetDetailsInfo":
            standard_details(request, payload, "ChannelList")
        elif name == "ChannelGetInfo":
            standard_details(request, payload, "ChannelInfoList")

        if os.environ.get("BB_TRACE_BODIES"):
            self.log_message("TRACE %s req=%s resp=%s", path, json.dumps(request)[:600], json.dumps(payload)[:300])
        sizes = ["{}={}".format(k, len(v)) for k, v in payload.items() if isinstance(v, list)]
        self.log_message("%s -> %s%s", path, response_id or "(generic)",
                         "  [{}]".format(", ".join(sizes)) if sizes else "")
        self._send_json(payload)

    def _login(self, request):
        account = str(request.get("PlatformAccountId") or "Hunter")
        store = self.server.store
        user_id = store.login(account) if store else stable_user_id(account)
        payload = {
            "ServerVersion": 1,
            "UserStatus": 0,
            "SessionId": str(uuid.uuid4()),
            "PlatformAccountId": account,
            "LanguageId": float(as_int(request.get("LanguageId"), 19)),
            "UserId": user_id,
            "MessageId": "LoginResponse",
            "WarningMessage": b64text(read_text(self.server.login_message_path).strip()),
            "IssuerId": float(as_int(request.get("IssuerId"), 100)),
            "ResKind": 0,
        }
        self.log_message("login: %s -> UserId %d", account, user_id)
        self._send_json(payload)


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def parse_show_own(value):
    chosen = {v.strip().lower() for v in (value or "").split(",") if v.strip()}
    if "all" in chosen:
        return set(SHOW_OWN_CHOICES)
    bad = chosen - set(SHOW_OWN_CHOICES)
    if bad:
        sys.exit("--show-own: unknown value(s) {}; choose from {} or all".format(
            ", ".join(sorted(bad)), ", ".join(SHOW_OWN_CHOICES)))
    return chosen


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--address", required=True,
                    help="this machine's address as the game sees it, e.g. 26.12.34.56")
    ap.add_argument("--port", type=int, default=20443)
    ap.add_argument("--shadnet-webapi", default=None,
                    help="shadNet WebAPI for summon traffic (default: http://<address>:31315)")
    ap.add_argument("--template", default=os.path.join(here, "ss_info_template.xml"))
    ap.add_argument("--summon-poll", type=int, default=5, metavar="SEC",
                    help="seconds between summon sign posts/searches (retail: 30/35; default 5)")
    ap.add_argument("--store", metavar="FILE", default=None,
                    help="keep messages, bloodstains, ghosts, lanterns and chalice dungeons in this SQLite file")
    ap.add_argument("--show-own", metavar="LIST", default="",
                    help="also show a player their own content: deaths, ghosts, lanterns, or all "
                         "(messages are always shown; default: none)")
    ap.add_argument("--ghost-ttl", type=int, default=30, metavar="MIN",
                    help="minutes a wandering ghost stays visible (default 30)")
    ap.add_argument("--lantern-ttl", type=int, default=10, metavar="MIN",
                    help="minutes a player counts as present at a lantern (default 10)")
    ap.add_argument("--tomb-cap", type=int, default=100, metavar="N",
                    help="bloodstains kept per area region (default 100)")
    ap.add_argument("--notice", default=os.path.join(here, "notice.txt"),
                    help="text file shown as the in-game notice: first line title, rest body")
    ap.add_argument("--broadcast", default=os.path.join(here, "broadcast.txt"),
                    help="text file sent as an in-game popup within 60s of being written; "
                         "same layout as --notice. Each distinct message reaches each player once")
    ap.add_argument("--login-message", default=os.path.join(here, "login_message.txt"),
                    help="text file shown as the login warning message")
    args = ap.parse_args()

    services_url = "http://{}:{}".format(args.address, args.port)
    summon_url = args.shadnet_webapi or "http://{}:31315".format(args.address)

    def ss_info_for(address):
        # Each game is sent back to the address it used (address_from_host), else --address.
        return build_ss_info(args.template, "http://{}:{}".format(address, args.port),
                             args.shadnet_webapi or "http://{}:31315".format(address),
                             max(1, args.summon_poll))

    # Everything that can fail is checked before the port is taken, so a bad
    # flag or an unwritable store path exits cleanly instead of half-starting.
    ss_info_for(args.address)
    store = None
    if args.store:
        try:
            store = Store(args.store, parse_show_own(args.show_own),
                          args.ghost_ttl * 60, args.lantern_ttl * 60, args.tomb_cap)
        except sqlite3.Error as exc:
            sys.exit("cannot open content store {}: {}".format(args.store, exc))

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    httpd.address = args.address
    httpd.ss_info_for = ss_info_for
    httpd.notice_path = args.notice
    httpd.broadcast_path = args.broadcast
    httpd.login_message_path = args.login_message
    httpd.store = store
    httpd.chalice_store = store or Store(":memory:", (), 0, 0, 0)  # chalice dungeons only
    httpd.chalice_lock = threading.Lock()
    httpd.standard_used = []

    print("bb-dummy listening on 0.0.0.0:{}".format(args.port))
    print("  ss.info      {}{}".format(services_url, SS_INFO_PATH))
    print("  services  -> {}".format(services_url))
    print("  summons   -> {}  (shadNet)".format(summon_url))
    if httpd.store:
        c = httpd.store.counts()
        print("  content   -> {}".format(os.path.abspath(args.store)))
        print("               {} players, {} messages, {} bloodstains, {} ghosts, {} chalice dungeons".format(
            c["users"], c["blood_messages"], c["tombstones"], c["ghosts"], c["chalice_dungeons"]))
        shown = sorted(httpd.store.show_own)
        print("               own content shown: {}".format(", ".join(shown) if shown else "messages only"))
    else:
        print("  content   -> off (world stays empty)")
    if not load_notice(args.notice):
        print("  notice    -> none ({} is empty)".format(args.notice))
    else:
        print("  notice    -> {}  (sign-in screen; shows again when edited)".format(args.notice))
    pending = load_broadcast(args.broadcast)
    print("  broadcast -> {}{}".format(args.broadcast, "  (message waiting)" if pending else "  (empty)"))
    print("\nhost_overrides.json:")
    print('  {{"https://ss4.scej-network.jp:20443": "{}"}}'.format(services_url))
    print("\nLeave this running for the whole session (ss.info refetches every 300s).")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
