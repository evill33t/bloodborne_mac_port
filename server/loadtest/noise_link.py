#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""The link's handshake and encryption (link version 4, server/coop/link_auth.h) in Python, on
the `cryptography` package: attack.py's games speak it, and

    noise_link.py --vectors

prints the sealed datagrams of server/tests/contracts/server_link_frames.txt, made without our C++
code: both test suites check that link_auth.h makes the same bytes (Noise NK as noiseprotocol.org
writes it, not as we read it twice)."""
import hashlib
import hmac
import struct
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

HEADER = b"\xff\xff\xff\xffSHLK"
HELLO, WELCOME, DATA, PING, PONG, COOKIE, WORK = 1, 2, 3, 4, 5, 9, 10
LINK_VERSION = 4
NAME = b"Noise_NK_25519_ChaChaPoly_BLAKE2b"
HELLO_CLEAR = 9 + 1 + 16


def public_of(secret: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(secret).public_key().public_bytes(
        Encoding.Raw, PublicFormat.Raw)


def dh(secret: bytes, other: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(secret).exchange(
        X25519PublicKey.from_public_bytes(other))


def blake2b(data: bytes) -> bytes:
    return hashlib.blake2b(data).digest()


def hkdf(chaining: bytes, data: bytes):
    temp = hmac.new(chaining, data, hashlib.blake2b).digest()
    first = hmac.new(temp, b"\x01", hashlib.blake2b).digest()
    return first, hmac.new(temp, first + b"\x02", hashlib.blake2b).digest()


def nonce(n: int) -> bytes:
    return b"\0" * 4 + struct.pack("<Q", n)


class Symmetric:
    def __init__(self, prologue: bytes):
        self.h = NAME.ljust(64, b"\0")
        self.ck = self.h
        self.k = None
        self.n = 0
        self.mix_hash(prologue)

    def copy(self):
        other = Symmetric(b"")
        other.h, other.ck, other.k, other.n = self.h, self.ck, self.k, self.n
        return other

    def mix_hash(self, data: bytes):
        self.h = blake2b(self.h + data)

    def mix_key(self, data: bytes):
        self.ck, key = hkdf(self.ck, data)
        self.k, self.n = key[:32], 0

    def encrypt_and_hash(self, plain: bytes) -> bytes:
        cipher = ChaCha20Poly1305(self.k).encrypt(nonce(self.n), plain, self.h)
        self.n += 1
        self.mix_hash(cipher)
        return cipher

    def decrypt_and_hash(self, cipher: bytes) -> bytes:
        plain = ChaCha20Poly1305(self.k).decrypt(nonce(self.n), cipher, self.h)
        self.n += 1
        self.mix_hash(cipher)
        return plain

    def split(self):
        up, down = hkdf(self.ck, b"")
        return up[:32], down[:32]


class Session:
    """One side of a link after its welcome: each datagram's body sealed, its counter and tag
    last."""

    def __init__(self, send: bytes, receive: bytes):
        self.send, self.receive, self.counter = ChaCha20Poly1305(send), ChaCha20Poly1305(receive), 0

    def seal(self, datagram: bytes) -> bytes:
        self.counter += 1
        sealed = self.send.encrypt(nonce(self.counter), datagram[9:], datagram[:9])
        return datagram[:9] + sealed[:-16] + struct.pack("<Q", self.counter) + sealed[-16:]

    def open(self, datagram: bytes):
        if len(datagram) < 9 + 24:
            return None
        counter = struct.unpack("<Q", datagram[-24:-16])[0]
        try:
            body = self.receive.decrypt(nonce(counter), datagram[9:-24] + datagram[-16:],
                                        datagram[:9])
        except Exception:
            return None
        return datagram[:9] + body


def text(value: bytes) -> bytes:
    return bytes([len(value)]) + value


def plain_hello(online_id: bytes, secret: bytes, build: bytes = b"bbload", cookie: bytes = b"",
                work=None) -> bytes:
    return (HEADER + bytes([HELLO, LINK_VERSION]) + cookie.ljust(16, b"\0") + text(online_id) +
            text(build) + text(secret) + (b"" if work is None else struct.pack("<Q", work)))


def first_hello() -> bytes:
    """A hello before the cookie: its clear part (a zero cookie), padded; nothing secret."""
    return (HEADER + bytes([HELLO, LINK_VERSION]) + b"\0" * 16).ljust(HELLO_CLEAR + 48, b"\0")


class Hello:
    """The game's handshake for one hello: its datagram, and the answers to it opened."""

    def __init__(self, plain: bytes, server_key: bytes, ephemeral: bytes):
        self.state = Symmetric(plain[:HELLO_CLEAR])
        self.state.mix_hash(server_key)
        self.ephemeral = ephemeral
        mine = public_of(ephemeral)
        self.state.mix_hash(mine)
        self.state.mix_key(dh(ephemeral, server_key))
        self.datagram = plain[:HELLO_CLEAR] + mine + self.state.encrypt_and_hash(plain[HELLO_CLEAR:])

    def open(self, datagram: bytes):
        """An answer (welcome, work) opened: (its plain datagram, the link's session) or None."""
        if len(datagram) < 9 + 48 + 10 or datagram[8] not in (WELCOME, WORK):
            return None
        state = self.state.copy()
        theirs = datagram[9:41]
        state.mix_hash(theirs)
        state.mix_key(dh(self.ephemeral, theirs))
        try:
            plain = state.decrypt_and_hash(datagram[41:])
        except Exception:
            return None
        if plain[:9] != datagram[:9]:
            return None
        up, down = state.split()
        return plain, Session(up, down)


def answer(hello_datagram: bytes, server_secret: bytes, plain: bytes, ephemeral: bytes):
    """The server's side, for the vectors only: the answer carrying `plain`."""
    state = Symmetric(hello_datagram[:HELLO_CLEAR])
    state.mix_hash(public_of(server_secret))
    game = hello_datagram[HELLO_CLEAR:HELLO_CLEAR + 32]
    state.mix_hash(game)
    state.mix_key(dh(server_secret, game))
    state.decrypt_and_hash(hello_datagram[HELLO_CLEAR + 32:])
    mine = public_of(ephemeral)
    state.mix_hash(mine)
    state.mix_key(dh(ephemeral, game))
    return plain[:9] + mine + state.encrypt_and_hash(plain)


def cookie_of(datagram: bytes):
    """A cookie answer: (cookie, work bits, the server's key) or None."""
    if len(datagram) >= 9 + 49 and datagram.startswith(HEADER) and datagram[8] == COOKIE:
        return datagram[9:25], datagram[25], datagram[26:58]
    return None


def vectors():
    build = b"92829ac4 events 1f protocol 9"
    server_secret, game_e, server_e = b"\x44" * 32, b"\x33" * 32, b"\x55" * 32
    plain = plain_hello(b"Kaniel", b"s3cr3t", build, b"\x11" * 16)
    hello = Hello(plain, public_of(server_secret), game_e)
    welcome = HEADER + bytes([WELCOME, 1, 0])
    sealed_welcome = answer(hello.datagram, server_secret, welcome, server_e)
    opened, session = hello.open(sealed_welcome)
    assert opened == welcome
    ping = HEADER + bytes([PING]) + struct.pack("<IQ", 7, 1000)
    return {
        "server.key": public_of(server_secret),
        "hello.sealed": hello.datagram,
        "welcome.sealed": sealed_welcome,
        "ping.sealed": session.seal(ping),
    }


if __name__ == "__main__":
    if sys.argv[1:] == ["--vectors"]:
        for name, value in vectors().items():
            print(f"{name}\t{value.hex()}")
