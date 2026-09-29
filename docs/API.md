# tor module — API

All calls are module IPC methods on the headless `tor` module (`interface: universal`, so all fields are strings / JSON strings; binary is base64). Every response includes `ok: bool` and, on failure, `error: string` + `error_kind: string`. Error kinds: `not_bootstrapped`, `bad_request`, `dns_failed`, `connect_failed`, `timeout`, `auth_failed`, `tor_failed`, `internal`.

Payloads below are shown as JSON for clarity; over the universal interface each is a JSON string argument / return.

---

## Lifecycle / readiness

### `status()`
Report the shared tor instance state. Callers should wait for `bootstrapped: true` before requesting.
```json
// -> 
{ "ok": true, "bootstrapped": true, "progress": 100,
  "socks_host": "127.0.0.1", "socks_port": 9250,
  "tor_version": "0.4.x", "error": "" }
```

### `get_socks_endpoint()`
The shared SOCKS5 proxy, for callers that **stream** (point their own client at `socks5h://host:port`).
```json
// ->
{ "ok": true, "host": "127.0.0.1", "port": 9250 }
```

---

## HTTP over Tor

### `http_request(request)`
Perform one buffered HTTP request over Tor (clearnet or `.onion`; DNS resolved over Tor).
```json
// request:
{
  "method": "GET",                 // GET|POST|PUT|PATCH|DELETE|HEAD
  "url": "http://<...>.onion/path", // or https://clearnet
  "headers": { "Accept": "application/json" },
  "body_b64": "",                  // request body, base64 (optional)
  "timeout_ms": 30000,             // total timeout (optional)
  "connect_timeout_ms": 15000,     // connect timeout (optional)
  "isolation_tag": "receiver-fetch" // optional: pin to its own circuit
}
// ->
{
  "ok": true,
  "status": 200,
  "headers": { "content-type": "application/json" },
  "body_b64": "<base64 response body>",
  "final_url": "http://<...>.onion/path",
  "error": "", "error_kind": ""
}
```

---

## Client authorization (v3 authenticated onions)

### `register_client_auth(request)`
Install a v3 client-auth key so an auth-gated `.onion` becomes reachable by subsequent `http_request` / SOCKS use. (node-remote's pairing model, generalized.)
```json
// request:
{ "onion_host": "<56-char>.onion", "private_key": "<base32 x25519 priv>" }
// ->
{ "ok": true, "error": "" }
```

### `remove_client_auth(request)`
```json
// request:
{ "onion_host": "<56-char>.onion" }
// ->
{ "ok": true }
```

### `list_client_auth()`
```json
// ->
{ "ok": true, "onions": ["<...>.onion", "<...>.onion"] }
```

---

## Circuits

### `new_circuit(request)`
Force a fresh circuit (privacy / retry after a bad exit). With `isolation_tag`, only that tag's stream group gets a new circuit; without it, a global `NEWNYM`.
```json
// request (optional):
{ "isolation_tag": "receiver-fetch" }
// ->
{ "ok": true }
```

---

## Onion service hosting (server)

### `create_onion_service(request)`
Publish a v3 HiddenService forwarding an `.onion` to a local port. Persistent key -> stable address across restarts.
```json
// request:
{
  "local_port": 8099,          // 127.0.0.1:<local_port> to forward to
  "virtual_port": 80,          // the port on the .onion (default 80)
  "persist_id": "node-remote", // names the persistent key dir; omit for ephemeral
  "require_auth": true         // v3 client authorization required to connect
}
// ->
{ "ok": true, "id": "node-remote", "onion": "<56-char>.onion", "error": "" }
```

### `onion_service_status(request)`
```json
// request: { "id": "node-remote" }
// ->
{ "ok": true, "published": true, "onion": "<56-char>.onion" }
```

### `remove_onion_service(request)`
```json
// request: { "id": "node-remote" }
// ->
{ "ok": true }
```

## Pairing — server side of client auth

### `generate_client_auth_keypair()`
Mint an x25519 keypair. The server keeps/authorizes the public half; the private half is handed to the peer (out-of-band, via the app's own channel) for its `register_client_auth`.
```json
// ->
{ "ok": true, "public": "<base32 x25519 pub>", "private": "<base32 x25519 priv>" }
```

### `authorize_client(request)`
Add a client's public key to a hosted service's `authorized_clients` and reload tor (SIGHUP, no restart).
```json
// request: { "id": "node-remote", "client_public": "<base32 x25519 pub>" }
// ->
{ "ok": true }
```

### `deauthorize_client(request)`  /  `list_authorized_clients(request)`
```json
// deauthorize -> { "id": "...", "client_public": "..." }  =>  { "ok": true }
// list        -> { "id": "..." }                          =>  { "ok": true, "clients": ["<pub>", ...] }
```

---

## Summary — the call list

**Client**

| call | purpose | consumer |
|---|---|---|
| `status()` | bootstrap state + socks port | all |
| `get_socks_endpoint()` | shared SOCKS for streaming | Radio, Receiver |
| `http_request(req)` | buffered HTTP over Tor | node-remote, 1-click, any app |
| `register_client_auth(req)` | add a client-auth **private** key (to connect) | node-remote client |
| `remove_client_auth(req)` / `list_client_auth()` | manage connect keys | node-remote client |
| `new_circuit(req?)` | fresh circuit / isolation | any app |

**Server / hosting** (Phase 2 — replaces node-remote + Radio broadcaster)

| call | purpose | consumer |
|---|---|---|
| `create_onion_service(req)` | host a persistent v3 `.onion` -> local port | node-remote, Radio |
| `onion_service_status(req)` | published? address? | node-remote, Radio |
| `remove_onion_service(req)` | tear down a hosted service | node-remote, Radio |
| `generate_client_auth_keypair()` | mint an x25519 client-auth keypair | node-remote |
| `authorize_client(req)` | admit a client pubkey (SIGHUP reload) | node-remote |
| `deauthorize_client(req)` / `list_authorized_clients(req)` | manage admitted clients | node-remote |

`register_client_auth` (client) + `generate_client_auth_keypair` + `authorize_client` (server) are the two halves of the pairing.

### Deferred (v2 — SPEC §3)
- `open_stream(url, opts)` — module-managed local loopback handle for streaming (vs raw SOCKS).
- `set_bridges(...)` — pluggable transports / Snowflake (ecosystem#94).
- control-port event subscription (stream/circuit events) if a consumer needs it.

## Test UI (`tor_test_ui`, dev-only)
Exercises every call above: a bootstrap/status indicator, a request builder (method + URL + headers + body) showing the decoded response, a client-auth manager (register/list/remove), and a "new circuit" button. Not shipped to operators; it is how we test calls and responses.
