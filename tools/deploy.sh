#!/bin/sh
# Build + upload one environment and archive the ELF so crash backtraces can be symbolized later.
#   tools/deploy.sh cyd_ota        (kitchen board, OTA)      tools/deploy.sh cyd2_ota   (board 2, OTA)
#   tools/deploy.sh cyd            (kitchen board, USB)      tools/deploy.sh cyd2       (board 2, USB)
#   tools/deploy.sh cyd_ota 192.168.68.64   (override the upload port/IP, e.g. when mDNS does not resolve)
# Symbolize a backtrace:  ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line -pfiaC -e builds/<file>.elf 0x400d1234 ...
set -e
ENV="${1:?environment name required}"
PORT="$2"
cd "$(dirname "$0")/.."
~/.local/bin/pio run -e "$ENV" -t upload ${PORT:+--upload-port "$PORT"}
mkdir -p builds
ELF=".pio/build/$ENV/firmware.elf"
SHA=$(shasum -a 256 "$ELF" | cut -c1-8)
cp "$ELF" "builds/$(date +%Y%m%d-%H%M%S)-$ENV-$SHA.elf"
echo "archived ELF sha $SHA -> builds/"
ls -t builds/*.elf | tail -n +11 | xargs rm -f 2>/dev/null || true    # keep the 10 newest
