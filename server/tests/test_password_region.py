# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A region's accounts with a password (docs/HOSTED_SERVER.md, "Global servers: accounts"): two
hosted shards with one link key and a directory (on fakeredis). The accounts' home makes the account;
the other shard lets its secret in at a real link hello with nothing asked of the home, refuses a new
id that signed in nowhere, and, a heartbeat or two after a sign out on the home, refuses the dead
secret too.

Needs a built shadnet (SHADNET_EXE) and redis-py with fakeredis[lua]; skipped without them."""
import contextlib
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / 'loadtest'), str(HERE.parent / 'directory')]
SHADNET = os.environ.get('SHADNET_EXE', '')
try:
    import fakeredis
    import directory as d
    import noise_link
    MISSING = None if SHADNET else 'needs SHADNET_EXE'
except ImportError as missing:
    MISSING = f'{missing.name} is not installed'

from test_password_signin import solve  # noqa: E402

TOKEN = 'region-token'
BUILD = b'regiontest'
WELCOME, REDIRECT, WORK = 2, 11, 10
PASSWORD = 'a long and fine password'


def free(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def hello(link, account, secret):
    """The link's verdict for this id and secret: 'welcome', 'redirect', 'refused <text>', 'work' or
    'none'."""
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(('127.0.0.1', 0))
    udp.settimeout(0.5)
    cookie, key, sealed = b'', None, None
    try:
        for _ in range(30):
            if key is None:
                datagram = noise_link.first_hello()
            else:
                if sealed is None:
                    sealed = noise_link.Hello(noise_link.plain_hello(account, secret, BUILD, cookie, None),
                                              key, os.urandom(32))
                datagram = sealed.datagram
            udp.sendto(datagram, ('127.0.0.1', link))
            try:
                answer = udp.recv(4096)
            except socket.timeout:
                continue
            got = noise_link.cookie_of(answer)
            if got:
                cookie, _bits, key = got
                sealed = None
                continue
            opened = sealed.open(answer) if sealed else None
            if answer[8] == REDIRECT:
                return 'redirect'
            if not opened:
                continue
            kind = opened[0][8]
            if kind == WELCOME:
                return 'welcome' if opened[0][9] == 1 else 'refused ' + opened[0][10:].decode('utf-8', 'replace')
            if kind == REDIRECT:
                return 'redirect'
            if kind == WORK:
                return 'work'
        return 'none'
    finally:
        udp.close()


def told_to_sign_in(answer):
    """A refusal sending the player to the launcher's sign-in (link_server.cpp AccountFirst)."""
    return answer.startswith('refused') and 'account: sign in in the launcher first' in answer


