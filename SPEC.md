# tor-basecamp — Specification

_Status: draft. Tracks [ecosystem#213](https://github.com/logos-co/ecosystem/issues/213)._

## 1. Problem

Basecamp apps that need to talk to the network privately currently each embed and drive their own `tor`: they ship a tor binary, write a `torrc`, launch and bootstrap it, open a SOCKS port, and (for authenticated onions) manage v3 client-auth keys. This is duplicated across **Radio**, **Receiver**, and **node-remote**, each with its own copy of the launch/SOCKS/auth logic. It is wasteful (N tor processes), inconsistent (three code paths to fix when tor behaviour changes), and a barrier for new apps that just want "fetch this URL privately".

## 2. Goal

A single **headless** capability module, `tor`, that owns **all** of a Basecamp app's Tor needs — client **and** server — so no app launches its own tor. It must be complete enough to fully replace the tor stacks in **node-remote**, **Radio**, and **Receiver**. Concretely:

**Client side**
1. Runs **one** shared tor instance for the whole Basecamp, bootstrapped once.
2. A buffered **HTTP-over-Tor request** call (clearnet and `.onion`).
3. The shared **SOCKS endpoint** for callers that stream (Radio/Receiver audio) rather than do a one-shot request.
4. **Connect to v3 client-authorized onions**: register a client-auth *private* key, then reach the auth-gated `.onion`.
5. **Circuit isolation** (fresh circuit per request/tag) for unlinkability.

**Server side** (required to replace node-remote + Radio's broadcaster)
6. **Host a v3 HiddenService**: forward an `.onion` to a local port, with a **persistent** key so the address survives restarts.
7. **Server-side client authorization**: generate a client-auth keypair, authorize/deauthorize client public keys on a hosted service, reloaded **without a restart** (SIGHUP).

One tor instance serves both: a client `SocksPort` **and** one or more HiddenServices simultaneously (node-remote uses `SocksPort 0` only because it never needed a client — a unified module runs both).

It is **headless** — a core module with `interface: universal`, no UI. All function is over the module IPC. A separate **dev-only test UI** (`tor_test_ui`) exercises the calls.

## 3. Non-goals

- **Pluggable transports / bridges / Snowflake** (censorship circumvention) — see [ecosystem#94](https://github.com/logos-co/ecosystem/issues/94). Possible later option (`set_bridges`).
- TLS/cert pinning beyond libcurl defaults.
- App-level protocols built *on top* of the tunnel (e.g. node-remote's SAS pairing handshake, Radio's station identity) stay in the apps; this module provides the transport + onion + auth primitives they run over.

## 4. Design

### 4.1 One shared tor
On module load, `tor` launches a single bundled `tor` process into a private run dir with a generated `torrc` (a `SocksPort`, no `HiddenService`), and bootstraps it. All callers share this one SOCKS proxy. `status()` reports bootstrap progress so callers can wait for readiness.

- Reuse node-remote's tor-launch/bootstrap machinery (`onion_service.cpp`: locate the tor binary — bundled first, then `$TOR_BIN`, then PATH — write torrc, start, watch bootstrap). Strip the HiddenService config; keep SocksPort + bootstrap.
- Bundle `tor` in the `.lgx` (node-remote's `postInstall` pattern), so no system tor is required.

### 4.2 HTTP requests
`http_request` performs a buffered HTTP request through the shared SOCKS proxy using **libcurl** with `CURLOPT_PROXY = socks5h://127.0.0.1:<socksPort>` (the `h` sends DNS through Tor too, required for `.onion`). Request and response bodies are **base64** in the IPC payload (binary-safe). Reuse the HTTP-over-tor pattern already used by the 1-click fork (`torsocks`) and Receiver.

### 4.3 Streaming
For long-lived streams (Radio/Receiver audio), a buffered request does not fit. Callers get the shared SOCKS endpoint via `get_socks_endpoint()` and point their existing stream client at it (`socks5h://host:port`). This reuses the one bootstrapped tor rather than each app launching its own. (A future `open_stream` that returns a module-managed local loopback handle is a possible v2.)

### 4.4 Client authorization (v3 onion auth)
`register_client_auth(onion_host, private_key)` installs a v3 client-auth key into tor's `ClientOnionAuthDir` and reloads tor, so subsequent `http_request`/SOCKS connections to that auth-gated `.onion` succeed. This is the node-remote pairing model, generalized. `remove_client_auth` / `list_client_auth` manage the set. Reuse node-remote's existing client-auth code (it already implements this).

### 4.5 Circuit isolation
`new_circuit(isolation_tag)` requests a fresh circuit (via `SIGNAL NEWNYM` on the control port, or SOCKS username/password stream-isolation per `isolation_tag`). `http_request` accepts an `isolation_tag` so unrelated requests do not share a circuit.

### 4.6 Onion service hosting (server)
`create_onion_service(local_port, opts)` publishes a v3 HiddenService that forwards `.onion:virtual_port` to `127.0.0.1:local_port`, and returns the `.onion`. The service **key is persistent** (`persist_id` names a keyed dir), so the address is stable across restarts — this is essential for node-remote (the phone keeps the same pairing) and Radio (a station keeps its address). `onion_service_status(id)` reports whether it is published; `remove_onion_service(id)` tears it down. The same tor instance keeps its client `SocksPort` open. Reuse node-remote's `onion_service.cpp` (persistent `HiddenServiceDir`, `HiddenServicePort`, reading the `hostname` file, immediate-exit diagnostics).

### 4.7 Pairing — both halves of v3 client auth
The two auth halves live in one module:
- **Server:** `generate_client_auth_keypair()` mints an x25519 keypair (reuse `pairing.cpp` `generateClientAuthKey`). `authorize_client(onion_id, client_public)` writes the public half into the service's `authorized_clients/` and reloads tor via **SIGHUP** (no restart, no dropped connections); `deauthorize_client` / `list_authorized_clients` manage the set. An empty `authorized_clients/` still means "auth required, nobody admitted" (do not treat empty as open).
- **Client:** `register_client_auth(onion_host, private_key)` (§4.4) installs the *private* half so this node can reach another's auth-gated service.

An app pairs by: host a service (`create_onion_service`), mint a keypair (`generate_client_auth_keypair`), `authorize_client` with the public half, and hand the private half to the peer out-of-band (the app's own channel + SAS). The peer calls `register_client_auth` with it. The module provides the primitives; the SAS/handshake stays in the app.

## 5. Consumers (adoption targets)

The module now covers every consumer end to end. Order by ease:

1. **Receiver** — pure SOCKS client → swap its torsocks/tor launch for `get_socks_endpoint()`. The simplest migration, so it is the proof-of-consolidation.
2. **1-click fork** — replace the `torsocks` runtime dep with `http_request` for outbound calls.
3. **Radio** — client half via SOCKS; broadcaster via `create_onion_service` + serve its stream through the hosted `.onion`.
4. **node-remote** — the fullest consumer: `create_onion_service` (persistent) for its onion, `generate_client_auth_keypair` + `authorize_client` for pairing, and `register_client_auth` + `http_request` for its client side. Its SAS handshake stays app-side.

**Phased delivery** (this is bigger than the client-only cut):
- **Phase 1 (the ~2-day target):** `tor` module + `tor_test_ui` covering the **client** family (`status`, `get_socks_endpoint`, `http_request`, client `register_client_auth`, `new_circuit`) + **Receiver** migrated as proof.
- **Phase 2:** the **server/hosting** family (`create_onion_service`, `generate_client_auth_keypair`, `authorize_client` …) + **node-remote** migrated (the real test of the pairing surface).
- **Phase 3:** Radio broadcaster + 1-click.

Migrations are tracked on #213; the module ships usable after Phase 1.

## 6. Build / packaging

- Fork node-remote's module skeleton: `metadata.json` (`interface: universal`, LIDL codegen), `flake.nix` (`mkLogosModule`), `postInstall` bundling `tor` + libcurl.
- Builds go to `/extra` (`TMPDIR=/extra/tmp`), never root.
- Sign + publish per the `/release` flow; add a catalog card.

## 7. Open questions (confirm with fryorcraken on #213)

- Buffered `http_request` only for Phase 1, or is a module-managed streaming handle (`open_stream`) needed beyond the raw SOCKS endpoint?
- Client-auth key format over IPC: raw base32 x25519, or the full `descriptor:x25519:<b32>` auth line?
- One shared tor for everything, or allow a per-caller isolated tor instance when an app needs guaranteed separation (e.g. node-remote wants its own)?
- Persistent onion keys: where do they live so they survive a module reinstall (module data dir vs app-provided path)?
- Expose a control-port capability (NEWNYM, stream/circuit events) to callers, or keep it internal?
