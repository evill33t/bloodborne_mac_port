#!/bin/sh
# Seeds /data on first start, then runs shadNet in the foreground (Dockerfile; docker-compose.yml
# and the Helm chart in deploy/helm/yharnam both use it).
#
# SHADNET_CONFIG=<file>: copied over /data/shadnet.cfg at every start (the chart's ConfigMap), so
# the config the operator keeps in git is the one that runs. Unset: shadnet.cfg is written once
# with the public-host defaults below and then left to the operator.
# SHADNET_BACKEND_FILES=<dir>: notice.txt, broadcast.txt, login_message.txt copied into
# /data/backend (BackendFolder) at every start.
# SHADNET_LINK_KEYS=<dir>: shard-<ordinal>.bin copied to link_key.bin when there is none.
set -eu
cd "${SHADNET_HOME:-/data}"
for f in worlds.cfg scoreboards.cfg; do
    [ -f "$f" ] || cp "/opt/shadnet/defaults/$f" "$f"
done
if [ -n "${SHADNET_CONFIG:-}" ]; then
    cp "$SHADNET_CONFIG" shadnet.cfg
    chmod 0600 shadnet.cfg
elif [ ! -f shadnet.cfg ]; then
    # Public-host defaults; shadNet writes every other key with its default on first load.
    cat > shadnet.cfg <<EOF
[General]
Host=0.0.0.0
; Games reach the TCP port through the link's tunnel: keep it on 127.0.0.1.
TcpLocalOnly=true
; docs/HOSTED_SERVER.md: rate limits, bans (banned.txt), team-only worlds, own names only.
Hosted=true
HostedBuild=${SHADNET_BUILD:-}
BloodborneSeamlessCoop=true
; Local fork: enabled for new installations; existing configuration is preserved.
Invasions=true
BloodborneWorldPath=bloodborne_worlds
BackendStore=db/backend.db
BackendFolder=backend
StatsEnabled=false
EOF
fi
mkdir -p db bloodborne_worlds backend
if [ -n "${SHADNET_BACKEND_FILES:-}" ] && [ -d "$SHADNET_BACKEND_FILES" ]; then
    for f in notice.txt broadcast.txt login_message.txt; do
        if [ -f "$SHADNET_BACKEND_FILES/$f" ]; then cp "$SHADNET_BACKEND_FILES/$f" backend/; fi
    done
fi
# Kubernetes (deploy/helm/yharnam): a StatefulSet pod is <name>-<ordinal> (POD_NAME; in
# hostNetwork HOSTNAME is the node's). The ordinal picks this shard's address from
# SHARD_PUBLIC_ADDRESSES (comma-separated), its name, and its key from SHADNET_LINK_KEYS.
if [ -z "${SHARD_INDEX:-}" ]; then
    case ${POD_NAME:-} in *-[0-9] | *-[0-9][0-9] | *-[0-9][0-9][0-9]) SHARD_INDEX=${POD_NAME##*-} ;; esac
fi
if [ -n "${SHARD_INDEX:-}" ]; then
    if [ -z "${SHARD_NAME:-}" ] && [ -n "${SHARD_NAME_PREFIX:-}" ]; then
        SHARD_NAME=$SHARD_NAME_PREFIX-$SHARD_INDEX
    fi
    if [ -z "${SHARD_PUBLIC_ADDRESS:-}" ] && [ -n "${SHARD_PUBLIC_ADDRESSES:-}" ]; then
        SHARD_PUBLIC_ADDRESS=$(printf '%s' "$SHARD_PUBLIC_ADDRESSES" | cut -d, -f$((SHARD_INDEX + 1)))
    fi
    key=${SHADNET_LINK_KEYS:-}/shard-$SHARD_INDEX.bin
    if [ -n "${SHADNET_LINK_KEYS:-}" ] && [ ! -f link_key.bin ] && [ -f "$key" ]; then
        (umask 077 && cp "$key" link_key.bin)
        echo "entrypoint: link_key.bin taken from $key"
    fi
fi
SHARD_PUBLIC_ADDRESS=${SHARD_PUBLIC_ADDRESS:-${HOST_IP:-}}
export SHARD_NAME="${SHARD_NAME:-}" SHARD_PUBLIC_ADDRESS
# The server's identity (LinkKeyFile): made by shadNet on its first start. Say so loudly when it
# is missing on a folder that has accounts: every game pinned the old key and will refuse this one.
if [ ! -f link_key.bin ] && [ -f db/shadnet.db ]; then
    echo "entrypoint: WARNING: db/shadnet.db exists but link_key.bin does not; a new server key" \
         "will be made and every player's pinned key will no longer match. Restore it from backup." >&2
fi
# No --watchdog: Docker/Kubernetes/systemd restart the process instead.
exec /opt/shadnet/shadnet "$@"
