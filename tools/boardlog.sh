#!/bin/sh
# Tail a board's log from the Proxmox log server:  tools/boardlog.sh sonos-koekken [lines]   (-f style follow)
# Board names are the syslog hostnames: sonos-koekken, infotavle, sonos-remote-2.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
IP="${LOGSERVER_IP:-$(cat "$HERE/proxmox/logserver-ip.txt" 2>/dev/null || true)}"
[ -n "$IP" ] || { echo "no log server IP; run tools/proxmox/setup-logserver-lxc.sh first"; exit 1; }
BOARD="${1:?board name}"; N="${2:-50}"
exec ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "root@$IP" "tail -n $N -F /var/log/esp32-boards/$BOARD.log"
