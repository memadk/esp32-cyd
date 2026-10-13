#!/bin/sh
# Install/refresh the launchd agent that runs the UDP log receiver at login and keeps it alive.
# The script is copied into ~/Library/Application Support because launchd-spawned processes are denied access
# to external volumes (TCC "Removable Volumes"); rerun this after editing tools/logserver.py.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APPDIR="$HOME/Library/Application Support/esp32-logserver"
LOGDIR="$HOME/Library/Logs/esp32-boards"
LABEL="dk.mikkel.esp32-logserver"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
mkdir -p "$APPDIR" "$LOGDIR" "$HOME/Library/LaunchAgents"
cp "$HERE/logserver.py" "$APPDIR/logserver.py"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/bin/python3</string>
        <string>$APPDIR/logserver.py</string>
        <string>5514</string>
        <string>$LOGDIR</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>30</integer>
    <key>StandardOutPath</key><string>$HOME/Library/Logs/esp32-logserver.out</string>
    <key>StandardErrorPath</key><string>$HOME/Library/Logs/esp32-logserver.err</string>
    <key>EnvironmentVariables</key><dict><key>PYTHONUNBUFFERED</key><string>1</string></dict>
</dict>
</plist>
PL
plutil -lint "$PLIST" >/dev/null
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
sleep 2
launchctl print "gui/$(id -u)/$LABEL" | grep -E "^\s*(state|pid|last exit)" | head -3
echo "installed: $PLIST -> $APPDIR/logserver.py, logs in $LOGDIR"
