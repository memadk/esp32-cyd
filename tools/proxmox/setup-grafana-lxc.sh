#!/bin/sh
# Install/refresh Grafana + Loki + Alloy inside the esp32-logserver LXC (CT 111, created by setup-logserver-lxc.sh).
# Pipeline: boards -> logserver.py -> /var/log/esp32-boards/<board>.log -> Alloy (labels board/level/tag) -> Loki
# -> Grafana on http://<ct-ip>:3000 with provisioned dashboards from grafana/dashboards/*.json.
# Idempotent: rerun after editing anything under grafana/ (run grafana/make_dashboards.py first for dashboards).
# First run also imports the Mac's log history from ~/Library/Logs/esp32-boards (marker file prevents a repeat).
set -eu
IMPORTED=0
PVE="${PVE:-root@192.168.68.78}"
VMID="${VMID:-111}"
HISTORY_DIR="${HISTORY_DIR:-$HOME/Library/Logs/esp32-boards}"
HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/grafana"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $PVE"
pe() { $SSH "pct exec $VMID -- sh -c '$*'"; }
push() { scp -q "$1" "$PVE:/tmp/esp32-push"; $SSH "pct push $VMID /tmp/esp32-push $2 && rm -f /tmp/esp32-push"; }

$SSH "pct status $VMID | grep -q running" || { echo "CT $VMID not running"; exit 1; }

echo "== apt repo + packages"
pe 'command -v gpg >/dev/null || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq apt-transport-https gnupg curl ca-certificates)'
pe 'test -f /etc/apt/keyrings/grafana.gpg || (mkdir -p /etc/apt/keyrings && curl -fsSL https://apt.grafana.com/gpg.key | gpg --dearmor -o /etc/apt/keyrings/grafana.gpg)'
pe 'test -f /etc/apt/sources.list.d/grafana.list || echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" > /etc/apt/sources.list.d/grafana.list'
pe 'dpkg -s grafana loki alloy >/dev/null 2>&1 || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq grafana loki alloy)'

echo "== import Mac log history (once)"
if [ -d "$HISTORY_DIR" ] && ! pe 'test -f /var/log/esp32-boards/.history-imported' 2>/dev/null; then
    pe 'systemctl stop esp32-logserver; systemctl stop alloy 2>/dev/null || true'
    for f in "$HISTORY_DIR"/*.log; do
        b="$(basename "$f")"
        case "$b" in rlog.log) continue;; esac
        push "$f" "/tmp/hist-$b"
        pe "cd /var/log/esp32-boards && if [ -f $b ]; then cat /tmp/hist-$b $b > $b.new && mv $b.new $b; else mv /tmp/hist-$b $b; fi; rm -f /tmp/hist-$b"
        echo "   imported $b"
    done
    pe 'touch /var/log/esp32-boards/.history-imported; rm -rf /var/lib/alloy/data/loki.source.file.boards 2>/dev/null; systemctl start esp32-logserver'
    IMPORTED=1
fi

echo "== configs"
pe 'mkdir -p /var/lib/loki /var/lib/grafana/dashboards /etc/grafana/provisioning/datasources /etc/grafana/provisioning/dashboards'
push "$G/loki.yml"        /etc/loki/config.yml
push "$G/config.alloy"    /etc/alloy/config.alloy
push "$G/datasource.yml"  /etc/grafana/provisioning/datasources/loki.yml
push "$G/dashboards.yml"  /etc/grafana/provisioning/dashboards/esp32.yml
for d in "$G"/dashboards/*.json; do push "$d" "/var/lib/grafana/dashboards/$(basename "$d")"; done
pe 'chown -R loki: /var/lib/loki; chown -R grafana:grafana /var/lib/grafana/dashboards; chmod 755 /var/log/esp32-boards; chmod 644 /var/log/esp32-boards/*.log'
pe 'grep -q "^reporting_enabled" /etc/grafana/grafana.ini || printf "\n[analytics]\nreporting_enabled = false\ncheck_for_updates = false\n" >> /etc/grafana/grafana.ini'

echo "== services"
pe 'systemctl daemon-reload; systemctl enable loki grafana-server alloy >/dev/null 2>&1; systemctl restart loki grafana-server'
echo "   waiting for Loki ingester"
pe 'for i in $(seq 1 60); do curl -sf http://127.0.0.1:3100/ready >/dev/null 2>&1 && exit 0; sleep 2; done; echo loki not ready; exit 1'
pe 'systemctl restart alloy'
pe 'sleep 5; systemctl is-active loki alloy grafana-server esp32-logserver; curl -s http://127.0.0.1:3100/ready; echo; curl -s http://127.0.0.1:3100/loki/api/v1/labels; echo; curl -s http://127.0.0.1:3000/api/health'
if [ "$IMPORTED" = 1 ]; then
    # Loki only queries the ingester for the last 3 h; older imported lines become visible once their chunks
    # are flushed to disk, which otherwise happens after 30 min idle. Give Alloy time to read, then flush.
    echo "== flushing imported history to the store"
    pe 'sleep 90; curl -s -m 120 -XPOST "http://127.0.0.1:3100/flush?wait=true"; echo flushed'
fi
IP="$($SSH "pct exec $VMID -- hostname -I" | awk '{print $1}')"
echo
echo "Grafana: http://$IP:3000  (first login admin/admin, Grafana asks you to set a new password)"
