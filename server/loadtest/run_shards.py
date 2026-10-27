#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A region on one machine (docs/SCALING.md): a redis-server, the directory, --shards shadNet shards
on their own ports (one link key), and simulated players (bbload --shards) whose teams form across
the shards (by code, by invite, both). Each shard's link thread is sampled every second.

    run_shards.py --shadnet build/shadnet --bbload build/bbload --players 1800 --shards 3

--shards 1 runs one lone shard (no directory) with the same players: the comparison. Prints one JSON
line: the players' numbers (relay delay, redirects, teams playing) and each shard's (link thread
CPU, players at the end)."""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from run_load import cpu_ticks, rss_mb, thread_ticks, udp_rcvbuf_errors  # noqa: E402

TOKEN = "load-test-token"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_http(url, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with urllib.request.urlopen(url, timeout=1) as answer:
                return json.loads(answer.read() or b"{}")
        except OSError:
            time.sleep(0.2)
    raise RuntimeError(f"{url} did not answer")


def start_shard(root, index, port, args, directory_url, key):
    home = os.path.join(root, f"shard{index}")
    os.makedirs(home)
    for name in ("worlds.cfg", "scoreboards.cfg"):
        shutil.copy(os.path.join(SERVER, name), home)
    with open(os.path.join(home, "link_key.bin"), "wb") as f:
        f.write(key)
    os.chmod(os.path.join(home, "link_key.bin"), 0o600)
    cfg = {
        "Host": "127.0.0.1",
        "MatchingUdpPort": str(port),
        "UnsecuredPort": str(port - 1),
        "WebApiPort": str(port + 1),
        "StatsEnabled": "false",
        "BloodborneBackend": "false",
        "BloodborneSeamlessCoop": "true",
        "Hosted": "true",
        # Every simulated player comes from a few loopback addresses.
        "HostedSignupWorkBits": "8",
        "HostedSignupWorkMaxBits": "8",
        "HostedNewAccountsPerHour": "100000",
        "HostedSignupsPerMinute": "100000",
        "HostedLinksPerAddress": "100000",
        "HostedHellosPerSecond": "100000",
        "HostedDatagramsPerSecond": "1000000",
        "HostedBytesPerSecond": "1000000000",
    }
    if directory_url:
        cfg.update({
            "DirectoryUrl": directory_url,
            "ShardName": f"lt-{index}",
            "ShardPublicAddress": f"127.0.0.1:{port}",
            "DirectoryToken": TOKEN,
            "ShardCapacity": str(args.capacity),
        })
    for item in args.cfg:
        k, v = item.split("=", 1)
        cfg[k] = v
    with open(os.path.join(home, "shadnet.cfg"), "w") as f:
        f.write("[General]\n" + "".join(f"{k}={v}\n" for k, v in cfg.items()))
    env = dict(os.environ, SHADNET_HOME=home)
    for name in ("DIRECTORY_URL", "SHARD_NAME", "SHARD_PUBLIC_ADDRESS", "DIRECTORY_TOKEN"):
        env.pop(name, None)  # the cfg's, not the caller's
    return subprocess.Popen([os.path.abspath(args.shadnet)], cwd=home, env=env,
                            stdout=open(os.path.join(home, "shadnet.log"), "w"),
                            stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shadnet", required=True)
    parser.add_argument("--bbload", required=True)
    parser.add_argument("--players", type=int, default=1800)
    parser.add_argument("--team-size", type=int, default=3)
    parser.add_argument("--shards", type=int, default=3)
    parser.add_argument("--capacity", type=int, default=800)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--ramp-ms", type=int, default=20)
    parser.add_argument("--processes", type=int, default=0, help="0: one per 300 players")
    parser.add_argument("--cross", default="mixed", help="code, invite, mixed")
    parser.add_argument("--port", type=int, default=32314, help="the first shard's link port")
    parser.add_argument("--cfg", action="append", default=[], help="Key=Value for every shard")
    parser.add_argument("--extra", default="", help="more bbload options")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    root = tempfile.mkdtemp(prefix="shadnet-region-")
    children = []
    try:
        directory_url = ""
        if args.shards > 1:
            redis_port = free_port()
            children.append(subprocess.Popen(
                ["redis-server", "--port", str(redis_port), "--save", "", "--appendonly", "no"],
                stdout=open(os.path.join(root, "redis.log"), "w"), stderr=subprocess.STDOUT))
            directory_port = free_port()
            env = dict(os.environ, DIRECTORY_TOKEN=TOKEN, PORT=str(directory_port),
                       REDIS_URL=f"redis://127.0.0.1:{redis_port}/0")
            time.sleep(0.5)
            children.append(subprocess.Popen(
                [sys.executable, os.path.join(SERVER, "directory", "directory.py")], env=env,
                stdout=open(os.path.join(root, "directory.log"), "w"), stderr=subprocess.STDOUT))
            directory_url = f"http://127.0.0.1:{directory_port}"
            wait_http(directory_url + "/healthz")

        key = os.urandom(32)  # the region's: one for every shard
        ports = [args.port + 10 * i for i in range(args.shards)]
        shards = [start_shard(root, i, port, args, directory_url, key)
                  for i, port in enumerate(ports)]
        children += shards
        time.sleep(1.5)
        for i, shard in enumerate(shards):
            if shard.poll() is not None:
                sys.exit(f"shard {i} did not start; log in {root}/shard{i}")
        if directory_url:
            end = time.time() + 10
            while len(wait_http(directory_url + "/v1/shards").get("shards", [])) < args.shards:
                if time.time() > end:
                    sys.exit(f"the shards did not all reach the directory; logs in {root}")
                time.sleep(0.5)

        samples = {i: [] for i in range(args.shards)}
        stop = threading.Event()

        def sample():
            hz = os.sysconf("SC_CLK_TCK")
            last = {i: (cpu_ticks(s.pid), thread_ticks(s.pid)) for i, s in enumerate(shards)}
            last_t = time.time()
            while not stop.wait(1.0):
                t = time.time()
                for i, s in enumerate(shards):
                    try:
                        now = (cpu_ticks(s.pid), thread_ticks(s.pid))
                    except FileNotFoundError:
                        continue
                    samples[i].append({
                        "cpu": (now[0] - last[i][0]) / hz / (t - last_t) * 100.0,
                        "main": (now[1] - last[i][1]) / hz / (t - last_t) * 100.0,
                        "rss_mb": rss_mb(s.pid)})
                    last[i] = now
                last_t = t

        drops_before = udp_rcvbuf_errors()
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()

        teams = max(1, args.players // args.team_size)
        processes = args.processes or max(1, (teams * args.team_size + 299) // 300)
        per = [teams // processes + (1 if i < teams % processes else 0) for i in range(processes)]
        if args.shards > 1:
            where = ["--shards", ",".join(f"127.0.0.1:{p}" for p in ports), "--cross", args.cross]
        else:
            where = ["--port", str(ports[0])]
        loads = []
        for index, count in enumerate(per):
            if count:
                loads.append(subprocess.Popen(
                    [os.path.abspath(args.bbload), "--json", "--teams", str(count), "--team-size",
                     str(args.team_size), "--seconds", str(args.seconds), "--ramp-ms",
                     str(args.ramp_ms * processes), "--tag", f"r{index}"] + where +
                    args.extra.split(),
                    stdout=subprocess.PIPE, text=True,
                    stderr=open(os.path.join(root, f"bbload{index}.log"), "w")))
        results = []
        for load in loads:
            out, _ = load.communicate()
            results += [json.loads(line) for line in out.splitlines() if line.startswith("{")]
        region = wait_http(directory_url + "/v1/shards") if directory_url else {}
        stop.set()
        udp_drops = udp_rcvbuf_errors() - drops_before

        def worst(name):
            return max((r.get(name, 0) for r in results), default=0)

        per_shard = []
        for i in range(args.shards):
            steady = samples[i][len(samples[i]) // 3:] or samples[i]
            row = next((s for s in region.get("shards", []) if s.get("name") == f"lt-{i}"), {})
            per_shard.append({
                "shard": f"lt-{i}",
                "players_at_end": row.get("players"),
                "link_thread_cpu_avg": round(sum(s["main"] for s in steady) / max(1, len(steady)), 1),
                "link_thread_cpu_max": round(max((s["main"] for s in samples[i]), default=0), 1),
                "process_cpu_avg": round(sum(s["cpu"] for s in steady) / max(1, len(steady)), 1),
                "rss_mb_max": round(max((s["rss_mb"] for s in samples[i]), default=0), 1),
            })
        print(json.dumps({
            "shards": args.shards,
            "players": sum(r["games"] for r in results),
            "playing": sum(r["playing"] for r in results),
            "refused": sum(r["refused"] for r in results),
            "redirects": sum(r.get("redirects", 0) for r in results),
            "redirect_ms_p50": worst("redirect_ms_p50"),
            "redirect_ms_p99": worst("redirect_ms_p99"),
            "relay_ms_p50": worst("relay_ms_p50"),
            "relay_ms_p99": worst("relay_ms_p99"),
            "packets_lost": sum(r["packets_lost"] for r in results),
            "packets_received": sum(r["packets_received"] for r in results),
            "playing_ms_p50": worst("playing_ms_p50"),
            "playing_ms_p99": worst("playing_ms_p99"),
            "udp_buffer_drops": udp_drops,
            "cores": os.cpu_count(),
            "load_avg": os.getloadavg()[0],
            "per_shard": per_shard,
        }))
    finally:
        for child in reversed(children):
            child.terminate()
        for child in children:
            try:
                child.wait(10)
            except subprocess.TimeoutExpired:
                child.kill()
        if args.keep:
            print("logs in", root, file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
