#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A shadNet load test (docs/HOSTED_SERVER.md): a fresh server in a temporary folder, simulated
players (bbload) against it, the server's CPU and memory sampled every second.

    run_load.py --shadnet build/shadnet --bbload build/bbload --players 120 --team-size 3

Splits the players over --processes bbload processes (one thread each), prints one JSON line
with the players' numbers (relay delay, losses, join times) and the server's (CPU, memory)."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.dirname(HERE)


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def rss_mb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024
    return 0.0


def thread_ticks(pid):
    with open(f"/proc/{pid}/task/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def udp_rcvbuf_errors():
    with open("/proc/net/snmp") as f:
        lines = [line.split() for line in f if line.startswith("Udp:")]
    return int(lines[1][lines[0].index("RcvbufErrors")])


def threads(pid):
    return len(os.listdir(f"/proc/{pid}/task"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shadnet", required=True)
    parser.add_argument("--bbload", required=True)
    parser.add_argument("--players", type=int, default=60)
    parser.add_argument("--team-size", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--ramp-ms", type=int, default=20)
    parser.add_argument("--processes", type=int, default=0, help="0: one per 150 players")
    parser.add_argument("--port", type=int, default=31314)
    parser.add_argument("--extra", default="", help="more bbload options")
    parser.add_argument("--cfg", action="append", default=[], help="Key=Value for shadnet.cfg")
    parser.add_argument("--keep", action="store_true", help="keep the server's folder")
    parser.add_argument("--attack", default="", help="attack.py arguments, run beside the players")
    parser.add_argument("--attack-delay", type=float, default=20, help="seconds before it")
    args = parser.parse_args()

    home = tempfile.mkdtemp(prefix="shadnet-load-")
    for name in ("worlds.cfg", "scoreboards.cfg"):
        shutil.copy(os.path.join(SERVER, name), home)
    cfg = {
        "Host": "127.0.0.1",
        "MatchingUdpPort": str(args.port),
        "UnsecuredPort": str(args.port - 1),
        "WebApiPort": str(args.port + 1),
        "StatsEnabled": "false",
        "BloodborneBackend": "false",
        "BloodborneSeamlessCoop": "true",
    }
    for item in args.cfg:
        key, value = item.split("=", 1)
        cfg[key] = value
    with open(os.path.join(home, "shadnet.cfg"), "w") as f:
        f.write("[General]\n")
        for key, value in cfg.items():
            f.write(f"{key}={value}\n")

    env = dict(os.environ, SHADNET_HOME=home)
    log = open(os.path.join(home, "shadnet.log"), "w")
    server = subprocess.Popen([os.path.abspath(args.shadnet)], cwd=home, env=env, stdout=log,
                              stderr=subprocess.STDOUT)
    time.sleep(1.5)
    if server.poll() is not None:
        print("shadnet did not start; log in", home, file=sys.stderr)
        sys.exit(1)

    samples = []
    stop = threading.Event()

    def sample():
        hz = os.sysconf("SC_CLK_TCK")
        last = cpu_ticks(server.pid)
        last_main = thread_ticks(server.pid)
        last_t = time.time()
        while not stop.wait(1.0):
            try:
                now = cpu_ticks(server.pid)
                main_now = thread_ticks(server.pid)
            except FileNotFoundError:
                return
            t = time.time()
            samples.append({"cpu": (now - last) / hz / (t - last_t) * 100.0,
                            "main": (main_now - last_main) / hz / (t - last_t) * 100.0,
                            "rss_mb": rss_mb(server.pid), "threads": threads(server.pid)})
            last, last_main, last_t = now, main_now, t

    drops_before = udp_rcvbuf_errors()
    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()

    teams = max(1, args.players // args.team_size)
    processes = args.processes or max(1, (teams * args.team_size + 149) // 150)
    per = [teams // processes + (1 if i < teams % processes else 0) for i in range(processes)]
    loads = []
    for index, count in enumerate(per):
        if count == 0:
            continue
        command = [os.path.abspath(args.bbload), "--json", "--port", str(args.port), "--teams",
                   str(count), "--team-size", str(args.team_size), "--seconds",
                   str(args.seconds), "--ramp-ms", str(args.ramp_ms * processes), "--tag",
                   f"lt{index}"] + args.extra.split()
        loads.append(subprocess.Popen(command, stdout=subprocess.PIPE,
                                      stderr=open(os.path.join(home, f"bbload{index}.log"), "w"),
                                      text=True))
    attack_result = {}
    if args.attack:
        time.sleep(args.attack_delay)
        attack = subprocess.run([sys.executable, os.path.join(HERE, "attack.py"), "--port",
                                 str(args.port)] + args.attack.split(), capture_output=True,
                                text=True)
        for line in attack.stdout.splitlines():
            if line.startswith("{"):
                attack_result = json.loads(line)
        if not attack_result:
            print("attack.py:", attack.stdout, attack.stderr, file=sys.stderr)
    results = []
    for load in loads:
        out, _ = load.communicate()
        for line in out.splitlines():
            if line.startswith("{"):
                results.append(json.loads(line))
    stop.set()
    udp_drops = udp_rcvbuf_errors() - drops_before
    server.terminate()
    try:
        server.wait(10)
    except subprocess.TimeoutExpired:
        server.kill()

    # Steady state: the samples after every game played (the last two thirds of the run).
    steady = samples[len(samples) // 3:] or samples
    summary = {
        "players": sum(r["games"] for r in results),
        "playing": sum(r["playing"] for r in results),
        "refused": sum(r["refused"] for r in results),
        "packets_sent": sum(r["packets_sent"] for r in results),
        "packets_received": sum(r["packets_received"] for r in results),
        "packets_lost": sum(r["packets_lost"] for r in results),
        "server_out_mbit": sum(r["bytes_received"] for r in results) * 8 / 1e6 /
        max(1, max(r["seconds"] for r in results)) if results else 0,
        "server_in_mbit": sum(r["bytes_sent"] for r in results) * 8 / 1e6 /
        max(1, max(r["seconds"] for r in results)) if results else 0,
        "relay_ms_p50": max((r["relay_ms_p50"] for r in results), default=0),
        "relay_ms_p99": max((r["relay_ms_p99"] for r in results), default=0),
        "relay_ms_max": max((r["relay_ms_max"] for r in results), default=0),
        "ping_ms_p99": max((r["ping_ms_p99"] for r in results), default=0),
        "welcome_ms_p50": max((r["welcome_ms_p50"] for r in results), default=0),
        "welcome_ms_p99": max((r["welcome_ms_p99"] for r in results), default=0),
        # Hosted: the new accounts' work (on each bbload's work threads).
        "accounts_worked": sum(r.get("accounts_worked", 0) for r in results),
        "work_ms_p50": max((r.get("work_ms_p50", 0) for r in results), default=0),
        "work_ms_p99": max((r.get("work_ms_p99", 0) for r in results), default=0),
        "solve_ms_p50": max((r.get("solve_ms_p50", 0) for r in results), default=0),
        "solve_ms_p99": max((r.get("solve_ms_p99", 0) for r in results), default=0),
        "work_bits_max": max((r.get("work_bits_max", 0) for r in results), default=0),
        "playing_ms_p50": max((r["playing_ms_p50"] for r in results), default=0),
        "playing_ms_p99": max((r["playing_ms_p99"] for r in results), default=0),
        "server_cpu_avg": round(sum(s["cpu"] for s in steady) / max(1, len(steady)), 1),
        "server_cpu_max": round(max((s["cpu"] for s in samples), default=0), 1),
        # The link's thread (the relay, the worlds, the lobby): at 100% it is the limit.
        "main_thread_cpu_avg": round(sum(s["main"] for s in steady) / max(1, len(steady)), 1),
        "main_thread_cpu_max": round(max((s["main"] for s in samples), default=0), 1),
        "udp_buffer_drops": udp_drops,  # the system's, every socket (server's and players')
        "load_avg": os.getloadavg()[0],
        "server_rss_mb_max": round(max((s["rss_mb"] for s in samples), default=0), 1),
        "server_threads": max((s["threads"] for s in samples), default=0),
    }
    if attack_result:
        summary["attack"] = attack_result
        with open(os.path.join(home, "shadnet.log")) as f:
            log_text = f.read()
        summary["accounts_made"] = log_text.count('made the account of player "bot')
    if summary["packets_sent"]:
        summary["delivered_pct"] = round(100.0 * summary["packets_received"] /
                                         summary["packets_sent"], 2)
    print(json.dumps(summary))
    if args.keep:
        print("server folder:", home, file=sys.stderr)
    else:
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
