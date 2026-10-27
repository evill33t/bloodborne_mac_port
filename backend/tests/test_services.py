"""Service-level tests for bb_dummy_server: start it on a free port and talk HTTP to it."""

import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "bb_dummy_server.py")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, "-u", SERVER, "--address", "127.0.0.1", "--port", str(cls.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://127.0.0.1:{}/status".format(cls.port), timeout=1)
                return
            except OSError:
                time.sleep(0.1)
        cls.proc.kill()
        raise RuntimeError("server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait()

    def post(self, path, body):
        req = urllib.request.Request("http://127.0.0.1:{}{}".format(self.port, path),
                                     data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())

    def sync(self, user_id, count):
        return self.post("/basic_utils/sync_chara_id?user_id={}".format(user_id),
                         {"CharaIdNum": count, "MessageId": "SyncCharaIdRequest",
                          "SessionId": "s", "UserId": user_id})

    # A save that meets a new account asks for fresh character ids. Without them in
    # PublishCharacterIdList the game shows "Failed to reset online character data" forever.
    def test_sync_chara_id_publishes_requested_ids(self):
        resp = self.sync(3841171, 1)
        self.assertEqual(resp["MessageId"], "SyncCharaIdResponse")
        self.assertEqual(resp["ResKind"], 0)
        ids = resp["PublishCharacterIdList"]
        self.assertEqual(len(ids), 1)
        self.assertIsInstance(ids[0]["PublishCharaId"], int)

    def test_sync_chara_id_is_stable_and_unique_per_user(self):
        a1 = [e["PublishCharaId"] for e in self.sync(1001, 2)["PublishCharacterIdList"]]
        a2 = [e["PublishCharaId"] for e in self.sync(1001, 2)["PublishCharacterIdList"]]
        b = [e["PublishCharaId"] for e in self.sync(1002, 2)["PublishCharacterIdList"]]
        self.assertEqual(a1, a2)
        self.assertEqual(len(set(a1)), 2)
        self.assertFalse(set(a1) & set(b))

    # A standard chalice's ritual asks for the one dungeon the server keeps for that chalice
    # (ChannelSearch, FixedOrGeneral 2, GetCount 1) and gives up without one: "Failed to
    # contribute ritual materials", then back to the title (rig 2026-10-17).
    PTHUMERU_SEARCH = {"FixedOrGeneralList": [{"FixedOrGeneral": 2}], "FormDataVersion": 0,
                       "GetCount": 1, "HolyGrailTypeId": 0, "MessageId": "ChannelSearchRequest",
                       "RitualLevel": 1, "SessionId": "s", "Status": 1, "SubFeatureFlag": 256,
                       "UnlockedFlagList": [{"UnlockedFlag": 0}, {"UnlockedFlag": 1},
                                            {"UnlockedFlag": 0}], "UserId": 1}
    # The Pthumeru Chalice's dungeon as the game uploads dungeons (format 5, 140 bytes, little
    # endian): depth 1 in Pthumeru, layout 0, rites 90, the chalice (goods 6109), its feature
    # slots as the game's HolygrailExParam 6109 has them (slot 2: 995010110, code 90; slot 5:
    # 994500000, code 45), conductor "Hunter". A join through Chalice Dungeon Search reads only
    # this (rig 2026-10-18: the old layout showed Depth 0 and died in the load).
    PTHUMERU_FORM = ("050000008c000000005a0a1ddd170000" + "ff" * 16 + "3ea64e3b5a000000" +
                     "ff" * 16 + "a0dd463b2d000000" + "ff" * 24 +
                     "48756e74657200000000000000000000" "480075006e007400650072" + "00" * 25)

    def test_standard_chalice_search_finds_its_dungeon(self):
        found = self.post("/channel/search?user_id=1", self.PTHUMERU_SEARCH)["ChannelList"]
        self.assertEqual(len(found), 1)
        dungeon = found[0]
        self.assertEqual(base64.b64decode(dungeon["FormData"]).hex(), self.PTHUMERU_FORM)
        self.assertEqual((dungeon["RitualLevel"], dungeon["HolyGrailTypeId"],
                          dungeon["FixedOrGeneral"], dungeon["Status"]), (1, 0, 2, 1))

    def test_standard_chalices_build_their_story_layouts(self):
        # A ritual builds its dungeon from the record's rites byte (FormData byte 9), even with the
        # chalice in hand: rites 0 gave the story Pthumeru Chalice the root layout m29_10_00_00
        # and its boss the root loot; 90 opens m29_10_90_00 (rig BB_FILE_TRACE, 2026-10-21).
        # Every standard chalice has its one m29_<key>_90_00 in the game's event folder.
        for key in (10, 20, 21, 30, 31, 40, 42, 50, 52, 53):
            search = dict(self.PTHUMERU_SEARCH, RitualLevel=key // 10, HolyGrailTypeId=key % 10)
            found = self.post("/channel/search?user_id=1", search)["ChannelList"]
            self.assertEqual(len(found), 1, key)
            form = base64.b64decode(found[0]["FormData"])
            self.assertEqual((form[8], form[9], form[10]), (0, 90, key), key)

    def test_standard_chalices_have_their_own_features(self):
        # Kaniel's playtest 2026-10-25: the story Pthumeru Chalice's last boss gave no chalice.
        # The ritual sets the altar's flag 5300 + each slot code's DungeonFeatureParam flag, and
        # the reward (m29 event 12901693) needs 5378 (codes 90, 94, 96, 98) or 5385 (code 85);
        # the root slots (9910030/35/50, codes 0x23 and 0x32) set neither.
        for key in (10, 20, 21, 30, 31, 40, 42, 50, 52, 53):
            search = dict(self.PTHUMERU_SEARCH, RitualLevel=key // 10, HolyGrailTypeId=key % 10)
            form = base64.b64decode(self.post("/channel/search?user_id=1", search)
                                    ["ChannelList"][0]["FormData"])
            slots = [struct.unpack_from("<Ii", form, 16 + slot * 8) for slot in range(9)]
            codes = {code for _, code in slots}
            self.assertTrue(codes & {85, 90, 94, 96, 98}, key)
            self.assertEqual(slots[5], (994500000, 45), key)
            self.assertFalse(any(9900000 <= ident < 10000000 for ident, _ in slots), key)

    def test_standard_chalice_dungeon_details(self):
        found = self.post("/channel/search?user_id=1", self.PTHUMERU_SEARCH)["ChannelList"]
        details = self.post("/channel/get_details_info?user_id=1",
                            {"ChannelIdList": [{"ChannelId": found[0]["ChannelId"]}],
                             "MessageId": "ChannelGetDetailsInfoRequest", "SessionId": "s",
                             "UserId": 1})
        self.assertEqual(details["ChannelList"], found)
        self.assertEqual(details["LostChannelIdList"], [])

    def test_other_searches_find_no_made_up_dungeon(self):
        # Chalice Dungeon Search (FixedOrGeneral 1, other players' dungeons) and an unknown
        # chalice stay empty.
        other = dict(self.PTHUMERU_SEARCH, FixedOrGeneralList=[{"FixedOrGeneral": 1}])
        self.assertEqual(self.post("/channel/search?user_id=1", other).get("ChannelList", []), [])
        unknown = dict(self.PTHUMERU_SEARCH, RitualLevel=9)
        self.assertEqual(self.post("/channel/search?user_id=1", unknown).get("ChannelList", []), [])


class ChaliceDungeonSearchTest(ServiceTest):
    """Chalice Dungeon Search (FixedOrGeneral 0 and 2, up to 100) lists the dungeons in use on
    this server: a teammate joins one there, free, instead of conducting its ritual. Its own
    server: what is in use depends on the requests before. Tests run in name order."""

    SEARCH = {"FixedOrGeneralList": [{"FixedOrGeneral": 0}, {"FixedOrGeneral": 2}],
              "FormDataVersion": 0, "GetCount": 100, "HolyGrailTypeId": None,
              "MessageId": "ChannelSearchRequest", "RitualLevel": None, "SessionId": "s",
              "Status": 1, "SubFeatureFlag": None,
              "UnlockedFlagList": [{"UnlockedFlag": 0}, {"UnlockedFlag": 1}, {"UnlockedFlag": 0}],
              "UserId": 2}
    UPLOAD = {"CharaId": 61458737, "FixedOrGeneral": 1, "FormData": "BQAAAIwAAAA=",
              "FormDataVersion": 0, "HolyGrailTypeId": 0, "MessageId": "ChannelUploadRequest",
              "RitualLevel": 1, "SessionId": "s", "ShareLevel": 0, "Status": 1,
              "SubFeatureFlag": 0, "UnlockFlagList": [{"UnlockFlag": 0}], "UserId": 1}

    def search(self, **changes):
        return self.post("/channel/search?user_id=2",
                         dict(self.SEARCH, **changes)).get("ChannelList", [])

    def test_a_nothing_in_use_finds_nothing(self):
        self.assertEqual(self.search(), [])

    def test_b_a_ritual_puts_its_dungeon_in_the_search(self):
        ritual = self.post("/channel/search?user_id=1", ServiceTest.PTHUMERU_SEARCH)["ChannelList"]
        self.assertEqual(self.search(), ritual)
        self.assertEqual(self.search(RitualLevel=1, HolyGrailTypeId=0), ritual)
        self.assertEqual(self.search(RitualLevel=2), [], "another depth")
        self.assertEqual(self.search(HolyGrailTypeId=1), [], "another area")

    def test_c_an_upload_is_found_and_joined_without_a_store(self):
        uploaded = self.post("/channel/upload?user_id=1", self.UPLOAD)
        channel = uploaded["ChannelId"]
        found = [d for d in self.search() if d["ChannelId"] == channel]
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["FormData"], "BQAAAIwAAAA=")
        self.assertEqual(found[0]["DiscernmentWord"], uploaded["DiscernmentWord"])
        details = self.post("/channel/get_details_info?user_id=2",
                            {"ChannelIdList": [{"ChannelId": channel}],
                             "MessageId": "ChannelGetDetailsInfoRequest", "SessionId": "s",
                             "UserId": 2})
        self.assertEqual(details["ChannelList"], found)
        self.assertEqual(details["LostChannelIdList"], [])


class SsInfoTest(unittest.TestCase):
    """ss.info drives how often the game polls for summons; retail's 30-35 s makes every
    (re)join take a minute or more, which defeats seamless auto-rejoin."""

    def fetch_ss_info(self, extra_args, address="127.0.0.1", host=None, path="/bb-eu/ss.info"):
        port = free_port()
        self.port = port
        proc = subprocess.Popen(
            [sys.executable, "-u", SERVER, "--address", address, "--port", str(port)] + extra_args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 10
            while True:
                try:
                    request = urllib.request.Request("http://127.0.0.1:{}{}".format(port, path))
                    if host is not None:
                        request.add_header("Host", host)
                    with urllib.request.urlopen(request, timeout=1) as r:
                        return base64.b64decode(r.read()).decode("utf-8")
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.1)
        finally:
            proc.kill()
            proc.wait()

    @staticmethod
    def tag(xml, name):
        match = re.search(r"<{0}>([^<]*)</{0}>".format(name), xml)
        return match.group(1) if match else None

    def test_summon_polling_is_fast_by_default(self):
        xml = self.fetch_ss_info([])
        self.assertEqual(self.tag(xml, "SummonDataCreateInterval2"), "5")
        self.assertEqual(self.tag(xml, "SummonDataGetListInterval2"), "5")

    def test_coop_guard_timers_do_not_stall_rejoin(self):
        # After a session ends the host waits out these guard timers (retail 60 s) before it
        # takes a new sign, so a guest who died could not rejoin for over a minute.
        xml = self.fetch_ss_info([])
        self.assertEqual(self.tag(xml, "SummonDataGetListGuardTimer2"), "5")
        self.assertEqual(self.tag(xml, "SummonDataGetListCoopGuardTimerClampTime2"), "5")
        # Invasion timers are not ours to touch.
        self.assertEqual(self.tag(xml, "SummonDataGetListInvationGuardTimerClampTime2"), "0")

    def test_each_game_is_sent_back_to_the_address_it_used(self):
        # Without a VPN the host's address is its LAN one (192.168.x); a guest over the internet
        # reached the host at its public address, and ss.info must send its game there too, or the
        # guest's game can't get online and never joins the summon session (2026-10-21).
        xml = self.fetch_ss_info([], address="192.168.4.25", host="203.0.113.7:20443")
        self.assertIn("http://203.0.113.7:{}".format(self.port), xml)
        self.assertIn("http://203.0.113.7:31315", xml)
        self.assertNotIn("192.168.4.25", xml)
        xml = self.fetch_ss_info([], address="192.168.4.25", host="26.1.2.3:20443")
        self.assertIn("http://26.1.2.3:31315", xml)
        # A name (the game's own host) or no usable address: the one the server was given.
        xml = self.fetch_ss_info([], address="192.168.4.25", host="ss4.scej-network.jp:20443")
        self.assertIn("http://192.168.4.25:31315", xml)
        self.assertNotIn("http://ss4.scej-network.jp", xml)

    def test_every_regions_game_gets_ss_info(self):
        # Players 2026-10-22: a US game (CUSA03179) asks /bb-us/ss.info and got 404 from a server that
        # knew only the EU path, so the host's game said it failed to acquire the server information.
        def fetch(path):  # each fetch has its own server port
            return self.fetch_ss_info([], path=path).replace(":{}<".format(self.port), ":PORT<")
        eu = fetch("/bb-eu/ss.info")
        self.assertIn(":PORT<", eu)
        for region in ("us", "jp", "asia"):
            self.assertEqual(fetch("/bb-{}/ss.info".format(region)), eu, region)
        from importlib import util
        spec = util.spec_from_file_location("bb_dummy_server", SERVER)
        server = util.module_from_spec(spec)
        spec.loader.exec_module(server)
        for path in ("/bb-eu/ss.info", "/bb-us/ss.info", "/bb-jp/ss.info", "/bb-asia/ss.info"):
            self.assertTrue(server.is_ss_info_path(path), path)
        for path in ("/ss.info", "/bb-/ss.info", "/bb-us/ss.infox", "/bb-us/x/ss.info", "/bb-US1/ss.info",
                     "/bb-eu/ss.info/", "/basic_utils/login"):
            self.assertFalse(server.is_ss_info_path(path), path)

    def test_every_game_language_finds_its_entries(self):
        # The game reads <msg2> and <BetaTestWebUrl2> by its language's <langN>, N from the eboot's
        # table at 0x4736570 (0 to 21). The template had 14 of them; a game whose language had none
        # (Unlock Game Region makes every language selectable) said it could not set the server
        # status (player log 2026-10-22: ss.info 200, then no login).
        xml = self.fetch_ss_info([])
        for section in ("msg2", "BetaTestWebUrl2"):
            body = re.search(r"<{0}>(.*?)</{0}>".format(section), xml, re.S).group(1)
            for lang in range(22):
                self.assertIsNotNone(self.tag(body, "lang{}".format(lang)), "{} lang{}".format(section, lang))
    def test_every_regions_game_finds_its_sections(self):
        # The game reads ss.info elements named with its region's number (eboot: 0x1489330 puts
        # table 0x4736550[region] at +0x9e8; JP 3, US 1, EU 2, Asia 4, EU 2, the Unlock Game Region
        # patch's region 3). The template only had the EU "2" ones, so a US game got ss.info and
        # then said it failed to resolve the server status (player log 2026-10-22).
        xml = self.fetch_ss_info([])
        names = sorted(set(re.findall(r"^<([A-Za-z_]+)2>", xml, re.M)) - {"lang"})  # <langN> sit inside others
        self.assertIn("gameurl", names)
        for number in (1, 3, 4):
            for name in names:
                self.assertEqual(xml.count("<{}{}>".format(name, number)), 1, "{}{}".format(name, number))
            gameurl = re.search(r"<gameurl{0}>(.*?)</gameurl{0}>".format(number), xml, re.S).group(1)
            self.assertIn("<api_Login>http://", gameurl)
            for tag in ("SummonDataCreateInterval", "SummonDataGetListInterval", "SummonDataGetListGuardTimer",
                        "SummonDataGetListCoopGuardTimerClampTime"):
                self.assertEqual(self.tag(xml, "{}{}".format(tag, number)), "5", "{}{}".format(tag, number))
        xml = self.fetch_ss_info(["--summon-poll", "12"])
        for number in (1, 2, 3, 4):
            self.assertEqual(self.tag(xml, "SummonDataGetListInterval{}".format(number)), "12")

    def test_the_host_header_is_read_as_an_address_only(self):
        from importlib import util
        spec = util.spec_from_file_location("bb_dummy_server", SERVER)
        server = util.module_from_spec(spec)
        spec.loader.exec_module(server)
        self.assertEqual(server.address_from_host("203.0.113.7:20443"), "203.0.113.7")
        self.assertEqual(server.address_from_host("26.1.2.3"), "26.1.2.3")
        for value in (None, "", "ss4.scej-network.jp:20443", "999.1.2.3:20443", "[::1]:20443",
                      "1.2.3.4:20443/evil", "1.2.3.4 :20443", "1.2.3.4@x"):
            self.assertIsNone(server.address_from_host(value), value)

    def test_summon_polling_interval_is_configurable(self):
        xml = self.fetch_ss_info(["--summon-poll", "12"])
        self.assertEqual(self.tag(xml, "SummonDataCreateInterval2"), "12")
        self.assertEqual(self.tag(xml, "SummonDataGetListInterval2"), "12")
        # Everything else in the template is left alone.
        self.assertEqual(self.tag(xml, "ReloadServerStatusInfoInterval2"), "300")


if __name__ == "__main__":
    unittest.main()
