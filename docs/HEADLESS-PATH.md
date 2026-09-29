# Autonomous headless path — Phase 1 + Phase 2

The goal: build and **verify** the `tor` module with **no GUI, no display, no phone, no external service** — so development is autonomous and every acceptance criterion has a headless gate (SKILL §6). The key enabler: `tor` has **no module dependencies**, so it can **test itself** — host an onion service and connect to it over Tor in one process.

## Harness (mirror node-remote `tests/`)
- Runner: the `logoscore` **daemon+client** (`logoscore daemon` / `load-module` / `call <mod> <method> [json]` / `stop`). Locate: `find /nix/store -name logoscore -path '*/bin/*'`.
- Stage a `-m` dir with just `tor` (+ its bundled tor binary); no deps to stage.
- Isolate `XDG_DATA_HOME` (tor's DataDirectory + module state) to a stable cache path (not mktemp — a cold tor consensus per run looks like a never-publishing onion).
- Unwrap the daemon's double-JSON envelope (`.result` is an escaped string) — copy `lib.sh` `lc_unwrap`.
- Fixture: a local HTTP echo (`python3 -m http.server` or a tiny socket echo) on loopback; the module hosts an onion to it and fetches it back.

## Phase 1 — client (all headless)
- **#4 status + get_socks_endpoint** — `call tor status` polled until `bootstrapped`; assert `socks_port`; open a SOCKS5 handshake to it. No fixture beyond tor bootstrapping.
- **#5 http_request + new_circuit** — `call tor http_request` to a **stable clearnet URL over Tor** (assert 200 + body) and error kinds (bad host → dns, refused → connect, slow → timeout); `new_circuit` then re-request. Clearnet-over-tor needs outbound UDP/TCP to the Tor network (the one external dependency — the Tor network itself; no app service).
- **#6 client auth** — verified as part of the Phase-2 self-loop below (needs an auth onion to connect to, which the module itself hosts).

## Phase 2 — server, and it closes the loop (all headless)
- **create_onion_service → self-loop** *(the keystone test)*: start a local HTTP echo on `127.0.0.1:P`; `call tor create_onion_service {local_port:P}` → get `<x>.onion`; `call tor http_request {url:"http://<x>.onion/..."}` → assert the echo comes back. This proves **hosting + client + Tor round-trip** in one headless process, no external onion.
- **pairing loop (covers #6)**: `create_onion_service {require_auth:true}` → `generate_client_auth_keypair` → `authorize_client {pub}` → `register_client_auth {onion, priv}` → `http_request` to the onion **succeeds**; then `deauthorize_client` (or drop the key) → the same request **fails** (`auth_failed` / no route). Proves both auth halves end to end, headless.

## Off the autonomous path (wetware — build, but verify by hand later)
- **#7 `tor_test_ui`** (ui_qml GUI) — headless render check only (qmltest); real use is manual.
- **#8 Receiver migration**, **node-remote migration** — real-env/streaming/phone. Verified with the human, not in CI.

## Definition of "green" for the autonomous slices
A slice is green only when its `tests/tor_test.sh` case passes against a freshly `nix build`-ed `.lgx` loaded by the `logoscore` daemon — not when it merely compiles (SKILL §6, §pitfalls). The self-loop + pairing loop are the end-to-end proof that the module is usable before any GUI or app touches it.
