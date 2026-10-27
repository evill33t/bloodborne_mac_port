#!/usr/bin/env python3
"""Report what the running local backend is holding.

Asks the server over HTTP instead of reading server/content.db. That file is
open and in WAL mode while the server runs; a second process opening it can
reset the write-ahead log and lose everything stored. The server is the only
safe way to ask while it is up.
"""

import argparse
import json
import sys
import urllib.error
import urllib.request

LABELS = [
    ("users", "players"),
    ("blood_messages", "blood messages"),
    ("tombstones", "bloodstains"),
    ("ghosts", "wandering ghosts"),
    ("chalice_dungeons", "chalice dungeons"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", required=True)
    ap.add_argument("--port", type=int, default=20443)
    args = ap.parse_args()

    url = "http://{}:{}/status".format(args.address, args.port)
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            data = json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as exc:
        print("  Could not reach the server at {}".format(url))
        print("  {}".format(getattr(exc, "reason", exc)))
        print()
        print("  Is \"SERVER - Start Local Backend\" running on that PC?")
        print("  Content can only be listed while the server is up.")
        return 1
    except (ValueError, OSError) as exc:
        print("  Unexpected reply from {}: {}".format(url, exc))
        return 1

    content = data.get("content")
    if content == "off":
        print("  Server is running at {}:{}".format(args.address, args.port))
        print("  Content is OFF - nothing is being stored.")
        print()
        print("  Restart it and answer Y to \"Keep content?\" to store messages,")
        print("  bloodstains, ghosts and chalice dungeons.")
        return 0
    if not isinstance(content, dict):
        print("  Server is running, but gave no content report.")
        return 1

    print("  Server is running at {}:{}".format(args.address, args.port))
    print()
    width = max(len(label) for _, label in LABELS)
    for key, label in LABELS:
        print("    {:<{w}}  {}".format(label, content.get(key, 0), w=width))
    if not any(content.get(key) for key, _ in LABELS):
        print()
        print("  Nothing stored yet. Leave a message or die once, then look again.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
