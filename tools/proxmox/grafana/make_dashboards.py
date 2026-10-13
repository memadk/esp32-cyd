#!/usr/bin/env python3
"""Generate the two provisioned Grafana dashboards (dashboards/*.json). Rerun after editing, then
tools/proxmox/setup-grafana-lxc.sh pushes them (Grafana re-reads provisioned files every 30 s)."""
import json, os
DS = {"type": "loki", "uid": "loki"}
HERE = os.path.dirname(os.path.abspath(__file__))

def tgt(expr, legend="", ref="A", instant=False):
    t = {"datasource": DS, "expr": expr, "legendFormat": legend, "refId": ref, "queryType": "range"}
    if instant: t.update(queryType="instant", instant=True, range=False)
    return t

def panel(ptype, title, x, y, w, h, targets, unit=None, opts=None, overrides=None, desc=""):
    p = {"type": ptype, "title": title, "description": desc, "gridPos": {"x": x, "y": y, "w": w, "h": h},
         "datasource": DS, "targets": targets,
         "fieldConfig": {"defaults": {}, "overrides": overrides or []}, "options": opts or {}}
    if unit: p["fieldConfig"]["defaults"]["unit"] = unit
    if ptype == "timeseries":
        p["fieldConfig"]["defaults"]["custom"] = {"lineWidth": 1, "fillOpacity": 8, "showPoints": "never", "spanNulls": 300000}
        p["options"] = {"legend": {"displayMode": "list", "placement": "bottom"}, "tooltip": {"mode": "multi", "sort": "desc"}, **(opts or {})}
    if ptype == "logs":
        p["options"] = {"showTime": True, "wrapLogMessage": True, "prettifyLogMessage": False, "enableLogDetails": True,
                        "dedupStrategy": "none", "sortOrder": "Descending", **(opts or {})}
    if ptype == "stat":
        p["options"] = {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False}, "colorMode": "value",
                        "graphMode": "area", "textMode": "value", **(opts or {})}
    return p

def unwrap(sel, pattern, field, rng="$__auto", fn="max_over_time"):
    # Metric extracted from the minute log line, e.g. heap free / rssi. The pattern must contain exactly ONE
    # named group (every extra group would become a label and split the series); aggregate by board so the
    # auto-detected labels (detected_level, service_name) never split it either.
    return f'max by (board) ({fn}({sel} |~ "{pattern}" | regexp "{pattern}" | unwrap {field} [{rng}]))'

def dashboard(uid, title, panels, templating=None, annotations=None, refresh="1m", tags=()):
    return {"uid": uid, "title": title, "tags": ["esp32", *tags], "timezone": "browser", "editable": True,
            "schemaVersion": 39, "version": 1, "refresh": refresh, "time": {"from": "now-24h", "to": "now"},
            "templating": {"list": templating or []}, "annotations": {"list": annotations or []}, "panels": panels}

def boot_annotation(sel):
    return {"name": "Boot", "enable": True, "iconColor": "red", "datasource": DS,
            "expr": f'{sel} |= "BOOT "', "titleFormat": "Boot", "textFormat": "{{board}}"}

HEAP = r'heap (?P<heap>\\d+) free'
LARG = r'largest (?P<largest>\\d+)'
RSSI = r'rssi (?P<rssi>-?\\d+)'
LDR  = r'ldr (?P<ldr>\\d+)'
BL   = r'backlight (?P<backlight>\\d+)'

