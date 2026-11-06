# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Global server's own accounts (password_accounts.h, webapi_routes_password.cpp) against a real
shadnet: signup with its work, sign in, one answer for a wrong password and a name nobody has,
growing waits and never a lock, a rehash when the parameters change, weak and common passwords
refused, recovery, a password change and a sign out killing the older sign-ins, and no password in
the server's log.

Needs a built shadnet: SHADNET_EXE=<path to shadnet>. Skipped without it."""

import contextlib
import hashlib
import http.client
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest

SHADNET = os.environ.get("SHADNET_EXE", "")
WORK_BITS = 6
PASSWORD = "a long and fine password"


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def solve(challenge, name, bits):
    """The signup's work (password_accounts.h WorkDone), as the launcher does it."""
    prefix = b"bb signup v1|" + challenge.encode() + b"|" + name.lower().encode() + b"|"
    nonce = 0
    while True:
        digest = hashlib.sha256(prefix + nonce.to_bytes(8, "little")).digest()
        if int.from_bytes(digest, "big") >> (256 - bits) == 0:
            return nonce
        nonce += 1


class Server:
    def __init__(self, home, **settings):
        self.home = home
        self.webapi = free_port()
        os.makedirs(os.path.join(home, "db"), exist_ok=True)
        config = {"Host": "127.0.0.1", "UnsecuredPort": free_port(),
                  "MatchingUdpPort": free_port(socket.SOCK_DGRAM), "WebApiPort": self.webapi,
                  "StatsEnabled": "false", "BloodborneSeamlessCoop": "true",
                  "BloodborneBackend": "false", "Hosted": "true", "HostedSignupWorkBits": WORK_BITS,
                  "HostedNewAccountsPerHour": 1000, "PasswordTriesPerMinute": 1000,
                  "PasswordHashMemoryKiB": 8192, "PasswordHashPasses": 1}
        config.update(settings)
        with open(os.path.join(home, "shadnet.cfg"), "w", encoding="utf-8") as f:
            f.write("[General]\n")
            for key, value in config.items():
                f.write("{}={}\n".format(key, value))
        self.log_path = os.path.join(home, "shadnet.log")
        self.log = open(self.log_path, "ab")
        self.proc = subprocess.Popen([SHADNET], cwd=home, env=dict(os.environ, SHADNET_HOME=home),
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 20
        while time.time() < deadline:
            with contextlib.suppress(OSError):
                socket.create_connection(("127.0.0.1", self.webapi), timeout=0.5).close()
                return
            time.sleep(0.1)
        raise RuntimeError("shadnet did not start")

    def stop(self):
        self.proc.terminate()
        self.proc.wait(10)
        self.log.close()

    def call(self, method, path, body=None, raw=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.webapi, timeout=30)
        data = raw if raw is not None else (None if body is None else json.dumps(body))
        conn.request(method, path, data, {"Content-Type": "application/json"})
        answer = conn.getresponse()
        text = answer.read()
        conn.close()
        return answer.status, json.loads(text) if text[:1] == b"{" else text

    def sign_up(self, name, password=PASSWORD):
        status, work = self.call("POST", "/bb/auth/challenge", {})
        assert status == 200, work
        nonce = solve(work["challenge"], name, work["bits"])
        return self.call("POST", "/bb/auth/sign-up", {"name": name, "password": password,
                                                      "challenge": work["challenge"], "nonce": nonce})

    def sign_in(self, name, password=PASSWORD):
        return self.call("POST", "/bb/auth/sign-in", {"name": name, "password": password})

    def stored_hash(self, name):
        with contextlib.closing(sqlite3.connect(os.path.join(self.home, "db", "shadnet.db"))) as db:
            row = db.execute("SELECT hash FROM password_account WHERE name_key=?",
                             (name.lower(),)).fetchone()
        return row[0] if row else None


@unittest.skipUnless(SHADNET, "needs SHADNET_EXE")
class PasswordSignInTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="shadnet-password-")
        with open(os.path.join(self.home, "common-passwords.txt"), "w", encoding="utf-8") as f:
            f.write("yharnam-is-lovely\nhunter of hunters\n")
        self.server = Server(self.home)

    def tearDown(self):
        self.server.stop()
        with open(self.server.log_path, "rb") as f:
            log = f.read()
        # Never a password in the log, whatever happened.
        for secret in (PASSWORD, "a new and better password", "the third password here",
                       "not the password"):
            self.assertNotIn(secret.encode(), log)
        shutil.rmtree(self.home, ignore_errors=True)

    def test_the_config_says_password(self):
        status, config = self.server.call("GET", "/bb/auth/config")
        self.assertEqual(status, 200)
        self.assertEqual(config["sign_in"], ["password"])
        self.assertEqual((config["name_min"], config["name_max"]), (3, 20))
        self.assertEqual((config["password_min"], config["password_max"]), (10, 128))
        self.assertEqual(config["work_bits"], WORK_BITS)
        # Kaniel 2026-11-04: password only; Sign in with Discord is gone.
        self.assertEqual(self.server.call("GET", "/bb/discord/config")[0], 404)

    def test_a_signup_then_a_sign_in(self):
        status, account = self.server.sign_up("Kaniel")
        self.assertEqual(status, 200, account)
        self.assertRegex(account["id"], r"^u[0-9a-f]{14}$")
        self.assertRegex(account["secret"], r"^[0-9a-f]{32}$")
        self.assertEqual(account["name"], "Kaniel")
        self.assertRegex(account["recovery_code"], r"^([0-9A-Z]{4}-){4}[0-9A-Z]{4}$")
        self.assertNotIn("password", account)
        stored = self.server.stored_hash("kaniel")
        self.assertTrue(stored.startswith("$argon2id$v=19$m=8192,t=1,p=1$"))
        # From another PC: the same account, a new secret.
        status, again = self.server.sign_in("kANIEL")
        self.assertEqual(status, 200, again)
        self.assertEqual(again["id"], account["id"])
        self.assertEqual(again["name"], "Kaniel")
        # One account a name, whatever its case.
        self.assertEqual(self.server.sign_up("KANIEL", "another fine password")[0], 409)

    def test_a_signup_does_its_work_once(self):
        status, work = self.server.call("POST", "/bb/auth/challenge", {})
        nonce = solve(work["challenge"], "hunter", work["bits"])
        body = {"name": "hunter", "password": PASSWORD, "challenge": work["challenge"], "nonce": nonce}
        def done(n):
            digest = hashlib.sha256(b"bb signup v1|" + work["challenge"].encode() + b"|hunter|" +
                                    n.to_bytes(8, "little")).digest()
            return int.from_bytes(digest, "big") >> (256 - WORK_BITS) == 0
        bad = next(n for n in range(1000) if not done(n))
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-up", dict(body, nonce=bad))[1]["error"], "work")
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-up", dict(body, challenge="0" * 72))[0], 400)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-up", body)[0], 200)
        # The challenge made its account: never another.
        again = dict(body, name="hunter2")
        again["nonce"] = solve(work["challenge"], "hunter2", WORK_BITS)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-up", again)[1]["error"], "work")

    def test_weak_names_and_passwords_are_refused(self):
        def problem(name, password):
            status, answer = self.server.sign_up(name, password)
            self.assertEqual(status, 400, answer)
            return answer["error"], answer.get("problem")
        self.assertEqual(problem("ab", PASSWORD), ("name", "length"))
        self.assertEqual(problem("a" * 21, PASSWORD), ("name", "length"))
        self.assertEqual(problem("Kan iel", PASSWORD), ("name", "characters"))
        self.assertEqual(problem("Kaniél", PASSWORD), ("name", "characters"))
        self.assertEqual(problem("kaniel", "short one"), ("password", "short"))
        self.assertEqual(problem("kaniel", "x" * 129), ("password", "long"))
        self.assertEqual(problem("kaniel", "my kaniel password"), ("password", "name"))
        self.assertEqual(problem("kaniel", "password123"), ("password", "common"))  # built in
        self.assertEqual(problem("kaniel", "Hunter Of Hunters"), ("password", "common"))  # the file's
        self.assertEqual(self.server.sign_up("kaniel", "x" * 128)[0], 200)  # no other rules

    def test_one_answer_for_a_wrong_password_and_a_name_nobody_has(self):
        self.assertEqual(self.server.sign_up("kaniel")[0], 200)
        wrong = self.server.sign_in("kaniel", "not the password")
        nobody = self.server.sign_in("nobody-here", "not the password")
        self.assertEqual(wrong, nobody)
        self.assertEqual(wrong[0], 401)
        # The same work too: the name nobody has is checked against a hash all the same.
        def timed(name):
            start = time.perf_counter()
            self.server.sign_in(name, "not the password")
            return time.perf_counter() - start
        for name in ("kaniel", "nobody-here"):
            timed(name)
        known = min(timed("kaniel") for _ in range(2))
        unknown = min(timed("nobody-here") for _ in range(2))
        self.assertLess(abs(known - unknown), max(known, unknown) * 0.6 + 0.02)

    def test_wrong_passwords_slow_down_and_never_lock(self):
        self.assertEqual(self.server.sign_up("kaniel")[0], 200)
        for _ in range(5):
            self.assertEqual(self.server.sign_in("kaniel", "not the password")[0], 401)
        status, answer = self.server.sign_in("kaniel")  # even the right one waits now
        self.assertEqual(status, 429)
        self.assertEqual(answer["error"], "wait")
        self.assertGreaterEqual(answer["seconds"], 1)
        # A name nobody has waits the same.
        for _ in range(5):
            self.server.sign_in("nobody-here", "not the password")
        self.assertEqual(self.server.sign_in("nobody-here", "x")[1]["error"], "wait")
        time.sleep(answer["seconds"] + 0.2)
        status, account = self.server.sign_in("kaniel")  # the wait ends: no lock
        self.assertEqual(status, 200, account)

    def test_new_parameters_rehash_at_the_next_sign_in(self):
        self.assertEqual(self.server.sign_up("kaniel")[0], 200)
        old = self.server.stored_hash("kaniel")
        self.server.stop()
        self.server = Server(self.home, PasswordHashMemoryKiB=16384, PasswordHashPasses=2)
        self.assertEqual(self.server.stored_hash("kaniel"), old)
        self.assertEqual(self.server.sign_in("kaniel")[0], 200)
        new = self.server.stored_hash("kaniel")
        self.assertTrue(new.startswith("$argon2id$v=19$m=16384,t=2,p=1$"), new)
        self.assertEqual(self.server.sign_in("kaniel")[0], 200)

    def test_a_password_change_kills_the_older_sign_ins(self):
        status, first = self.server.sign_up("kaniel")
        status, second = self.server.sign_in("kaniel")
        status, changed = self.server.call("POST", "/bb/auth/change", {
            "name": "kaniel", "password": PASSWORD, "new_password": "a new and better password"})
        self.assertEqual(status, 200, changed)
        self.assertEqual(changed["id"], first["id"])
        for old in (first, second):
            self.assertEqual(self.server.call("POST", "/bb/auth/sign-out",
                                              {"id": old["id"], "secret": old["secret"]})[0], 403)
        self.assertEqual(self.server.sign_in("kaniel")[0], 401)
        self.assertEqual(self.server.sign_in("kaniel", "a new and better password")[0], 200)
        # A change needs the old password, and the new one keeps the rules.
        self.assertEqual(self.server.call("POST", "/bb/auth/change", {
            "name": "kaniel", "password": "not the password",
            "new_password": "the third password here"})[0], 401)
        self.assertEqual(self.server.call("POST", "/bb/auth/change", {
            "name": "kaniel", "password": "a new and better password",
            "new_password": "password123"})[1].get("problem"), "common")

    def test_a_recovery_code_gives_a_new_password_and_code(self):
        status, account = self.server.sign_up("kaniel")
        code = account["recovery_code"]
        recover = {"name": "kaniel", "recovery_code": code.lower().replace("-", " "),
                   "new_password": "a new and better password"}
        self.assertEqual(self.server.call("POST", "/bb/auth/recover",
                                          dict(recover, recovery_code="0000-0000-0000-0000-0000"))[0], 401)
        self.assertEqual(self.server.call("POST", "/bb/auth/recover",
                                          dict(recover, name="nobody-here"))[0], 401)
        status, recovered = self.server.call("POST", "/bb/auth/recover", recover)
        self.assertEqual(status, 200, recovered)
        self.assertEqual(recovered["id"], account["id"])
        self.assertNotEqual(recovered["recovery_code"], code)
        # The old code, the old password and the old sign-ins are dead.
        self.assertEqual(self.server.call("POST", "/bb/auth/recover", dict(
            recover, new_password="the third password here"))[0], 401)
        self.assertEqual(self.server.sign_in("kaniel")[0], 401)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-out", {
            "id": account["id"], "secret": account["secret"]})[0], 403)
        self.assertEqual(self.server.sign_in("kaniel", "a new and better password")[0], 200)

    def test_a_sign_out_kills_the_older_sign_ins(self):
        status, account = self.server.sign_up("kaniel")
        sign_out = {"id": account["id"], "secret": account["secret"]}
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-out", sign_out)[0], 200)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-out", sign_out)[0], 403)
        status, again = self.server.sign_in("kaniel")
        self.assertEqual(status, 200)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-out", {
            "id": again["id"], "secret": again["secret"]})[0], 200)  # a new one lives
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-out", {
            "id": account["id"], "secret": "0" * 32})[0], 403)

    def test_a_large_body_is_refused(self):
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-in", raw="x" * 5000)[0], 413)


