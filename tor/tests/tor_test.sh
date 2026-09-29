#!/usr/bin/env bash
# tor module — headless test. No Basecamp, no display, no phone.
#
# Phase 1 #4 gate:
#   T1  module loads under the logoscore daemon and status() is callable
#   T2  tor bootstraps to 100% and status() reports a live SOCKS port
#   T3  get_socks_endpoint() returns that same loopback port, and a SOCKS5
#       handshake on it succeeds
#
# Runs against a freshly-built .lgx (SKILL §6: green means loaded+called, not compiled).
set -uo pipefail
MOD=tor
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LGX="$(find -L "$HERE/result" -name '*.lgx' 2>/dev/null | head -1)"
LOGOSCORE=$(find /nix/store -maxdepth 4 -name logoscore -path '*/bin/*' 2>/dev/null | head -1)
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

[ -f "$LGX" ]       || { echo "no .lgx at $HERE/result — run: nix build .#lgx-portable --impure"; exit 1; }
[ -n "$LOGOSCORE" ] || { echo "logoscore not found"; exit 1; }

# Isolate module state (tor DataDirectory). Stable path, not mktemp — a fresh dir
# each run forces a cold consensus fetch that looks like tor never bootstrapping.
export XDG_DATA_HOME="${TOR_TEST_HOME:-$HOME/.cache/tor-module-test-home}"
mkdir -p "$XDG_DATA_HOME"
export LOGOSCORE_CONFIG_DIR=$(mktemp -d)
MDIR=$(mktemp -d)

cleanup() {
  "$LOGOSCORE" stop >/dev/null 2>&1
  pkill -f "tor -f $XDG_DATA_HOME" 2>/dev/null
  rm -rf "$MDIR" "$LOGOSCORE_CONFIG_DIR"
}
trap cleanup EXIT

# Stage the module dir from the lgx: plugin + bundled tor + a -dev-resolvable manifest.
mkdir -p "$MDIR/$MOD"
tar xzf "$LGX" -C "$MDIR/$MOD" --strip-components=2 variants/linux-amd64 2>/dev/null
tar xzf "$LGX" -O manifest.json > "$MDIR/$MOD/manifest.json" 2>/dev/null
python3 - "$MDIR/$MOD/manifest.json" <<'PY'
import json,sys
p=sys.argv[1]; m=json.load(open(p))
main=m.get("main")
so = main.get("linux-amd64") if isinstance(main,dict) else (main if isinstance(main,str) else "tor_plugin.so")
if not so.endswith(".so"): so=so+".so"
m["main"]={"linux-amd64":so,"linux-amd64-dev":so,"linux-x86_64-dev":so}
json.dump(m,open(p,"w"),indent=2)
PY
echo linux-amd64-dev > "$MDIR/$MOD/variant"

call() {  # call <method> [args...]  → unwrapped JSON
  "$LOGOSCORE" call "$MOD" "$@" 2>&1 | python3 -c '
import sys,json
raw=sys.stdin.read().strip()
try: env=json.loads(raw)
except Exception: print(raw); sys.exit()
r=env.get("result",env)
if isinstance(r,str):
    try: r=json.loads(r)
    except Exception: pass
print(json.dumps(r,separators=(",",":")) if not isinstance(r,str) else r)
'
}

"$LOGOSCORE" -D -m "$MDIR" > /tmp/tor-module-daemon.log 2>&1 &
for _ in $(seq 1 20); do "$LOGOSCORE" status >/dev/null 2>&1 && break; sleep 1; done

echo "== T1  module loads, status() callable"
"$LOGOSCORE" load-module "$MOD" >/dev/null 2>&1
S=$(call status)
if echo "$S" | grep -q '"bootstrapped"'; then ok "loads + status() → $S"
else bad "load/call failed: $S"; echo "--- daemon log ---"; tail -25 /tmp/tor-module-daemon.log; exit 1; fi

echo "== T2  tor bootstraps to 100% with a live SOCKS port (up to 120s)"
SOCKS=0
for _ in $(seq 1 40); do
  S=$(call status)
  echo "$S" | grep -q '"bootstrapped":true' && { SOCKS=$(echo "$S" | grep -oE '"socks_port":[0-9]+' | grep -oE '[0-9]+'); break; }
  sleep 3
done
if [ "${SOCKS:-0}" -gt 0 ] 2>/dev/null; then ok "bootstrapped:true, socks_port=$SOCKS ($(echo "$S" | grep -oE '"tor_version":"[^"]*"'))"
else bad "did not bootstrap: $S"; tail -20 /tmp/tor-module-daemon.log; fi

echo "== T3  get_socks_endpoint() matches + SOCKS5 handshake succeeds"
E=$(call get_socks_endpoint); echo "      $E"
EP=$(echo "$E" | grep -oE '"port":[0-9]+' | grep -oE '[0-9]+')
if [ "${EP:-0}" = "${SOCKS:-x}" ] && [ "${EP:-0}" -gt 0 ] 2>/dev/null; then
  # SOCKS5 no-auth handshake: send 05 01 00, expect 05 00
  R=$(printf '\x05\x01\x00' | timeout 5 python3 -c '
import sys,socket
p=int(sys.argv[1]); s=socket.create_connection(("127.0.0.1",p),3); s.sendall(b"\x05\x01\x00")
d=s.recv(2); print("ok" if d==b"\x05\x00" else "bad:"+d.hex())' "$EP" 2>&1)
  [ "$R" = "ok" ] && ok "endpoint matches status; SOCKS5 handshake ok on $EP" || bad "socks handshake: $R"
else bad "endpoint/port mismatch: status socks=$SOCKS endpoint=$EP"; fi

echo; echo "  RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
