#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mac launch preparation for the pinned bloodborne-server client. Never alters game assets.

An identity and save namespace belong to a server. Secrets stay in an owner-only file and
are passed directly to execve, never printed or evaluated as shell commands.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import sys
import tempfile
from urllib.parse import urlsplit

HOST = 'bloodborne.phobos.services'
PIN = '1562e07491a27d6c7aea2ed37364b8839a4a924886afae1ff6eae5c483ea3424'
ROOT = Path(__file__).resolve().parent.parent


def server(value):
    value = value or f'{HOST}:31314'
    if ':' not in value:
        value += ':31314'
    parsed = urlsplit('//' + value)
    if (not parsed.hostname or parsed.username or parsed.password or parsed.path or
            parsed.query or parsed.fragment or not parsed.port or not 0 < parsed.port < 65536 or
            not re.fullmatch(r'[A-Za-z0-9.-]+', parsed.hostname)):
        raise ValueError('Seamless server must be a host name or IPv4 address with an optional UDP port.')
    return parsed.hostname.lower(), parsed.port


def identity(folder, endpoint):
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / (hashlib.sha256(endpoint.encode()).hexdigest()[:20] + '.json')
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    except FileExistsError:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd) as file:
            info = os.fstat(file.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
                raise ValueError(f'Co-op identity must be an owner-only regular file: {path}')
            value = json.load(file)
    else:
        value = {'server': endpoint, 'account': 'mac_' + secrets.token_hex(6), 'password': secrets.token_hex(16)}
        with os.fdopen(fd, 'w') as file:
            json.dump(value, file);file.write('\n');file.flush();os.fsync(file.fileno())
    if (value.get('server') != endpoint or not re.fullmatch(r'[A-Za-z0-9_-]{3,16}', value.get('account', '')) or
            not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', value.get('password', ''))):
        raise ValueError(f'Invalid co-op identity in {path}; restore its backup rather than replacing an existing account.')
    return value


def ini_values(path):
    values = {}
    if path.is_file():
        for line in path.read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and not key.startswith('#'):
                values[key] = value
    return values


def launch_environment(environ, data):
    env = dict(environ)
    host, port = server(env.get('BB_SHADNET_SERVER'))
    endpoint = f'{host}:{port}'
    namespace = hashlib.sha256(endpoint.encode()).hexdigest()[:20]
    seed = Path(env.get('BB_SAVE_DIR') or data / 'user' / 'savedata').expanduser().resolve()
    settings = ini_values(Path(env.get('BB_CONFIG', data / 'bbport.ini')))
    if bool(env.get('BB_SHADNET_NPID')) != bool(env.get('BB_SHADNET_PASSWORD')):
        raise ValueError('Set both BB_SHADNET_NPID and BB_SHADNET_PASSWORD, or leave both unset for a persistent automatic identity.')
    if not env.get('BB_SHADNET_NPID'):
        own = identity(data / 'seamless' / 'identities', endpoint)
        env.update(BB_SHADNET_NPID=own['account'], BB_SHADNET_PASSWORD=own['password'])
    if 'BB_USER_DIR' not in env:
        user = data / 'user-seamless' / namespace
        if not user.exists():
            user.parent.mkdir(parents=True, exist_ok=True)
            staging = Path(tempfile.mkdtemp(prefix='.seed-', dir=user.parent))
            try:
                if seed.is_dir():
                    shutil.copytree(seed, staging / 'savedata')
                staging.rename(user)
            finally:
                if staging.exists(): shutil.rmtree(staging)
        env['BB_USER_DIR'] = str(user.resolve())
    user = Path(env['BB_USER_DIR']).resolve()
    user.mkdir(parents=True, exist_ok=True)
    # The launcher's BB_SAVE_DIR overrides the runtime's --user/savedata. Treat it as
    # a seed too, then route actual writes to this server's selected co-op folder.
    savedata = user / 'savedata'
    if not savedata.exists() and seed.is_dir() and seed != savedata:
        shutil.copytree(seed, savedata)
    env['BB_SAVE_DIR'] = str(savedata)
    services = env.get('BB_SEAMLESS_SERVICES', f'http://{host}:20443').rstrip('/')
    webapi = env.get('BB_SHADNET_WEBAPI', f'http://{host}:31315').rstrip('/')
    for address in (services, webapi):
        parsed = urlsplit(address)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError('Invalid co-op HTTP service address.')
    overrides = {'https://ss4.scej-network.jp:20443': services,
                 'http://ss4.scej-network.jp:20443': services}
    for scheme in ('http', 'https'):
        overrides[f'{scheme}://{host}:18671/summon_messenger'] = webapi
        overrides[f'{scheme}://{host}:20443/summon_messenger'] = webapi
    custom = user / 'host_overrides.custom.json'
    override_path = custom if custom.is_file() else user / 'seamless-host-overrides.json'
    if override_path != custom:
        override_path.write_text(json.dumps(overrides, indent=2) + '\n')
    env.update(BB_ONLINE='1', BB_NET_BACKEND='seamless', BB_SHADNET_SERVER=endpoint,
               BB_SHADNET_WEBAPI=webapi, BB_FPS='60', BB_FPS_LIMIT='60', BB_VBLANK_HZ='60',
               BB_FPS_PRESET='60', BB_PARTY_HUD='0', BB_UPNP='0', SHADPS4_BLOODBORNE_SEAMLESS_COOP='1',
               SHADPS4_BLOODBORNE_RE_TRACE='0', SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(override_path),
               BB_SEAMLESS_PREPARED='1', BB_LOG_FILE=str(user / 'log' / 'seamless.log'))
    env.setdefault('BB_GPU_USER_DIR', str(user))
    if host == HOST and port == 31314:
        env.setdefault('BB_SHADNET_SERVER_KEY', PIN)
    # The server relay uses an ephemeral local socket. Legacy p2p_port is kept for the other pools.
    for name in ('SHADPS4_BLOODBORNE_PARTY', 'SHADPS4_BLOODBORNE_PARTY_ROLE'):
        env.pop(name, None)
    rules = settings.get('seamless_host_rules', 'off')
    if not re.fullmatch(r'off|(?:rot|travel|vote|scale)(?:,(?:rot|travel|vote|scale))*', rules):
        raise ValueError('Invalid seamless_host_rules setting.')
    env.setdefault('SHADPS4_BLOODBORNE_HOST_RULES', rules)
    harder = settings.get('seamless_harder_enemies', 'off')
    if harder != 'off':
        try:
            rates = [float(x) for x in harder.split(',')]
        except ValueError as error:
            raise ValueError('Invalid seamless_harder_enemies setting.') from error
        if len(rates) != 3 or not all(1 <= x <= 10 for x in rates):
            raise ValueError('Enemy HP, damage and poise multipliers must each be from 1 to 10.')
        env.setdefault('SHADPS4_BLOODBORNE_HARDER_ENEMIES', harder)
    return env


def game_view(game, out):
    """Patch checked copies of events, then link a private game view over the current mods."""
    sys.path.insert(0, str(ROOT / 'scripts'))
    import mods
    patcher = ROOT / 'scripts' / 'seamless' / 'party_boss_events.py'
    spec = importlib.util.spec_from_file_location('party_boss_events', patcher)
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source = game / 'dvdroot_ps4' / 'event'
    files = sorted(source.glob('*.emevd.dcx'))
    if not files:
        raise ValueError(f'No event files in {source}')
    digest = hashlib.sha256(patcher.read_bytes())
    hashes = {}
    for file in files:
        raw = file.read_bytes(); hashes[file.name] = hashlib.sha256(raw).hexdigest()
        digest.update(file.name.encode());digest.update(bytes.fromhex(hashes[file.name]))
    cache = out / 'seamless-events' / digest.hexdigest()
    manifest = cache / 'manifest.json'
    cached = json.loads(manifest.read_text()) if manifest.is_file() else None
    if cached:
        for name, row in cached['files'].items():
            if hashlib.sha256((cache / name).read_bytes()).hexdigest() != row['output_sha256']:
                raise ValueError(f'Generated co-op event changed: {cache / name}; remove that generated cache to rebuild.')
    else:
        outputs = {}; entries = {}
        for file in files:
            changed, edits, _counts = module.patch_file(file.name, file.read_bytes())
            if changed is not None:
                outputs[file.name] = changed
                entries[file.name] = {'input_sha256': hashes[file.name], 'output_sha256': hashlib.sha256(changed).hexdigest(), 'edits': edits}
        cache.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.events-', dir=cache.parent))
        try:
            for name, raw in outputs.items(): (staging / name).write_bytes(raw)
            (staging / 'manifest.json').write_text(json.dumps({'tool': str(patcher.name), 'files': entries}, indent=1) + '\n')
            staging.rename(cache)
        finally:
            if staging.exists(): shutil.rmtree(staging)
        cached = {'files': entries}
    view = Path(tempfile.mkdtemp(prefix='seamless-game-', dir=out))
    try:
        for entry in game.iterdir(): mods.make_link(view / entry.name, entry, target_is_directory=entry.is_dir())
        mods.expand(view / 'dvdroot_ps4'); mods.expand(view / 'dvdroot_ps4' / 'event')
        for name in cached['files']:
            destination = view / 'dvdroot_ps4' / 'event' / name
            if destination.is_symlink(): destination.unlink()
            elif destination.exists(): raise ValueError(f'Event overlay conflict: {destination}')
            destination.symlink_to(cache / name)
        print(f'Co-op: {len(cached["files"])} checked event files in a private game view', file=sys.stderr)
        return view
    except BaseException:
        shutil.rmtree(view);raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('launch', 'view'))
    parser.add_argument('--data', type=Path)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--out', type=Path)
    args, extra = parser.parse_known_args()
    if args.action == 'view':
        if not args.game or not args.out or extra: parser.error('view needs --game and --out')
        print(game_view(args.game.resolve(), args.out.resolve()))
    else:
        if not args.data: parser.error('launch needs --data')
        env = launch_environment(os.environ, args.data.resolve())
        print(f'Co-op: {env["BB_SHADNET_SERVER"]}; fixed 60 FPS; F6 Team, F8 Map', file=sys.stderr)
        if extra[:1] == ['--']: extra = extra[1:]
        os.execve('/bin/bash', ['bash', str(ROOT / 'run.sh'), *extra], env)

if __name__ == '__main__':
    try: main()
    except (OSError, ValueError, KeyError) as error:
        print(f'Co-op: {error}', file=sys.stderr);sys.exit(1)