@unittest.skipUnless(SHADNET, "needs SHADNET_EXE")
class SignInModeTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="shadnet-password-")

    def tearDown(self):
        self.server.stop()
        shutil.rmtree(self.home, ignore_errors=True)

    def test_behind_the_proxy_only_https_through_it(self):
        self.server = Server(self.home, SignInProxied="true")
        # Straight to the WebAPI's plain port (127.0.0.1 is inside, but no proxy said https).
        self.assertEqual(self.server.call("POST", "/bb/auth/challenge", {})[0], 403)
        conn = http.client.HTTPConnection("127.0.0.1", self.server.webapi, timeout=10)
        conn.request("POST", "/bb/auth/challenge", "{}", {"X-Forwarded-Proto": "https",
                                                          "X-Forwarded-For": "203.0.113.9"})
        self.assertEqual(conn.getresponse().status, 200)
        conn.close()

    def test_a_server_at_home_has_no_accounts(self):
        self.server = Server(self.home, Hosted="false")
        self.assertEqual(self.server.call("GET", "/bb/auth/config")[0], 404)

    def test_open_has_no_accounts(self):
        self.server = Server(self.home, SignIn="open")
        self.assertEqual(self.server.call("GET", "/bb/auth/config")[0], 404)

    def test_another_shard_than_the_home_only_says_how(self):
        self.server = Server(self.home, PasswordHome="false")
        self.assertEqual(self.server.call("GET", "/bb/auth/config")[0], 200)
        self.assertEqual(self.server.call("POST", "/bb/auth/sign-in", {"name": "a", "password": "b"})[0], 404)


if __name__ == "__main__":
    unittest.main()
