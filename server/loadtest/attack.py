#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Abuse against a hosted shadNet (docs/HOSTED_SERVER.md), to run beside a load test (bbload):

    attack.py signin-flood --rate 2000 --seconds 30 [--addresses 1]
        hellos with a new player id each, from one loopback address (or spread over many, as a
        botnet would): every new id is an account to make and a password to hash
        (link version 4: each address does the cookie round trip first, as a real one can;
        --no-cookie sends only first hellos, all a spoofed source can do; hosted, a new id's
        hello must carry work (coop/link_auth.h): without --work the bots skip it, with it
        each does it, as a game does, on this one thread)
    attack.py packet-flood --rate 20000 --seconds 30 [--bad-tags]
        one signed-in game sending datagrams as fast as it can (--bad-tags: from its address with
        tags that do not open, as someone injecting into its link would)

Prints one JSON line: what it sent and what came back."""
import argparse
import hashlib
import json
import os
import random
import socket
import struct
import time

import noise_link as nl

HEADER = b"\xff\xff\xff\xffSHLK"
HELLO, WELCOME, DATA, PING, PONG, COOKIE, WORK = 1, 2, 3, 4, 5, 9, 10
def new_hello(online_id: bytes, cookie: bytes, server_key: bytes, work=None) -> nl.Hello:
    """A hello after the cookie, sealed to the server's key (link version 4)."""
    plain = nl.plain_hello(online_id, b"pw-" + online_id, cookie=cookie, work=work)
    return nl.Hello(plain, server_key, os.urandom(32))


def solve_work(cookie: bytes, online_id: bytes, bits: int) -> int:
    """A nonce whose SHA-256(cookie, id, nonce) starts with `bits` zero bits."""
    prefix = hashlib.sha256(cookie + online_id)
    limit = 1 << (256 - bits)
    nonce = 0
    while True:
        h = prefix.copy()
        h.update(struct.pack("<Q", nonce))
        if int.from_bytes(h.digest(), "big") < limit:
            return nonce
        nonce += 1


def data(peer_ip: int, peer_port: int, frame: bytes) -> bytes:
    return HEADER + bytes([DATA]) + struct.pack(">IH", peer_ip, peer_port) + frame


def welcome_of(datagram: bytes):
    if len(datagram) >= 11 and datagram.startswith(HEADER) and datagram[8] == WELCOME:
        return datagram[9] != 0, datagram[11:11 + datagram[10]].decode(errors="replace")
    return None


def signin_flood(args):
    sockets = []
    for index in range(args.addresses):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((f"127.200.{index // 250}.{index % 250 + 2}", 0))
        s.setblocking(False)
        sockets.append(s)
    sent = welcomed = refused = cookies = work_asked = worked = 0
    work_seconds = 0.0
    reasons = {}
    last_ids = {}  # socket index -> the id of its last first hello (its cookie answers that one)
    hellos = {}  # socket index -> its recent sealed hellos (a sign-in answers late), newest last
    cookies_of = {}  # socket index -> its cookie and the server's key
    again = []  # [socket, hello, when, tries]: a game says hello again until its sign-in answers
    start = time.time()
    interval = 1.0 / args.rate
    next_at = start
    tag = random.randrange(1 << 30)
    while time.time() - start < args.seconds:
        now = time.time()
        if now >= next_at:
            s = sockets[sent % len(sockets)]
            online_id = f"bot{tag % 1000}x{sent}".encode()[:16]
            last_ids[sent % len(sockets)] = online_id
            s.sendto(nl.first_hello(), (args.server, args.port))
            sent += 1
            next_at += interval
        for index, s in enumerate(sockets):
            try:
                while True:
                    datagram = s.recv(2048)
                    cookie = nl.cookie_of(datagram)
                    if cookie:
                        cookies += 1
                        if not args.no_cookie:
                            cookies_of[index] = (cookie[0], cookie[2])
                            sealed = new_hello(last_ids[index], cookie[0], cookie[2])
                            hellos[index] = (hellos.get(index, []) + [(last_ids[index], sealed)])[-8:]
                            s.sendto(sealed.datagram, (args.server, args.port))
                        continue
                    online_id, opened = None, None
                    for said, sealed in reversed(hellos.get(index, [])):
                        opened = sealed.open(datagram)
                        if opened:
                            online_id = said
                            break
                    body = opened[0] if opened else None
                    if body and body[8] == WORK:
                        work_asked += 1
                        if args.work:
                            # What a game does: the work, then a new hello with it.
                            began = time.time()
                            nonce_work = solve_work(cookies_of[index][0], online_id, body[9])
                            work_seconds += time.time() - began
                            worked += 1
                            sealed = new_hello(online_id, *cookies_of[index], work=nonce_work)
                            hellos[index] = (hellos[index] + [(online_id, sealed)])[-8:]
                            s.sendto(sealed.datagram, (args.server, args.port))
                            again.append([s, sealed, time.time() + 0.25, 8])
                        continue
                    answer = welcome_of(body) if body else None
                    if answer:
                        again = [entry for entry in again if entry[1] is not sealed]
                        if answer[0]:
                            welcomed += 1
                        else:
                            refused += 1
                            reasons[answer[1]] = reasons.get(answer[1], 0) + 1
            except BlockingIOError:
                pass
        now = time.time()
        for entry in again:
            if entry[2] <= now and entry[3] > 0:
                entry[0].sendto(entry[1].datagram, (args.server, args.port))
                entry[2], entry[3] = now + 0.25, entry[3] - 1
        again = [entry for entry in again if entry[3] > 0]
        wait = next_at - time.time()
        if wait > 0:
            time.sleep(min(0.001, wait))
    return {"attack": "signin-flood", "addresses": args.addresses, "hellos_sent": sent,
            "cookies": cookies, "work_asked": work_asked, "worked": worked,
            "work_seconds": round(work_seconds, 1), "welcomed": welcomed, "refused": refused,
            "reasons": reasons,
            "seconds": round(time.time() - start, 1)}


