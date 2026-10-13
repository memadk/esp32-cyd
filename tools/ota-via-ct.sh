#!/bin/sh
# OTA-update a board from the log-server container instead of the Mac: the container is on the wired side of the
# mesh (one Wi-Fi hop to the board), whereas the Mac's own Wi-Fi path to the boards is often poor (2026-09-19/20:
# HTTP 6 s, ping loss, "OTA fejl 2" = board could not connect back to the Mac). espota makes the BOARD connect to the
# uploader, so the uploader must be reachable from the board.
#   tools/ota-via-ct.sh cyd_ota 192.168.68.64        tools/ota-via-ct.sh info2_ota 192.168.68.92
# Builds, archives the ELF, copies firmware.bin + espota.py into CT 111 via pve, runs espota there.
set -eu
ENV="${1:?env}"; IP="${2:?board ip}"
PVE="${PVE:-root@192.168.68.78}"; VMID="${VMID:-111}"
cd "$(dirname "$0")/.."
~/.local/bin/pio run -e "$ENV" | tail -n 3
ELF=".pio/build/$ENV/firmware.elf"; BIN=".pio/build/$ENV/firmware.bin"
SHA=$(shasum -a 256 "$ELF" | cut -c1-8)
mkdir -p builds && cp "$ELF" "builds/$(date +%Y%m%d-%H%M%S)-$ENV-$SHA.elf" && ls -t builds/*.elf | tail -n +11 | xargs rm -f 2>/dev/null || true
ESPOTA=$(ls ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py)
scp -q "$BIN" "$ESPOTA" "$PVE:/tmp/"
ssh -o BatchMode=yes "$PVE" "pct push $VMID /tmp/firmware.bin /root/ota.bin >/dev/null && pct push $VMID /tmp/espota.py /root/espota.py >/dev/null && rm -f /tmp/firmware.bin /tmp/espota.py"
echo "uploading $ENV (elf $SHA) to $IP from CT $VMID"
ssh -o BatchMode=yes "$PVE" "pct exec $VMID -- sh -c 'python3 /root/espota.py -i $IP -p 3232 -f /root/ota.bin -r -t 30 2>&1 | tr \"\\r\" \"\\n\" | grep -E \"Result|Error|Success|100%|invitation\" | tail -4; rm -f /root/ota.bin /root/espota.py'"
