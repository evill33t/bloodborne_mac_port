"""The backend built into shadnet against bb_dummy_server.py: the same requests, the same bytes back.

shadnet answers the game's online services itself (BloodborneBackend, on by default). Nothing
may change for the game, so every service, with and without a content store, is sent to both servers and the
answers are compared byte for byte (status, Content-Type, sp-res-kind and body). Only what is
random or the clock's is masked: the login SessionId, chalice glyphs and their dates, and a
CheckTime the server stamps itself. A content store written by either server is then read by the
other.

Needs a built shadnet: SHADNET_EXE=<path to shadnet(.exe)>. Skipped without it.
"""

import http.client
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PYTHON_SERVER = os.path.join(HERE, "..", "bb_dummy_server.py")
SHADNET = os.environ.get("SHADNET_EXE", "")

NOTICE = "Welcome, hunter\nThe night is long.\r\nMind the beasts.\n"
LOGIN_MESSAGE = "  Private server \u2014 be kind  \n"
BROADCAST = "Server restart\nAt dawn."


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(port, proc, what):
    deadline = time.time() + 20
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("{} exited with {}".format(what, proc.returncode))
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/status")
            conn.getresponse().read()
            conn.close()
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("{} did not start".format(what))


class Server:
    """One backend: the Python script or shadnet, with the same options."""

    def __init__(self, kind, workdir, store=None, show_own="", tomb_cap=100, summon_poll=5,
                 address=None, flag="true"):
        self.kind = kind
        self.port = free_port()
        self.workdir = workdir
        os.makedirs(workdir, exist_ok=True)
        files = os.path.join(workdir, "files")
        os.makedirs(files, exist_ok=True)
        for name, text in (("notice.txt", NOTICE), ("login_message.txt", LOGIN_MESSAGE),
                           ("broadcast.txt", BROADCAST)):
            with open(os.path.join(files, name), "w", encoding="utf-8", newline="") as f:
                f.write(text)
        if kind == "python":
            self.webapi_port = 31315
            args = [sys.executable, "-u", PYTHON_SERVER, "--address", address or "127.0.0.1",
                    "--port", str(self.port), "--summon-poll", str(summon_poll),
                    "--tomb-cap", str(tomb_cap),
                    "--notice", os.path.join(files, "notice.txt"),
                    "--broadcast", os.path.join(files, "broadcast.txt"),
                    "--login-message", os.path.join(files, "login_message.txt")]
            if store:
                args += ["--store", store]
            if show_own:
                args += ["--show-own", show_own]
            self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            self.webapi_port = free_port()
            os.makedirs(os.path.join(workdir, "db"), exist_ok=True)
            config = {
                "Host": "127.0.0.1",
                "UnsecuredPort": free_port(),
                "MatchingUdpPort": free_port(),
                "WebApiPort": self.webapi_port,
                "StatsEnabled": "false",
                "BloodborneSeamlessCoop": "true",
                "BloodborneBackend": flag,
                "BackendPort": self.port,
                "BackendFolder": files,
                "BackendStore": store or "",
                "BackendShowOwn": show_own,
                "BackendTombCap": tomb_cap,
                "BackendSummonPoll": summon_poll,
            }
            with open(os.path.join(workdir, "shadnet.cfg"), "w", encoding="utf-8") as f:
                f.write("[General]\n")
                for key, value in config.items():
                    if value is None:
                        continue
                    f.write("{}={}\n".format(key, str(value).replace("\\", "/")))
            for name in ("worlds.cfg", "scoreboards.cfg"):
                source = os.path.join(os.path.dirname(SHADNET), name)
                if os.path.exists(source):
                    shutil.copy(source, workdir)
            env = dict(os.environ, SHADNET_HOME=workdir)
            env.pop("SHADNET_BACKEND_ADDRESS", None)
            if address:  # the kit's HOSTIP, as "HOST - Start Servers" passes it
                env["SHADNET_BACKEND_ADDRESS"] = address
            self.log = open(os.path.join(workdir, "shadnet.log"), "wb")
            self.proc = subprocess.Popen([SHADNET], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        try:
            wait_for(self.port, self.proc, kind)
        except RuntimeError:
            self.stop()
            raise

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if getattr(self, "log", None):
            self.log.close()

    def send(self, method, path, body=None, host=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = {"Host": host} if host else {}
        if body is not None:
            if not isinstance(body, bytes):
                body = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        result = (resp.status, resp.getheader("Content-Type"), resp.getheader("sp-res-kind"), data)
        conn.close()
        return result


VOLATILE = [
    (re.compile(rb'"SessionId":"[0-9a-f-]{36}"'), b'"SessionId":"<uuid>"'),
    (re.compile(rb'"DiscernmentWord":"[2-9a-km-np-z]{8}"'), b'"DiscernmentWord":"<glyph>"'),
    (re.compile(rb'"(CreateDate|LastPlayDate)":"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d"'), rb'"\1":"<date>"'),
    (re.compile(rb'"CheckTime":"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d"'), b'"CheckTime":"<now>"'),
]


def normalise(server, reply):
    status, content_type, res_kind, body = reply
    for pattern, mask in VOLATILE:
        body = pattern.sub(mask, body)
    if status != 200:  # an error page (the game never asks for one): only its status matters
        return status, None, None, b"<page>"
    return status, content_type, res_kind, body


def svc(name, **fields):
    body = {"MessageId": name + "Request", "SessionId": "s"}
    body.update(fields)
    return body


AREA = {"AreaId": 2012000, "AreaRegionId": 7, "ChannelId": 0}
OTHER_AREA = {"AreaId": 2412000, "AreaRegionId": 1, "ChannelId": 0}


def script():
    """Requests in order; a body may be a function of what this server answered before."""
    steps = [
        ("GET", "/status", None),
        ("GET", "/bb-eu/ss.info", None),
        ("GET", "/nothing-here", None),
        ("POST", "/login", svc("Login", PlatformAccountId="Alice", LanguageId=19, IssuerId=100)),
        ("POST", "/login", svc("Login", PlatformAccountId="bob", LanguageId="7")),
        ("POST", "/login", svc("Login", PlatformAccountId="", LanguageId=None, IssuerId=4.5)),
        ("POST", "/login", svc("Login", PlatformAccountId="Fran\u00e7ois\U0001F600")),
        ("POST", "/login", svc("Login", PlatformAccountId=12345)),
        ("POST", "/basic_utils/sync_chara_id", svc("SyncCharaId", UserId=1001, CharaIdNum=3)),
        ("POST", "/basic_utils/sync_chara_id", svc("SyncCharaId", UserId="1002", CharaIdNum=99)),
        ("POST", "/basic_utils/sync_chara_id", svc("SyncCharaId", UserId=1003, CharaIdNum=-4)),
        ("POST", "/notice", svc("NoticeNormalGet", UserId=1)),
        ("POST", "/notice", svc("NoticeEmergencyGet", UserId=1, CheckTime="2026-10-01T10:00:00")),
        ("POST", "/notice", svc("NoticeEmergencyGet", UserId=1, CheckTime="2026-10-01T10:00:00")),
        ("POST", "/notice", svc("NoticeEmergencyGet", UserId=2)),
        ("POST", "/notice", svc("NoticeEmergencyGet", UserId=3, CheckTime=1234)),
        ("POST", "/user", svc("UserPropertiesMoveCountCheck", UserId=1)),
        ("POST", "/nope", svc("SomethingNeverSeen", UserId=1)),
        ("POST", "/nope", svc("SomethingNeverSeen", UserId=1)),
        ("POST", "/raw", {"Hello": "no MessageId"}),
        ("POST", "/raw", b"not json at all"),
        ("POST", "/raw", b"[1, 2, 3]"),
        ("POST", "/raw", b""),
        ("POST", "/raw", b'{"MessageId": "Request"}'),
        ("POST", "/raw", b'\xef\xbb\xbf{"MessageId": "LoginRequest"}'),
        ("POST", "/raw", b'{"MessageId": "BloodMessGetListRequest", "UserId": 1, "AreaInfoList": [], "X": NaN}'),
        # Content: with a store these are kept; without one they get the empty answers.
        ("POST", "/shell", svc("MessengerShellUpload", UserId=11, ShellData="c2hlbGw=", ShellDataVersion=3)),
        ("POST", "/bm", svc("BloodMessCreate", UserId=11, CharaId=77, BloodMessList=[
            dict(AREA, BloodData="AAAA", BloodDataVersion=5, CharaData=1, CharaDataVersion=2,
                 PrevBloodMessId=0),
            dict(AREA, BloodData="BBBB", BloodDataVersion=5.0, CharaData=0, PrevBloodMessId=4),
            "not a dict",
            dict(OTHER_AREA, BloodData="CCCC", AreaId="2412000"),
        ])),
        ("POST", "/bm", svc("BloodMessCreate", UserId=12, CharaId="88", BloodMessList=[
            dict(AREA, BloodData="DDDD", BloodDataVersion=5)])),
        ("POST", "/bm", svc("BloodMessCreate", UserId=12, BloodMessList=[dict(AREA, BloodData=None)])),
        ("POST", "/bm", svc("BloodMessCreate", UserId=12, BloodMessList=7)),
        ("POST", "/bm", svc("BloodMessCreate", UserId=12, BloodMessList={"k": 1})),
        ("POST", "/bm", svc("BloodMessGetList", UserId=12, GetMaxCount=10,
                            AreaInfoList=[dict(AREA, GetCount=5), dict(AREA, GetCount=5)])),
        ("POST", "/bm", svc("BloodMessGetList", UserId=" 12 ", GetMaxCount=2,
                            AreaInfoList=[{"AreaId": -1, "AreaRegionId": -1, "ChannelId": -1}, OTHER_AREA])),
        ("POST", "/bm", svc("BloodMessGetList", UserId=12,
                            AreaInfoList=[{"AreaId": 4294967295}, "x", None])),
        ("POST", "/bm", svc("BloodMessEvaluate", UserId=12, BloodMessId=1, EvaluateKind=1)),
        ("POST", "/bm", svc("BloodMessEvaluate", UserId=12, BloodMessId=1, EvaluateKind=1)),
        ("POST", "/bm", svc("BloodMessEvaluate", UserId=13, BloodMessId="1", EvaluateKind="-1")),
        ("POST", "/bm", svc("BloodMessEvaluate", UserId=13, BloodMessId=2, EvaluateKind=0)),
        ("POST", "/bm", svc("BloodMessEvaluate", UserId=13, BloodMessId=999, EvaluateKind=1)),
        ("POST", "/bm", svc("BloodMessSearchAdd", UserId=12,
                            BloodMessIdList=[{"BloodMessId": 1}, 2, {"BloodMessId": 999}, {"Other": 1}, None])),
        ("POST", "/bm", svc("BloodMessSearchAdd", UserId=12, BloodMessIdList="12")),
        ("POST", "/bm", svc("BloodMessGetEvaluate", UserId=12, BloodMessIdList=[{"BloodMessId": 1}, {"BloodMessId": 3}])),
        ("POST", "/bm", svc("BloodMessGetEvaluate", UserId=12)),
        ("POST", "/bm", svc("BloodMessRemove", UserId=12, BloodMessId=1)),
        ("POST", "/bm", svc("BloodMessRemove", UserId=11, BloodMessId=1)),
        ("POST", "/bm", svc("BloodMessGetList", UserId=11, AreaInfoList=[AREA])),
        ("POST", "/tomb", svc("TombMessCreate", UserId=11, CharaId=77, TombData="T1", TombDataVersion=1,
                              DeathVisionData="V1", DeathVisionDataVersion=4, **AREA)),
        ("POST", "/tomb", svc("TombMessCreate", UserId=12, TombData="T2", DeathVisionData="V2", **AREA)),
        ("POST", "/tomb", svc("TombMessCreate", UserId=12, TombData="T3", **AREA)),
        ("POST", "/tomb", svc("TombMessCreate", UserId=12, TombData="T4", **AREA)),
        ("POST", "/tomb", svc("TombMessCreate", UserId=12, TombData="T5", **OTHER_AREA)),
        ("POST", "/tomb", svc("TombMessCreate", UserId=12, TombData=None, **AREA)),
        ("POST", "/tomb", svc("TombMessGetList", UserId=11, AreaInfoList=[AREA, OTHER_AREA])),
        ("POST", "/tomb", svc("TombMessGetList", UserId=12, GetMaxCount=1, AreaInfoList=[AREA, OTHER_AREA])),
        ("POST", "/tomb", svc("DeathVisionGet", UserId=12, TombMessId=2)),
        ("POST", "/tomb", svc("DeathVisionGet", UserId=12, TombMessId=999)),
        ("POST", "/tomb", svc("DeathVisionGet", UserId=12, TombMessId=999, DeathVisionDataVersion=None)),
        ("POST", "/ghost", svc("WanderingGhostCreate", UserId=11, CharaId=77, MatchingLevel=40,
                               WanderingGhostData="G1", WanderingGhostDataVersion=2, **AREA)),
        ("POST", "/ghost", svc("WanderingGhostCreate", UserId=11, WanderingGhostData="G1b", **AREA)),
        ("POST", "/ghost", svc("WanderingGhostCreate", UserId=12, WanderingGhostData="G2", **AREA)),
        ("POST", "/ghost", svc("WanderingGhostGet", UserId=11, AreaList=[AREA])),
        ("POST", "/ghost", svc("WanderingGhostGet", UserId=12, GetMaxCount=5,
                               AreaList=[{"AreaId": -1, "AreaRegionId": -1, "ChannelId": -1}])),
        ("POST", "/chair", svc("ChairMessRespawnPointNotice", UserId=11, WarpInfoId=2010100, ChannelId=0)),
        ("POST", "/chair", svc("ChairMessRespawnPointNotice", UserId=12, WarpInfoId=2010100, ChannelId=0)),
        ("POST", "/chair", svc("ChairMessRespawnPointNotice", UserId=12, WarpInfoId=2410000)),
        ("POST", "/chair", svc("ChairMessGetList", UserId=11)),
        ("POST", "/chair", svc("ChairMessGetList", UserId=13)),
        ("POST", "/chalice", svc("ChannelUpload", UserId=11, CharaId="77", FormData="F1", FormDataVersion=1,
                                 HolyGrailTypeId=3, RitualLevel=2, FixedOrGeneral=1, ShareLevel=0, Status=0,
                                 SubFeatureFlag=0, UnlockFlagList=[{"UnlockFlag": 1}],
                                 WishMaterialList=[{"MaterialId": 5, "Num": 2}])),
        ("POST", "/chalice", svc("ChannelUpload", UserId=12, CharaId="x9", FormData="F2", HolyGrailTypeId=3,
                                 RitualLevel=1, FixedOrGeneral=2, ShareLevel=1, Status=0, SubFeatureFlag=1)),
        ("POST", "/chalice", svc("ChannelShare", UserId=11, ChannelId=1, ShareLevel=2)),
        ("POST", "/chalice", svc("ChannelShare", UserId=12, ChannelId=1, ShareLevel=0)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[{"FixedOrGeneral": 1}, 2],
                                 HolyGrailTypeId=3, RitualLevel=None, GetCount=10)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, Status=0, SubFeatureFlag=1)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13)),
        ("POST", "/chalice", lambda glyphs: svc("ChannelWordSearch", UserId=13,
                                                SearchWord=" " + glyphs[0].upper() + " ")),
        ("POST", "/chalice", svc("ChannelWordSearch", UserId=13, SearchWord="zzzzzzzz")),
        ("POST", "/chalice", svc("ChannelWordSearch", UserId=13, SearchWord="")),
        ("POST", "/chalice", svc("ChannelGetInfo", UserId=13, ChannelIdList=[{"ChannelId": 2}, 1, 99, 2])),
        ("POST", "/chalice", svc("ChannelGetDetailsInfo", UserId=13, ChannelIdList=[{"ChannelId": 1}, 42])),
        ("POST", "/chalice", svc("ChannelGetInfo", UserId=13)),
        # A standard chalice's ritual: the server's own dungeon for it, then its details.
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[{"FixedOrGeneral": 2}],
                                 FormDataVersion=0, GetCount=1, HolyGrailTypeId=0, RitualLevel=1,
                                 Status=1, SubFeatureFlag=256)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[{"FixedOrGeneral": 2}],
                                 GetCount=1, HolyGrailTypeId=3, RitualLevel=5)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[{"FixedOrGeneral": 2}],
                                 GetCount=1, HolyGrailTypeId=1, RitualLevel=9)),
        ("POST", "/chalice", svc("ChannelGetDetailsInfo", UserId=13,
                                 ChannelIdList=[{"ChannelId": 2000000010}, 1, 2000000099])),
        ("POST", "/chalice", svc("ChannelGetInfo", UserId=13, ChannelIdList=[2000000053, 2])),
        # Chalice Dungeon Search: the dungeons in use (rituals above, then uploads), all of them,
        # of a depth and area, and only the first.
        ("POST", "/chalice", svc("ChannelSearch", UserId=13,
                                 FixedOrGeneralList=[{"FixedOrGeneral": 0}, {"FixedOrGeneral": 2}],
                                 GetCount=100, HolyGrailTypeId=None, RitualLevel=None, Status=1,
                                 SubFeatureFlag=None)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[0, 2],
                                 HolyGrailTypeId=3, RitualLevel=5)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[0], HolyGrailTypeId=3)),
        ("POST", "/chalice", svc("ChannelSearch", UserId=13, FixedOrGeneralList=[0], GetCount=1)),
        ("POST", "/login", svc("Login", PlatformAccountId="alice")),
        ("GET", "/status", None),
    ]
    return steps


