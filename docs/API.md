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

## Summary — the call list

| call | purpose | consumer |
|---|---|---|
| `status()` | bootstrap state + socks port | all |
| `get_socks_endpoint()` | shared SOCKS for streaming | Radio, Receiver |
| `http_request(req)` | buffered HTTP over Tor | node-remote, 1-click, any app |
| `register_client_auth(req)` | add v3 client-auth key | node-remote |
| `remove_client_auth(req)` | remove a client-auth key | node-remote |
| `list_client_auth()` | list registered auth onions | node-remote |
| `new_circuit(req?)` | fresh circuit / isolation | any app |

### Deferred (v2 — flagged in SPEC §3, §7)
- `open_stream(url, opts)` — module-managed local loopback handle for streaming (vs raw SOCKS).
- `set_bridges(...)` — pluggable transports / Snowflake (ecosystem#94).
- control-port event subscription (stream/circuit events) if a consumer needs it.

## Test UI (`tor_test_ui`, dev-only)
Exercises every call above: a bootstrap/status indicator, a request builder (method + URL + headers + body) showing the decoded response, a client-auth manager (register/list/remove), and a "new circuit" button. Not shipped to operators; it is how we test calls and responses.
