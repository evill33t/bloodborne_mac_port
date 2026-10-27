# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The region's directory (directory.py) on fakeredis, and once over HTTP:

    python3 -m unittest -v test_directory     (pip install redis 'fakeredis[lua]')
"""
import json
import threading
import unittest
import urllib.error
import urllib.request

try:
    import fakeredis
    import directory as d
    MISSING = None
except ImportError as missing:  # ctest runs them where they are installed only
    MISSING = f'{missing.name} is not installed'

KEY = 'ab' * 32


class Clock:
    def __init__(self):
        self.at = 1000.0

    def __call__(self):
        return self.at


def beat(shard, port, players=(), codes=(), worlds=(), started=(), capacity=800, draining=False):
    return {'shard': shard, 'address': f'10.0.0.1:{port}', 'key': KEY, 'capacity': capacity,
            'draining': draining, 'count': len(players),
            'players': [{'id': p, 'member': p.encode().hex(), 'name': p} for p in players],
            'codes': list(codes), 'worlds': list(worlds), 'started': list(started)}


@unittest.skipIf(MISSING, MISSING)
class DirectoryTests(unittest.TestCase):
    def setUp(self):
        self.server = fakeredis.FakeServer()
        self.redis = fakeredis.FakeRedis(server=self.server)
        self.clock = Clock()
        self.directory = d.Directory(self.redis, now=self.clock)

    def replica(self):
        """Another directory process on the same Redis."""
        return d.Directory(fakeredis.FakeRedis(server=self.server), now=self.clock)

    def test_heartbeats_list_the_shards_least_loaded_first(self):
        self.directory.heartbeat(beat('eu-1', 1, players=['a', 'b', 'c']))
        self.directory.heartbeat(beat('eu-2', 2, players=['d']))
        self.directory.heartbeat(beat('eu-3', 3, players=[], draining=True))
        rows = self.replica().shards()
        self.assertEqual([row['name'] for row in rows], ['eu-2', 'eu-1', 'eu-3'])
        self.assertEqual(rows[1]['players'], 3)
        self.assertEqual(rows[0]['address'], '10.0.0.1:2')
        self.assertEqual(rows[0]['key'], KEY)

    def test_a_shard_that_stops_beating_goes(self):
        self.directory.heartbeat(beat('eu-1', 1, players=['a']))
        self.directory.heartbeat(beat('eu-2', 2))
        self.redis.delete('shard:eu-1')  # its TTL ran out
        self.assertEqual([row['name'] for row in self.directory.shards()], ['eu-2'])
        self.assertEqual(self.redis.smembers('shards'), {b'eu-2'})
        self.assertGreater(self.redis.ttl('shard:eu-2'), 0)
        self.assertLessEqual(self.redis.ttl('online:a'), d.PRESENCE_S)

    def test_the_answer_says_the_other_shards_players_and_codes(self):
        self.directory.heartbeat(beat('eu-1', 1, players=['Ada'], codes=['ABCDEF']))
        answer = self.replica().heartbeat(beat('eu-2', 2, players=['Kaniel'], codes=['GHJKLM']))
        self.assertEqual(answer['players'], [{'shard': 'eu-1', 'member': 'Ada'.encode().hex(), 'name': 'Ada'}])
        self.assertEqual(answer['codes'], {'ABCDEF': 'eu-1'})
        self.assertEqual({row['name'] for row in answer['shards']}, {'eu-1', 'eu-2'})

    def test_where_a_player_is_or_last_played(self):
        self.directory.heartbeat(beat('eu-1', 1, players=['Ada'], started=['Ada']))
        self.directory.heartbeat(beat('eu-2', 2, players=['Kaniel']))
        found = self.directory.where('Kaniel')
        self.assertEqual((found['online'], found['shard']['name']), (True, 'eu-2'))
        self.redis.delete('online:Ada')  # Ada signed off
        found = self.directory.where('Ada')
        self.assertEqual((found['online'], found['shard']['name']), (False, 'eu-1'))
        self.assertIsNone(self.directory.where('nobody'))
        # Its home shard down: nowhere (the launcher picks the least loaded).
        self.redis.delete('shard:eu-1')
        self.assertIsNone(self.directory.where('Ada'))

    def test_a_world_has_one_holder(self):
        answer = self.directory.heartbeat(beat('eu-1', 1, worlds=['w1', 'w2']))
        self.assertEqual(answer['held'], {})
        # Another shard listing w1 (a copy) does not get it; its own w3 it does.
        answer = self.replica().heartbeat(beat('eu-2', 2, worlds=['w1', 'w3']))
        self.assertEqual(answer['held'], {'w1': 'eu-1'})
        self.assertEqual(self.directory.world('w1')['shard'], 'eu-1')
        self.assertEqual(self.directory.world('w3')['shard'], 'eu-2')
        self.assertIsNone(self.directory.world('w9'))
        # Renewed by its holder only.
        self.redis.expire('lease:w1', 5)
        self.replica().heartbeat(beat('eu-2', 2, worlds=['w1']))
        self.assertLessEqual(self.redis.ttl('lease:w1'), 5)
        self.directory.heartbeat(beat('eu-1', 1, worlds=['w1']))
        self.assertGreater(self.redis.ttl('lease:w1'), 5)
        # Lapsed (its shard died): the next shard that lists it takes it.
        self.redis.delete('lease:w1')
        answer = self.replica().heartbeat(beat('eu-2', 2, worlds=['w1']))
        self.assertEqual(answer['held'], {})
        self.assertEqual(self.directory.world('w1')['shard'], 'eu-2')

    def test_a_new_id_is_its_first_shards(self):
        self.assertEqual(self.directory.claim_account({'id': 'Ada', 'shard': 'eu-1'})['shard'], 'eu-1')
        self.assertEqual(self.replica().claim_account({'id': 'Ada', 'shard': 'eu-2'})['shard'], 'eu-1')
        self.assertEqual(self.directory.claim_account({'id': 'Ada', 'shard': 'eu-1'})['shard'], 'eu-1')
        with self.assertRaises(d.Refused):
            self.directory.claim_account({'id': 'Ada', 'shard': 'not a name'})

    def test_an_invite_reaches_its_shard_once(self):
        invite = {'shard': 'eu-2', 'member': '4164', 'team': '@eu-1/ABCDEF', 'from': '4b', 'from_name': 'Kaniel'}
        self.directory.invite(invite)
        answer = self.replica().heartbeat(beat('eu-2', 2))
        self.assertEqual(answer['inbox'], [{key: invite[key] for key in ('member', 'team', 'from', 'from_name')}])
        self.assertEqual(self.directory.heartbeat(beat('eu-2', 2))['inbox'], [])
        self.assertEqual(self.directory.heartbeat(beat('eu-1', 1))['inbox'], [])
        with self.assertRaises(d.Refused):
            self.directory.invite(dict(invite, team='t1'))

    def test_a_bad_heartbeat_is_refused(self):
        for bad in (dict(beat('eu-1', 1), shard='a b'), dict(beat('eu-1', 1), address='nowhere'),
                    dict(beat('eu-1', 1), key='12'), dict(beat('eu-1', 1), address='10.0.0.1:0')):
            with self.assertRaises(d.Refused):
                self.directory.heartbeat(bad)


@unittest.skipIf(MISSING, MISSING)
class HttpTests(unittest.TestCase):
    def setUp(self):
        self.directory = d.Directory(fakeredis.FakeRedis())
        self.server = d.serve(self.directory, 'token', port=0, host='127.0.0.1')
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}'
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def call(self, path, body=None, token='token'):
        request = urllib.request.Request(self.url + path, data=None if body is None else json.dumps(body).encode())
        if token:
            request.add_header('Authorization', f'Bearer {token}')
        try:
            with urllib.request.urlopen(request, timeout=5) as answer:
                return answer.status, json.loads(answer.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_the_shards_calls_need_the_token(self):
        self.assertEqual(self.call('/v1/heartbeat', beat('eu-1', 1), token='')[0], 401)
        self.assertEqual(self.call('/v1/heartbeat', beat('eu-1', 1), token='wrong')[0], 401)
        self.assertEqual(self.call('/v1/account', {'id': 'a', 'shard': 'eu-1'}, token='')[0], 401)
        status, answer = self.call('/v1/heartbeat', beat('eu-1', 1, players=['Ada']))
        self.assertEqual(status, 200)
        self.assertEqual(answer['shards'][0]['name'], 'eu-1')

    def test_a_launcher_reads_the_shards_and_where_its_player_is(self):
        self.call('/v1/heartbeat', beat('eu-1', 1, players=['Ada']))
        status, answer = self.call('/v1/shards', token='')
        self.assertEqual((status, answer['shards'][0]['address']), (200, '10.0.0.1:1'))
        status, answer = self.call('/v1/where?player=Ada', token='')
        self.assertEqual((status, answer['shard']['name']), (200, 'eu-1'))
        self.assertEqual(self.call('/v1/where?player=Bob', token='')[0], 404)
        self.assertEqual(self.call('/v1/nothing', token='')[0], 404)
        self.assertEqual(self.call('/v1/heartbeat', {'shard': 'x'})[0], 400)


if __name__ == '__main__':
    unittest.main()
