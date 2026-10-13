#!/usr/bin/env python3
"""UDP log receiver for the Sonos remotes. Each board sends syslog-style lines ("<134>hostname message").
Lines are appended to logs/<hostname>.log with a local timestamp and echoed to stdout.
Start:  python3 tools/logserver.py [port] [logdir]   (default 5514, <repo>/logs)
As a launchd agent: tools/install-logserver-agent.sh (runs a copy from ~ because launchd may not read external volumes)
"""
import socket, sys, time, os, re, threading
port = int(sys.argv[1]) if len(sys.argv) > 1 else 5514
BEACON_PORT = 5515          # boards listen here; we announce "SONOSLOG <port>" every 5 s so no name lookup is needed

def beacon():
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    b.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        try: b.sendto(f"SONOSLOG {port}".encode(), ("255.255.255.255", BEACON_PORT))
        except OSError: pass
        time.sleep(5)
threading.Thread(target=beacon, daemon=True).start()
logdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")   # launchd agent passes ~/Library/Logs/esp32-boards
os.makedirs(logdir, exist_ok=True)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", port))
print(f"listening on udp/{port}, writing to {os.path.abspath(logdir)}", flush=True)
pat = re.compile(r"^<\d+>(\S+) ?(.*)$", re.S)
while True:
    data, addr = s.recvfrom(4096)
    text = data.decode("utf-8", "replace").rstrip()
    m = pat.match(text)
    host, msg = (m.group(1), m.group(2)) if m else (addr[0], text)
    if msg.endswith("RLOG-HELLO"):                 # handshake: the board buffers its lines until we answer
        s.sendto(b"RLOG-ACK", addr)
        continue
    host = re.sub(r"[^A-Za-z0-9._-]", "_", host)
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {addr[0]} {msg}"
    with open(os.path.join(logdir, host + ".log"), "a", encoding="utf-8") as f:
        f.write(line + "\n")
    print(f"[{host}] {line}", flush=True)
