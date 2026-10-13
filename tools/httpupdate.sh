#!/bin/sh
# Build and upload firmware through the board's web server (robust on poor Wi-Fi links).
#   tools/httpupdate.sh cyd_ota sonos-koekken.local      tools/httpupdate.sh info2_ota infotavle.local
# If an OTA password is set on the board, pass it as the third argument (HTTP basic auth, user "admin").
set -e
ENV="${1:?env}"; HOST="${2:?host}"; PASS="$3"
cd "$(dirname "$0")/.."
~/.local/bin/pio run -e "$ENV"
BIN=".pio/build/$ENV/firmware.bin"; ELF=".pio/build/$ENV/firmware.elf"
SHA=$(shasum -a 256 "$ELF" | cut -c1-8)
mkdir -p builds && cp "$ELF" "builds/$(date +%Y%m%d-%H%M%S)-$ENV-$SHA.elf"
AUTH=""; [ -n "$PASS" ] && AUTH="-u admin:$PASS"
echo "uploading $(wc -c < "$BIN") bytes to http://$HOST/update (elf $SHA)"
LIMIT=""; [ -n "$RATE" ] && LIMIT="--limit-rate $RATE"      # e.g. RATE=40k for weak Wi-Fi links
curl -sS -m 600 $AUTH $LIMIT -F "firmware=@$BIN" "http://$HOST/update"; echo
