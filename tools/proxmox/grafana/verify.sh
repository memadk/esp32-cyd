#!/bin/sh
# Runs inside the log container: quick health check of the Loki/Alloy/Grafana pipeline.
echo "services: $(systemctl is-active esp32-logserver loki alloy grafana-server | tr '\n' ' ')"
echo "loki: $(curl -s -m 5 http://127.0.0.1:3100/ready)"
echo "grafana: $(curl -s -m 5 http://127.0.0.1:3000/api/health | tr -d '\n ')"
echo "alloy errors (5 min): $(journalctl -u alloy --since -5min -o cat --no-pager | grep -ciE 'level=(error|warn)')"
journalctl -u alloy --since -5min -o cat --no-pager | grep -iE 'level=(error|warn)' | tail -3 | cut -c1-250
q() { curl -s -m 90 -G http://127.0.0.1:3100/loki/api/v1/query --data-urlencode "query=$1" \
      | python3 -c 'import sys,json
raw=sys.stdin.read()
try: d=json.loads(raw)
except Exception: print("  RAW:", raw[:300]); sys.exit()
for r in d.get("data",{}).get("result",[]): print("  ", r["metric"], r["value"][1])'; }
R="${1:-14d}"
echo "lines per board [$R]";   q "sum by (board) (count_over_time({board=~\".+\"}[$R]))"
echo "lines per level [$R]";   q "sum by (level) (count_over_time({board=~\".+\"}[$R]))"
echo "BOOT per board [$R]";    q "sum by (board) (count_over_time({board=~\".+\"} |= \"BOOT \"[$R]))"
echo "CRASH per board [$R]";   q "sum by (board) (count_over_time({board=~\".+\"} |= \"CRASH\"[$R]))"
echo "tags";                   curl -s -m 5 http://127.0.0.1:3100/loki/api/v1/label/tag/values; echo
echo "dashboards: $(curl -s -m 5 -u admin:admin 'http://127.0.0.1:3000/api/search?type=dash-db' | python3 -c 'import sys,json; print([d["title"] for d in json.load(sys.stdin)])' 2>/dev/null)"
echo "datasources: $(curl -s -m 5 -u admin:admin http://127.0.0.1:3000/api/datasources | python3 -c 'import sys,json; print([d["name"] for d in json.load(sys.stdin)])' 2>/dev/null)"
free -m | sed -n 2p