def run_script(server):
    glyphs = []
    replies = []
    for method, path, body in script():
        if callable(body):
            body = body(glyphs) if glyphs else svc("ChannelWordSearch", SearchWord="none")
        reply = server.send(method, path, body)
        glyphs += [g.decode() for g in re.findall(rb'"DiscernmentWord":"([^"]+)"', reply[3])]
        replies.append(((method, path, body), reply))
    return replies


def mask_ports(server, reply):
    status, content_type, res_kind, body = reply
    if content_type and content_type.startswith("text/plain") and status == 200:
        import base64
        xml = base64.b64decode(body).decode("utf-8")
        xml = xml.replace(":{}".format(server.port), ":<backend>").replace(
            ":{}".format(server.webapi_port), ":<webapi>")
        body = xml.encode("utf-8")
    return status, content_type, res_kind, body


@unittest.skipUnless(SHADNET and os.path.exists(SHADNET), "set SHADNET_EXE to a built shadnet")
class ParityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bb-parity-")
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, kind, name, **options):
        server = Server(kind, os.path.join(self.tmp, name), **options)
        self.servers.append(server)
        return server

    def compare(self, python, shadnet):
        ours = run_script(shadnet)
        theirs = run_script(python)
        for (request, mine), (_, expected) in zip(ours, theirs):
            got = normalise(shadnet, mask_ports(shadnet, mine))
            want = normalise(python, mask_ports(python, expected))
            self.assertEqual(got, want, "different answers to {} {} {!r}".format(*request))

    def test_without_a_store(self):
        python = self.start("python", "py")
        shadnet = self.start("shadnet", "sn")
        self.compare(python, shadnet)

    def test_with_a_store(self):
        python = self.start("python", "py", store=os.path.join(self.tmp, "py.db"), tomb_cap=3)
        shadnet = self.start("shadnet", "sn", store=os.path.join(self.tmp, "sn.db"), tomb_cap=3)
        self.compare(python, shadnet)

    def test_with_own_content_shown(self):
        python = self.start("python", "py", store=os.path.join(self.tmp, "py.db"), show_own="all",
                            summon_poll=12)
        shadnet = self.start("shadnet", "sn", store=os.path.join(self.tmp, "sn.db"), show_own="all",
                             summon_poll=12)
        self.compare(python, shadnet)

    def test_on_without_the_setting(self):
        # An install's shadnet.cfg from before the setting: shadnet answers the services too.
        shadnet = self.start("shadnet", "sn", flag=None)
        self.assertEqual(shadnet.send("GET", "/status")[0], 200)
        with open(os.path.join(shadnet.workdir, "shadnet.cfg"), encoding="utf-8") as f:
            self.assertIn("BloodborneBackend=true", f.read())

    def test_the_hosts_address_is_the_one_given(self):
        # Port forwarding: the host's LAN address is not the one the guests reach, so the kit
        # names it (HOSTIP) and ss.info must send every game there.
        # A request that names no address (the game's own host name) gets the given one.
        python = self.start("python", "py", address="10.20.30.40")
        shadnet = self.start("shadnet", "sn", address="10.20.30.40")
        name = "ss4.scej-network.jp:20443"
        for server in (python, shadnet):
            reply = mask_ports(server, server.send("GET", "/bb-eu/ss.info", host=name))
            self.assertIn(b"http://10.20.30.40:<backend>", reply[3])
        self.assertEqual(normalise(shadnet, mask_ports(shadnet, shadnet.send("GET", "/bb-eu/ss.info", host=name))),
                         normalise(python, mask_ports(python, python.send("GET", "/bb-eu/ss.info", host=name))))

    def test_each_game_is_sent_back_to_the_address_it_used(self):
        # Without a VPN the host's address is its LAN one; a guest over the internet reached the
        # public one, and its ss.info must point there (2026-10-21: the guest stayed invisible).
        python = self.start("python", "py", address="192.168.4.25")
        shadnet = self.start("shadnet", "sn", address="192.168.4.25")
        used = "203.0.113.7:20443"
        for server in (python, shadnet):
            reply = mask_ports(server, server.send("GET", "/bb-eu/ss.info", host=used))
            self.assertIn(b"http://203.0.113.7:<backend>", reply[3])
            self.assertNotIn(b"192.168.4.25", reply[3])
        self.assertEqual(normalise(shadnet, mask_ports(shadnet, shadnet.send("GET", "/bb-eu/ss.info", host=used))),
                         normalise(python, mask_ports(python, python.send("GET", "/bb-eu/ss.info", host=used))))

    def test_a_store_moves_between_the_servers(self):
        # Each server writes a store, then both read each store: the same answers.
        reads = [
            svc("BloodMessGetList", UserId=13, AreaInfoList=[AREA, OTHER_AREA]),
            svc("TombMessGetList", UserId=13, AreaInfoList=[AREA, OTHER_AREA]),
            svc("WanderingGhostGet", UserId=13, AreaList=[AREA]),
            svc("ChairMessGetList", UserId=13),
            svc("ChannelSearch", UserId=13),
            svc("ChannelGetDetailsInfo", UserId=13, ChannelIdList=[1, 2]),
            svc("DeathVisionGet", UserId=13, TombMessId=1),
        ]
        for writer in ("python", "shadnet"):
            store = os.path.join(self.tmp, writer + ".db")
            server = self.start(writer, "w-" + writer, store=store)
            run_script(server)
            server.stop()
            self.servers.remove(server)
            answers = {}
            for reader in ("python", "shadnet"):
                copy = os.path.join(self.tmp, "{}-read-by-{}.db".format(writer, reader))
                for suffix in ("", "-wal", "-shm"):
                    if os.path.exists(store + suffix):
                        shutil.copy(store + suffix, copy + suffix)
                server = self.start(reader, "r-{}-{}".format(writer, reader), store=copy)
                answers[reader] = [normalise(server, server.send("POST", "/x", body)) for body in reads]
                server.stop()
                self.servers.remove(server)
            self.assertEqual(answers["shadnet"], answers["python"],
                             "a store written by {} reads differently".format(writer))


if __name__ == "__main__":
    unittest.main()
