#!/bin/sh
# Create (or refresh) the Debian LXC on Proxmox that receives the boards' UDP syslog lines, so logging
# works while the Mac is off. Idempotent: rerun after editing tools/logserver.py to push the new version.
#
# Needs key-based SSH to the Proxmox host as root (once:  ssh-copy-id root@192.168.68.78).
#   PVE=root@192.168.68.78 VMID=111 tools/proxmox/setup-logserver-lxc.sh
#
# What it does on pve:  pct create 111 (debian-12, 1 core, 256 MB, 4 GB, DHCP on vmbr0, onboot, your SSH key)
#                       -> python3, /opt/esp32-logserver/logserver.py, systemd units (udp/5514 receiver +
#                          http/8080 viewer), logrotate, timezone Europe/Copenhagen.
# Afterwards it stops the launchd agent on this Mac: two receivers would both broadcast the SONOSLOG beacon
# and the boards would flap between them.
set -eu
PVE="${PVE:-root@192.168.68.78}"
VMID="${VMID:-111}"
CTNAME="${CTNAME:-esp32-logserver}"
TEMPLATE="${TEMPLATE:-local:vztmpl/debian-12-standard_12.7-1_amd64.tar.zst}"
STORAGE="${STORAGE:-local-lvm}"
PUBKEY="${PUBKEY:-$HOME/.ssh/id_ed25519.pub}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $PVE"

$SSH true || { echo "no key-based SSH to $PVE. Run once:  ssh-copy-id $PVE"; exit 1; }

if ! $SSH "pct status $VMID" >/dev/null 2>&1; then
    echo "creating CT $VMID ($CTNAME) from $TEMPLATE"
    $SSH "cat > /tmp/esp32-logserver.pub" < "$PUBKEY"
    $SSH "pct create $VMID $TEMPLATE --hostname $CTNAME --unprivileged 1 --cores 1 --memory 256 --swap 0 \
        --rootfs $STORAGE:4 --net0 name=eth0,bridge=vmbr0,ip=dhcp,firewall=0 --onboot 1 \
        --ssh-public-keys /tmp/esp32-logserver.pub --description 'UDP syslog receiver for the ESP32 boards (repo ESP32/tools/proxmox)'"
else
    echo "CT $VMID exists, refreshing"
fi
$SSH "pct status $VMID | grep -q running || pct start $VMID"

pe() { $SSH "pct exec $VMID -- sh -c '$*'"; }
echo "waiting for network in the container"
pe 'for i in $(seq 1 30); do getent hosts deb.debian.org >/dev/null 2>&1 && exit 0; sleep 1; done; echo no DNS; exit 1'
pe 'ln -sf /usr/share/zoneinfo/Europe/Copenhagen /etc/localtime; echo Europe/Copenhagen > /etc/timezone'
pe 'command -v python3 >/dev/null || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3)'
pe 'mkdir -p /opt/esp32-logserver /var/log/esp32-boards'

push() {   # push <local file> <path in CT>
    scp -q "$1" "$PVE:/tmp/esp32-push"
    $SSH "pct push $VMID /tmp/esp32-push $2 && rm -f /tmp/esp32-push"
}
push "$HERE/../logserver.py"            /opt/esp32-logserver/logserver.py
push "$HERE/esp32-logserver.service"    /etc/systemd/system/esp32-logserver.service
push "$HERE/esp32-logviewer.service"    /etc/systemd/system/esp32-logviewer.service
push "$HERE/esp32-boards.logrotate"     /etc/logrotate.d/esp32-boards
pe 'systemctl daemon-reload && systemctl enable --now esp32-logserver esp32-logviewer && systemctl restart esp32-logserver'
pe 'sleep 1; systemctl is-active esp32-logserver esp32-logviewer; ss -lun | grep -E ":5514|:8080" || true'

IP="$($SSH "pct exec $VMID -- hostname -I" | awk '{print $1}')"
echo "$IP" > "$HERE/logserver-ip.txt"
echo "container up: $CTNAME (CT $VMID) at $IP  -> udp/5514 receiver, http://$IP:8080/ log viewer, ssh root@$IP"

# Retire the Mac-side receiver (beacon conflict). Plist is kept as .disabled so it can be restored.
LABEL=dk.mikkel.esp32-logserver
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
if [ -f "$PLIST" ]; then
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
    mv "$PLIST" "$PLIST.disabled"
    echo "Mac launchd agent $LABEL stopped and plist renamed to .disabled"
fi
