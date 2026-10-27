#!/bin/sh
# Docker HEALTHCHECK / Kubernetes probe: the admin socket's `stats` answers only while the server's
# event loop runs (hosted). Without an admin socket (a home host's config): the WebAPI port takes
# a TCP connection.
set -u
cd "${SHADNET_HOME:-/data}"
sock=$(sed -n 's/^HostedAdminSocket=//p' shadnet.cfg 2>/dev/null | tr -d '\r')
if [ -S "${sock:-admin.sock}" ]; then
    exec timeout 8 /opt/shadnet/shadnet --admin stats >/dev/null
fi
port=$(sed -n 's/^WebApiPort=//p' shadnet.cfg 2>/dev/null | tr -d '\r')
exec timeout 5 bash -c "</dev/tcp/127.0.0.1/${port:-31315}"