# ---------------------------------------------------------------- per-board dashboard
B = '{board="$board"}'
BT = '{board="$board", tag=~"$tag"}'
board_panels = [
    panel("stat", "Heap free", 0, 0, 4, 4, [tgt(unwrap(B, HEAP, "heap"))], unit="bytes"),
    panel("stat", "Largest block", 4, 0, 4, 4, [tgt(unwrap(B, LARG, "largest"))], unit="bytes"),
    panel("stat", "RSSI", 8, 0, 4, 4, [tgt(unwrap(B, RSSI, "rssi"))], unit="dBm"),
    panel("stat", "Boots (range)", 12, 0, 4, 4, [tgt(f'sum(count_over_time({B} |= "BOOT " [$__range]))', instant=True)],
          opts={"colorMode": "background", "graphMode": "none"}),
    panel("stat", "Crashes (range)", 16, 0, 4, 4, [tgt(f'sum(count_over_time({B} |= "CRASH" [$__range]))', instant=True)],
          opts={"colorMode": "background", "graphMode": "none"}),
    panel("stat", "Warnings + errors (range)", 20, 0, 4, 4,
          [tgt(f'sum(count_over_time({{board="$board", level=~"warn|error"}} [$__range]))', instant=True)],
          opts={"colorMode": "background", "graphMode": "none"}),

    panel("timeseries", "Heap", 0, 4, 12, 8, [tgt(unwrap(B, HEAP, "heap"), "free", "A"),
                                            tgt(unwrap(B, LARG, "largest"), "largest block", "B")], unit="bytes",
          desc="From the minute status line. Restart threshold (heap watchdog) is 18 KB."),
    panel("timeseries", "Wi-Fi RSSI", 12, 4, 12, 8, [tgt(unwrap(B, RSSI, "rssi"), "rssi")], unit="dBm",
          desc="Below -80 dBm for 2 min the board forces a reconnect to find a better mesh node."),
    panel("timeseries", "Light sensor / backlight (info board)", 0, 12, 12, 7,
          [tgt(unwrap(B, LDR, "ldr"), "ldr", "A"), tgt(unwrap(B, BL, "backlight"), "backlight", "B")],
          desc="LDR: 0 daylight, ~600 evening, ~1700 dark. Only the info board logs these."),
    panel("timeseries", "Log lines per level", 12, 12, 12, 7,
          [tgt(f'sum by (level) (count_over_time({B} [$__auto]))', "{{level}}")],
          opts={"legend": {"displayMode": "list", "placement": "right"}},
          overrides=[{"matcher": {"id": "byName", "options": "error"}, "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "red"}}]},
                     {"matcher": {"id": "byName", "options": "warn"}, "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "orange"}}]},
                     {"matcher": {"id": "byName", "options": "boot"}, "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": "purple"}}]}]),
    panel("logs", "Boot reports", 0, 19, 24, 6, [tgt(f'{B} |= "BOOT "')]),
    panel("logs", "Warnings and errors", 0, 25, 24, 8, [tgt('{board="$board", level=~"warn|error"}')]),
    panel("logs", "All lines (filtered by tag)", 0, 33, 24, 14, [tgt(BT)]),
]
board_vars = [
    {"name": "board", "label": "Board", "type": "query", "datasource": DS, "refresh": 2, "sort": 1,
     "query": {"label": "board", "refId": "LokiVariableQueryEditor-VariableQuery", "stream": "", "type": 1},
     "definition": "label_values(board)", "current": {}, "options": []},
    {"name": "tag", "label": "Tag", "type": "query", "datasource": DS, "refresh": 2, "sort": 1, "multi": True,
     "includeAll": True, "allValue": ".*",
     "query": {"label": "tag", "refId": "LokiVariableQueryEditor-VariableQuery", "stream": '{board="$board"}', "type": 1},
     "definition": 'label_values({board="$board"}, tag)', "current": {"text": "All", "value": "$__all"}, "options": []},
]
board = dashboard("esp32-board", "ESP32 board", board_panels, board_vars, [boot_annotation(B)], tags=["board"])

# ---------------------------------------------------------------- overview dashboard
A = '{board=~".+"}'
over_panels = [
    panel("timeseries", "Heap free, all boards", 0, 0, 12, 8,
          [tgt(unwrap(A, HEAP, "heap"), "{{board}}")], unit="bytes"),
    panel("timeseries", "Wi-Fi RSSI, all boards", 12, 0, 12, 8,
          [tgt(unwrap(A, RSSI, "rssi"), "{{board}}")], unit="dBm"),
    panel("timeseries", "Warnings + errors per board", 0, 8, 12, 7,
          [tgt(f'sum by (board) (count_over_time({{board=~".+", level=~"warn|error"}} [$__auto]))', "{{board}}")]),
    panel("timeseries", "Log lines per board", 12, 8, 12, 7,
          [tgt(f'sum by (board) (count_over_time({A} [$__auto]))', "{{board}}")]),
    panel("logs", "Boot reports (all boards)", 0, 15, 24, 7, [tgt(f'{A} |= "BOOT "')]),
    panel("logs", "Recent warnings and errors", 0, 22, 24, 10, [tgt('{board=~".+", level=~"warn|error"}')]),
]
overview = dashboard("esp32-overview", "ESP32 overview", over_panels, [], [boot_annotation(A)], tags=["overview"])

os.makedirs(os.path.join(HERE, "dashboards"), exist_ok=True)
for d in (board, overview):
    path = os.path.join(HERE, "dashboards", d["uid"] + ".json")
    with open(path, "w") as f: json.dump(d, f, indent=1, ensure_ascii=False)
    print("wrote", path, len(d["panels"]), "panels")
