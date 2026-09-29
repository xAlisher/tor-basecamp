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
skp() { echo "  SKIP  $*"; }

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

echo "== T4  http_request over Tor (proves routing via check.torproject.org)"
REQ='{"method":"GET","url":"https://check.torproject.org/api/ip","timeout_ms":45000}'
ISTOR=""
for _ in 1 2 3; do
  H=$(call http_request "$REQ")
  ISTOR=$(echo "$H" | python3 -c '
import sys,json,base64
try:
    r=json.load(sys.stdin); r=r.get("value",r)
    b=base64.b64decode(r.get("body_b64","")).decode("utf-8","replace")
    print(str(r.get("status"))+" "+("IsTor:true" if "\"IsTor\":true" in b.replace(" ","") else "IsTor:false")+" kind="+str(r.get("error_kind")))
except Exception as e: print("parse-fail "+str(e))')
  echo "      $ISTOR"
  echo "$ISTOR" | grep -q '200 IsTor:true' && break
  sleep 4
done
if echo "$ISTOR" | grep -q '200 IsTor:true'; then ok "fetched over Tor, IsTor:true"
elif echo "$ISTOR" | grep -q 'connect_failed'; then skp "external check.torproject.org unreachable via this exit (env, not product) — self-loop T7 proves the path"
else bad "not routed through Tor: $ISTOR"; fi

echo "== T5  error_kind on an unresolvable host"
E5=$(call http_request '{"url":"http://no-such-host-xyzzy.invalid/","timeout_ms":15000}')
echo "      $(echo "$E5" | grep -oE '"error_kind":"[^"]*"|"ok":(true|false)')"
echo "$E5" | grep -qE '"error_kind":"(dns_failed|connect_failed)"' && ok "unresolvable -> dns/connect error_kind" || bad "wrong error_kind: $E5"

echo "== T6  new_circuit returns ok"
C6=$(call new_circuit '{}')
echo "      $C6"
echo "$C6" | grep -q '"ok":true' && ok "new_circuit ok" || bad "new_circuit failed: $C6"

echo "== T7  self-loop: host an onion + fetch it back over Tor"
ECHODIR=$(mktemp -d); MARK="SEALED-LOOP-OK-$$"; echo "$MARK" > "$ECHODIR/probe.txt"
HP=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
( cd "$ECHODIR" && exec python3 -m http.server "$HP" --bind 127.0.0.1 >/dev/null 2>&1 ) &
HTTP_PID=$!
OO=$(call create_onion_service "{\"local_port\":$HP,\"virtual_port\":80}")
echo "      $OO"
ONION=$(echo "$OO" | grep -oE '[a-z2-7]{56}\.onion' | head -1)
if [ -n "$ONION" ]; then
  GOT=""; LAST=""
  for _ in $(seq 1 30); do
    R=$(call http_request "{\"url\":\"http://$ONION/probe.txt\",\"timeout_ms\":30000}")
    LAST=$(echo "$R" | python3 -c 'import sys,json,base64
try:
 r=json.load(sys.stdin); r=r.get("value",r)
 print(base64.b64decode(r.get("body_b64","")).decode("utf-8","replace").strip())
except Exception as e: print("")' 2>/dev/null)
    [ "$LAST" = "$MARK" ] && { GOT=1; break; }
    sleep 5
  done
  [ -n "$GOT" ] && ok "hosted onion ${ONION%.onion}... fetched back over Tor" || bad "self-loop fetch failed (last body: '$LAST')"
else bad "create_onion_service returned no onion: $OO"; fi
kill $HTTP_PID 2>/dev/null; call remove_onion_service "{\"id\":\"${ONION%.onion}\"}" >/dev/null 2>&1; rm -rf "$ECHODIR"

echo "== T8  pairing: keypair -> auth onion -> authorize -> register -> fetch"
KP=$(call generate_client_auth_keypair)
PUB=$(echo "$KP" | python3 -c 'import sys,json;r=json.load(sys.stdin);r=r.get("value",r);print(r.get("public",""))')
PRIV=$(echo "$KP" | python3 -c 'import sys,json;r=json.load(sys.stdin);r=r.get("value",r);print(r.get("private",""))')
if [ -z "$PUB" ] || [ -z "$PRIV" ]; then bad "keypair gen failed: $KP"; else
  echo "      pub=${PUB:0:12}... priv=${PRIV:0:12}..."
  ED=$(mktemp -d); MK="PAIR-OK-$$"; echo "$MK" > "$ED/probe.txt"
  HP=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
  ( cd "$ED" && exec python3 -m http.server "$HP" --bind 127.0.0.1 >/dev/null 2>&1 ) & HPID=$!
  OO=$(call create_onion_service "{\"local_port\":$HP,\"require_auth\":true}")
  OID=$(echo "$OO" | grep -oE '[a-z2-7]{56}' | head -1)
  call authorize_client "{\"id\":\"$OID\",\"client_public\":\"$PUB\"}" >/dev/null
  call register_client_auth "{\"onion_host\":\"$OID.onion\",\"private_key\":\"$PRIV\"}" >/dev/null
  GOT=""; LAST=""
  for _ in $(seq 1 30); do
    R=$(call http_request "{\"url\":\"http://$OID.onion/probe.txt\",\"timeout_ms\":30000}")
    LAST=$(echo "$R" | python3 -c 'import sys,json,base64
try:
 r=json.load(sys.stdin);r=r.get("value",r);print(base64.b64decode(r.get("body_b64","")).decode("utf-8","replace").strip())
except: print("")')
    [ "$LAST" = "$MK" ] && { GOT=1; break; }; sleep 5
  done
  [ -n "$GOT" ] && ok "authorized+registered client fetched the auth onion" || bad "pairing fetch failed (last: '$LAST')"
  LAC=$(call list_authorized_clients "{\"id\":\"$OID\"}")
  echo "$LAC" | grep -q "$PUB" && ok "list_authorized_clients shows the key" || bad "LAC missing key: $LAC"
  kill $HPID 2>/dev/null; call remove_onion_service "{\"id\":\"$OID\"}" >/dev/null 2>&1; rm -rf "$ED"
fi

echo; echo "  RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
