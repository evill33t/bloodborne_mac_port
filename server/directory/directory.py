# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A region's directory (docs/SCALING.md): which shards are up and how full, who is on which,
which shard holds each world, whose new account each id is. Stateless: every replica reads and
writes one Redis, every write one command or script, so any number of replicas may run.

    REDIS_URL=redis://redis:6379/0 DIRECTORY_TOKEN=... python3 directory.py   # port 8080

Shards authenticate with `Authorization: Bearer $DIRECTORY_TOKEN`; GET /v1/shards, /v1/where and
/v1/world (what a launcher asks) are open.
"""
import hmac
import ipaddress
import json
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PRESENCE_S = 10           # a shard, its players and codes, gone this long after its last heartbeat
LEASE_S = 30              # a world's lease, renewed by every heartbeat of its shard
HOME_S = 90 * 24 * 3600   # where a player last started a world
INBOX_S = 60
INBOX_MAX = 10000
BODY_MAX = 8 << 20
NAME = re.compile(r'[A-Za-z0-9_.-]{1,64}')
KEY = re.compile(r'[0-9a-f]{64}')

# A world's lease: taken when free, renewed only by its holder (a lease that lapsed and was taken by
# another shard is never extended by the old one). Answers the holder.
LEASE_SCRIPT = """
local held = redis.call('GET', KEYS[1])
if not held then
  redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
  return ARGV[1]
end
if held == ARGV[1] then
  redis.call('EXPIRE', KEYS[1], ARGV[2])
end
return held
"""


class Refused(Exception):
    def __init__(self, status, text):
        super().__init__(text)
        self.status = status


def text(value, limit=256):
    if not isinstance(value, str) or len(value) > limit:
        raise Refused(400, 'a field is not a short string')
    return value


def address_of(value):
    """'a.b.c.d:port', or Refused."""
    host, _, port = text(value).rpartition(':')
    try:
        ipaddress.IPv4Address(host)
        if not 0 < int(port) < 65536:
            raise ValueError
    except ValueError:
        raise Refused(400, 'address is not a.b.c.d:port') from None
    return value


def as_str(value):
    return value.decode() if isinstance(value, bytes) else value


class Directory:
    def __init__(self, redis, now=time.time):
        self.redis = redis
        self.now = now
        self.lease = redis.register_script(LEASE_SCRIPT)

    # --- the shards' calls ---

    def heartbeat(self, beat):
        shard = text(beat.get('shard'))
        if not NAME.fullmatch(shard):
            raise Refused(400, 'shard name')
        if not KEY.fullmatch(text(beat.get('key', ''))):
            raise Refused(400, 'key is not 64 hex digits')
        row = {'name': shard, 'address': address_of(beat.get('address')), 'key': beat['key'],
               'capacity': int(beat.get('capacity', 0)), 'draining': bool(beat.get('draining')),
               'players': int(beat.get('count', 0)), 'load': float(beat.get('load', 0.0)),
               'at': self.now()}
        players = [{'id': text(p.get('id'), 64), 'member': text(p.get('member'), 128),
                    'name': text(p.get('name'), 128)} for p in beat.get('players') or []]
        codes = [text(code, 16) for code in beat.get('codes') or []]
        worlds = [text(world, 128) for world in beat.get('worlds') or []]
        started = [text(player, 64) for player in beat.get('started') or []]
        row['players'] = max(row['players'], len(players))

        pipe = self.redis.pipeline(transaction=False)
        pipe.set(f'shard:{shard}', json.dumps(row), ex=PRESENCE_S)
        pipe.sadd('shards', shard)
        pipe.set(f'players:{shard}', json.dumps(players), ex=PRESENCE_S)
        pipe.set(f'codes:{shard}', json.dumps(codes), ex=PRESENCE_S)
        for player in players:
            pipe.set(f'online:{player["id"]}', shard, ex=PRESENCE_S)
        for code in codes:
            pipe.set(f'code:{code}', shard, ex=PRESENCE_S)
        for player in started:
            pipe.set(f'home:{player}', shard, ex=HOME_S)
        for world in worlds:
            self.lease(keys=[f'lease:{world}'], args=[shard, LEASE_S], client=pipe)
        inbox = self.redis.pipeline(transaction=True)
        inbox.lrange(f'inbox:{shard}', 0, 999)
        inbox.ltrim(f'inbox:{shard}', 1000, -1)
        answers = pipe.execute()
        holders = answers[len(answers) - len(worlds):] if worlds else []
        held = {world: as_str(holder) for world, holder in zip(worlds, holders) if as_str(holder) != shard}
        invites = [json.loads(item) for item in inbox.execute()[0]]

        shards = self.shards()
        others = [row['name'] for row in shards if row['name'] != shard]
        region_players, region_codes = [], {}
        if others:
            blobs = self.redis.mget([f'players:{name}' for name in others] +
                                    [f'codes:{name}' for name in others])
            for name, blob in zip(others, blobs[:len(others)]):
                for player in json.loads(blob or '[]'):
                    region_players.append({'shard': name, 'member': player['member'], 'name': player['name']})
            for name, blob in zip(others, blobs[len(others):]):
                for code in json.loads(blob or '[]'):
                    region_codes[code] = name
        return {'shards': shards, 'players': region_players, 'codes': region_codes, 'held': held,
                'inbox': invites}

    def claim_account(self, body):
        """A new id's account: the first shard to claim it holds it for good."""
        player, shard = text(body.get('id'), 64), text(body.get('shard'))
        if not player or not NAME.fullmatch(shard):
            raise Refused(400, 'id and shard')
        self.redis.set(f'account:{player}', shard, nx=True)
        return {'id': player, 'shard': as_str(self.redis.get(f'account:{player}'))}

    def invite(self, body):
        shard = text(body.get('shard'))
        if not NAME.fullmatch(shard):
            raise Refused(400, 'shard name')
        item = {key: text(body.get(key), 128) for key in ('member', 'team', 'from', 'from_name')}
        if not item['team'].startswith('@'):
            raise Refused(400, 'team')
        pipe = self.redis.pipeline(transaction=True)
        pipe.rpush(f'inbox:{shard}', json.dumps(item))
        pipe.ltrim(f'inbox:{shard}', -INBOX_MAX, -1)
        pipe.expire(f'inbox:{shard}', INBOX_S)
        pipe.execute()
        return {'ok': True}

    # --- anyone's ---

    def shards(self):
        """The healthy shards, the least loaded first (by their share of their own capacity)."""
        names = sorted(as_str(name) for name in self.redis.smembers('shards'))
        if not names:
            return []
        rows, gone = [], []
        for name, blob in zip(names, self.redis.mget([f'shard:{name}' for name in names])):
            if blob is None:
                gone.append(name)
            else:
                rows.append(json.loads(blob))
        if gone:
            self.redis.srem('shards', *gone)
        for row in rows:
            row.pop('at', None)
        rows.sort(key=lambda row: (row['draining'], row['players'] / max(1, row['capacity']), row['name']))
        return rows

    def shard(self, name):
        blob = self.redis.get(f'shard:{name}') if name else None
        if blob is None:
            return None
        row = json.loads(blob)
        row.pop('at', None)
        return row

    def where(self, player):
        """The shard `player` is on now, else the one it last started a world on (when up)."""
        player = text(player, 64)
        online = as_str(self.redis.get(f'online:{player}'))
        shard = self.shard(online) if online else None
        if shard:
            return {'player': player, 'online': True, 'shard': shard}
        home = as_str(self.redis.get(f'home:{player}'))
        shard = self.shard(home) if home else None
        if shard:
            return {'player': player, 'online': False, 'shard': shard}
        return None

    def world(self, world):
        holder = as_str(self.redis.get(f'lease:{text(world, 128)}'))
        if not holder:
            return None
        return {'world': world, 'shard': holder, 'up': self.shard(holder) is not None}


