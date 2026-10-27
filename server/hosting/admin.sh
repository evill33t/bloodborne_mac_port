#!/bin/sh
# The operator's commands to a running hosted shadNet (src/admin.h, README.md "Operator commands").
# They go to its admin socket (HostedAdminSocket, a file in its data folder only its own user may
# open), never over the network.
#
#   ./admin.sh players | kick <id|address> | ban <id|address> [90s|30m|12h|7d] [note]
#   ./admin.sh unban <id|address> | bans | worlds | restore <folder> | retire | stats
#
# Docker (docker-compose.yml here): runs inside the shadnet container. systemd or a bare binary:
# SHADNET_BIN (default /opt/shadnet/shadnet) with SHADNET_HOME (default /var/lib/shadnet), as the
# server's user or root. The answer is one JSON object (pretty with jq when it is installed); the
# exit code is 0 when it says "ok": true.
set -eu
if [ $# -eq 0 ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
bin=${SHADNET_BIN:-/opt/shadnet/shadnet}
if [ -z "${SHADNET_BIN:-}" ] && [ -f "$here/docker-compose.yml" ] && command -v docker >/dev/null 2>&1 \
    && [ -n "$(docker compose -f "$here/docker-compose.yml" ps -q shadnet 2>/dev/null)" ]; then
    set +e
    answer=$(docker compose -f "$here/docker-compose.yml" exec -T shadnet /opt/shadnet/shadnet --admin "$@")
    status=$?
    set -e
else
    set +e
    answer=$(SHADNET_HOME=${SHADNET_HOME:-/var/lib/shadnet} "$bin" --admin "$@")
    status=$?
    set -e
fi
if command -v jq >/dev/null 2>&1 && [ -n "$answer" ]; then
    printf '%s\n' "$answer" | jq .
else
    printf '%s\n' "$answer"
fi
exit $status