class Shard:
    def __init__(self, home, name, directory, home_of_accounts, link_key=None):
        os.makedirs(os.path.join(home, 'db'), exist_ok=True)
        if link_key:
            shutil.copy(link_key, os.path.join(home, 'link_key.bin'))
        self.home, self.link, self.webapi = home, free(socket.SOCK_DGRAM), free()
        config = {'Host': '127.0.0.1', 'UnsecuredPort': free(), 'MatchingUdpPort': self.link,
                  'WebApiPort': self.webapi, 'StatsEnabled': 'false', 'BloodborneSeamlessCoop': 'true',
                  'BloodborneBackend': 'false', 'Hosted': 'true', 'HostedBuild': BUILD.decode(),
                  'DirectoryUrl': f'http://127.0.0.1:{directory}', 'ShardName': name,
                  'ShardPublicAddress': f'127.0.0.1:{self.link}', 'DirectoryToken': TOKEN,
                  'ShardCapacity': 1000, 'ShardHeartbeatMs': 300, 'HostedLinksPerAddress': 50,
                  'HostedHellosPerSecond': 200, 'HostedSignupWorkBits': 6, 'HostedSignupWorkMaxBits': 6,
                  'SignIn': 'password', 'PasswordHome': 'true' if home_of_accounts else 'false',
                  'PasswordHashMemoryKiB': 8192, 'PasswordHashPasses': 1, 'PasswordTriesPerMinute': 1000}
        with open(os.path.join(home, 'shadnet.cfg'), 'w', encoding='utf-8') as f:
            f.write('[General]\n' + ''.join(f'{k}={v}\n' for k, v in config.items()))
        self.log = open(os.path.join(home, 'shadnet.log'), 'wb')
        self.proc = subprocess.Popen([SHADNET], cwd=home, env=dict(os.environ, SHADNET_HOME=home),
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 20
        while time.time() < deadline:
            with contextlib.suppress(OSError):
                socket.create_connection(('127.0.0.1', self.webapi), timeout=0.5).close()
                return
            time.sleep(0.1)
        raise RuntimeError('shadnet did not start')

    def stop(self):
        self.proc.terminate()
        self.proc.wait(10)
        self.log.close()

    def call(self, method, path, body=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.webapi, timeout=30)
        conn.request(method, path, None if body is None else json.dumps(body),
                     {'Content-Type': 'application/json'})
        answer = conn.getresponse()
        text = answer.read()
        conn.close()
        return answer.status, json.loads(text) if text[:1] == b'{' else text


@unittest.skipIf(MISSING, MISSING)
class PasswordRegionTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix='shadnet-password-region-')
        self.directory = d.serve(d.Directory(fakeredis.FakeRedis()), TOKEN, 0, host='127.0.0.1')
        threading.Thread(target=self.directory.serve_forever, daemon=True).start()
        port = self.directory.server_address[1]
        self.shards = []
        self.one = Shard(os.path.join(self.work, 'one'), 'eu-1', port, True)
        self.shards.append(self.one)
        self.two = Shard(os.path.join(self.work, 'two'), 'eu-2', port, False,
                         os.path.join(self.work, 'one', 'link_key.bin'))
        self.shards.append(self.two)

    def tearDown(self):
        for shard in self.shards:
            shard.stop()
        self.directory.shutdown()
        self.directory.server_close()
        shutil.rmtree(self.work, ignore_errors=True)

    def sign_up(self, name):
        status, work = self.one.call('POST', '/bb/auth/challenge', {})
        self.assertEqual(status, 200, work)
        status, account = self.one.call('POST', '/bb/auth/sign-up', {
            'name': name, 'password': PASSWORD, 'challenge': work['challenge'],
            'nonce': solve(work['challenge'], name, work['bits'])})
        self.assertEqual(status, 200, account)
        return account

    def test_one_account_on_every_shard_and_a_sign_out_everywhere(self):
        # The accounts are the home's only.
        self.assertEqual(self.two.call('POST', '/bb/auth/sign-in', {'name': 'kaniel', 'password': PASSWORD})[0], 404)
        account = self.sign_up('kaniel')
        ident, secret = account['id'].encode(), account['secret'].encode()
        self.assertEqual(hello(self.one.link, ident, secret), 'welcome')
        # The other shard tells it by its signature alone.
        self.assertIn(hello(self.two.link, ident, secret), ('welcome', 'redirect'))
        # A new id that signed in nowhere, and a forged secret: refused, with the launcher's words.
        plain = hello(self.two.link, b'p0123456789abcd', b'a' * 32)
        self.assertTrue(told_to_sign_in(plain), plain)
        forged = secret[:-1] + (b'1' if secret[-1:] == b'0' else b'0')
        self.assertTrue(told_to_sign_in(hello(self.two.link, ident, forged)))
        # A sign out on the home: dead there at once, and on the other shard by the directory.
        status, _answer = self.one.call('POST', '/bb/auth/sign-out', {'id': account['id'],
                                                                      'secret': account['secret']})
        self.assertEqual(status, 200)
        self.assertTrue(told_to_sign_in(hello(self.one.link, ident, secret)))
        deadline, answer = time.time() + 10, ''
        while time.time() < deadline:
            answer = hello(self.two.link, ident, secret)
            if told_to_sign_in(answer):
                break
            time.sleep(0.5)
        self.assertTrue(told_to_sign_in(answer), answer)
        # A new sign-in lives on both.
        time.sleep(1.1)
        status, again = self.one.call('POST', '/bb/auth/sign-in', {'name': 'kaniel', 'password': PASSWORD})
        self.assertEqual(status, 200, again)
        self.assertIn(hello(self.two.link, ident, again['secret'].encode()), ('welcome', 'redirect'))


if __name__ == '__main__':
    unittest.main()