def make_handler(directory, token):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'shadnet-directory/1'

        def log_message(self, fmt, *args):  # one line a request is too many at 1000s a second
            pass

        def answer(self, status, body):
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def run(self, call):
            try:
                found = call()
                if found is None:
                    self.answer(404, {'error': 'not found'})
                else:
                    self.answer(200, found)
            except Refused as refused:
                self.answer(refused.status, {'error': str(refused)})
            except (ValueError, TypeError, AttributeError, KeyError):
                self.answer(400, {'error': 'bad request'})
            except Exception as error:  # Redis down: the shards keep their last view
                print(f'directory: {error!r}', file=sys.stderr)
                self.answer(503, {'error': 'unavailable'})

        def do_GET(self):
            url = urlparse(self.path)
            query = {key: values[0] for key, values in parse_qs(url.query).items()}
            routes = {'/v1/shards': lambda: {'shards': directory.shards()},
                      '/v1/where': lambda: directory.where(query.get('player', '')),
                      '/v1/world': lambda: directory.world(query.get('id', '')),
                      '/healthz': lambda: {'ok': bool(directory.redis.ping())}}
            if url.path not in routes:
                return self.answer(404, {'error': 'no such call'})
            self.run(routes[url.path])

        def do_POST(self):
            routes = {'/v1/heartbeat': directory.heartbeat, '/v1/account': directory.claim_account,
                      '/v1/invite': directory.invite}
            call = routes.get(urlparse(self.path).path)
            if call is None:
                return self.answer(404, {'error': 'no such call'})
            said = self.headers.get('Authorization', '')
            if not token or not hmac.compare_digest(said.encode(), f'Bearer {token}'.encode()):
                return self.answer(401, {'error': 'token'})
            size = int(self.headers.get('Content-Length') or 0)
            if size > BODY_MAX:
                return self.answer(413, {'error': 'too large'})
            try:
                body = json.loads(self.rfile.read(size) or b'{}')
            except ValueError:
                return self.answer(400, {'error': 'not JSON'})
            if not isinstance(body, dict):
                return self.answer(400, {'error': 'not an object'})
            self.run(lambda: call(body))

    return Handler


def serve(directory, token, port=8080, host='0.0.0.0'):
    server = ThreadingHTTPServer((host, port), make_handler(directory, token))
    server.daemon_threads = True
    return server


def main():
    import redis
    token = os.environ.get('DIRECTORY_TOKEN', '')
    if not token:
        sys.exit('DIRECTORY_TOKEN is needed')
    client = redis.Redis.from_url(os.environ.get('REDIS_URL', 'redis://127.0.0.1:6379/0'))
    port = int(os.environ.get('PORT', '8080'))
    server = serve(Directory(client), token, port)
    print(f'directory: on port {port}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