def packet_flood(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.201.0.2", 0))
    s.settimeout(0.5)
    online_id = f"flood{random.randrange(10000)}".encode()
    welcomed = False
    session = None
    sealed = None
    for _ in range(20):  # the sign-in is checked on another thread: hello until welcomed
        s.sendto(sealed.datagram if sealed else nl.first_hello(), (args.server, args.port))
        try:
            datagram = s.recv(2048)
            cookie = nl.cookie_of(datagram)
            if cookie:
                cookie_now = cookie
                sealed = new_hello(online_id, cookie[0], cookie[2])
                continue
            opened = sealed.open(datagram) if sealed else None
            if opened and opened[0][8] == WORK:  # hosted: a new account's work first
                sealed = new_hello(online_id, cookie_now[0], cookie_now[2],
                                   work=solve_work(cookie_now[0], online_id, opened[0][9]))
                continue
            answer = welcome_of(opened[0]) if opened else None
            if answer and answer[0]:
                welcomed, session = True, opened[1]
                break
        except socket.timeout:
            pass
    s.setblocking(False)
    frame = b"\x0e\x4a\x0e\x4a" + b"X" * 600
    victim = (10 << 24) | 0x123456
    sent = pongs = 0
    start = time.time()
    interval = 1.0 / args.rate
    next_at = start
    last_ping = 0.0
    pongs_after_ban_check = 0
    while time.time() - start < args.seconds:
        now = time.time()
        while now >= next_at and sent < args.rate * args.seconds:
            datagram = session.seal(data(victim, 3658, frame)) if session else b""
            if args.bad_tags and datagram:
                datagram = datagram[:-1] + bytes([datagram[-1] ^ 1])
            s.sendto(datagram, (args.server, args.port))
            sent += 1
            next_at += interval
        if now - last_ping >= 0.5 and session:
            last_ping = now
            s.sendto(session.seal(HEADER + bytes([PING]) + struct.pack("<IQ", 1, 0)),
                     (args.server, args.port))
        try:
            while True:
                datagram = s.recv(2048)
                if datagram.startswith(HEADER) and datagram[8] == PONG and session and session.open(datagram):
                    pongs += 1
                    if now - start > args.seconds / 2:
                        pongs_after_ban_check += 1
        except BlockingIOError:
            pass
        time.sleep(0.0005)
    return {"attack": "packet-flood", "welcomed": welcomed, "datagrams_sent": sent,
            "rate": round(sent / (time.time() - start)), "pongs": pongs,
            "pongs_in_second_half": pongs_after_ban_check}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("attack", choices=["signin-flood", "packet-flood"])
    parser.add_argument("--server", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=31314)
    parser.add_argument("--rate", type=float, default=2000)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--addresses", type=int, default=1)
    parser.add_argument("--no-cookie", action="store_true", help="signin-flood: first hellos only")
    parser.add_argument("--bad-tags", action="store_true", help="packet-flood: tags that fail")
    parser.add_argument("--work", action="store_true",
                        help="signin-flood: do the work a new account needs (hosted)")
    args = parser.parse_args()
    result = signin_flood(args) if args.attack == "signin-flood" else packet_flood(args)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
