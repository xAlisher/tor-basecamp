# tor-basecamp — Specification

_Status: draft. Tracks [ecosystem#213](https://github.com/logos-co/ecosystem/issues/213)._

## 1. Problem

Basecamp apps that need to talk to the network privately currently each embed and drive their own `tor`: they ship a tor binary, write a `torrc`, launch and bootstrap it, open a SOCKS port, and (for authenticated onions) manage v3 client-auth keys. This is duplicated across **Radio**, **Receiver**, and **node-remote**, each with its own copy of the launch/SOCKS/auth logic. It is wasteful (N tor processes), inconsistent (three code paths to fix when tor behaviour changes), and a barrier for new apps that just want "fetch this URL privately".

## 2. Goal

A single **headless** capability module, `tor`, that:

1. Runs **one** shared tor instance for the whole Basecamp, bootstrapped once.
2. Exposes a simple **HTTP-over-Tor request** call (clearnet and `.onion`), buffered.
3. Exposes the shared **SOCKS endpoint** for callers that stream (Radio/Receiver audio) rather than do a one-shot request.
4. Supports **v3 client-authorized onions** (node-remote's model): register a client-auth key, then reach the auth-gated `.onion`.
5. Supports **circuit isolation** (fresh circuit per request/tag) for unlinkability.

It is **headless** — a core module with `interface: universal`, no UI. All function is over the module IPC. A separate **dev-only test UI** (`tor_test_ui`) exercises the calls.

## 3. Non-goals (for this module / this issue)

- **Hosting** a HiddenService (the inbound/server side that Radio's broadcaster and node-remote's onion service need). The issue is scoped to "HTTP **request** over Tor" (outbound/client). Server-side hosting is a natural sibling module/issue, not folded in here.
- **Pluggable transports / bridges / Snowflake** (censorship circumvention) — see [ecosystem#94](https://github.com/logos-co/ecosystem/issues/94). May be a later option (`set_bridges`).
- TLS/cert pinning beyond libcurl defaults.

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

## 5. Consumers (adoption targets)

The module only pays off if apps adopt it. Order by ease:

1. **Receiver** — pure SOCKS client → swap its torsocks/tor launch for `get_socks_endpoint()` (proof-of-consolidation migration).
2. **Radio** (client half) — same SOCKS reuse; its broadcaster HiddenService stays app-side until the server sibling exists.
3. **node-remote** — its outbound + client-auth needs map onto `register_client_auth` + `http_request`; its inbound onion service stays app-side.
4. **1-click fork** — replace `torsocks` runtime dep with the module for its outbound calls.

**2-day target:** the `tor` module + `tor_test_ui` + **one** app migrated (Receiver) as proof. The other migrations are fast-follow, tracked on #213.

## 6. Build / packaging

- Fork node-remote's module skeleton: `metadata.json` (`interface: universal`, LIDL codegen), `flake.nix` (`mkLogosModule`), `postInstall` bundling `tor` + libcurl.
- Builds go to `/extra` (`TMPDIR=/extra/tmp`), never root.
- Sign + publish per the `/release` flow; add a catalog card.

## 7. Open questions (confirm with fryorcraken on #213)

- API shape: full HTTP (verbs/headers/body, as specced) vs a minimal `fetch(url)`?
- Buffered only, or is a module-managed streaming handle (`open_stream`) in scope now?
- Client-auth key format: raw base32 private key, or the full `descriptor:x25519:...` auth line?
- One tor for everything, or allow a per-caller isolated tor when an app needs guaranteed separation?
- Does it also expose a control-port capability (NEWNYM, stream events) or keep that internal?
