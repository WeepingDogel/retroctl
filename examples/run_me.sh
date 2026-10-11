#!/usr/bin/env bash
# examples/run_me.sh - quick tour of the CLI against a running agent.
# Override host/port/token via RETROCTL_* env or the flags below.
set -euo pipefail
RCTL="python3 $(cd "$(dirname "$0")/.." && pwd)/cli/retroctl.py"
H=${RETROCTL_HOST:-127.0.0.1}
P=${RETROCTL_PORT:-46911}
T=${RETROCTL_TOKEN:-changeme}

echo "=== info ==="
$RCTL -H "$H" -P "$P" -T "$T" info

echo; echo "=== exec: ver & systeminfo ==="
$RCTL -H "$H" -P "$P" -T "$T" exec "ver & systeminfo"

echo; echo "=== ps (top 12 by pid) ==="
$RCTL -H "$H" -P "$P" -T "$T" ps | head -12

echo; echo "=== ls C:\\Inetpub ==="
$RCTL -H "$H" -P "$P" -T "$T" ls "C:\\Inetpub"

echo; echo "=== put a file up ==="
echo "hello from the host, $(date)" > /tmp/retroctl_ping.txt
$RCTL -H "$H" -P "$P" -T "$T" put /tmp/retroctl_ping.txt "C:\\Inetpub\\gozweb\\retroctl_ping.txt"

echo; echo "=== get it back ==="
$RCTL -H "$H" -P "$P" -T "$T" get "C:\\Inetpub\\gozweb\\retroctl_ping.txt"
