# tor-basecamp

A **headless Basecamp capability module** that wraps HTTP requests (and raw streams) over **Tor**, so any Logos Basecamp module or app gets anonymous networking without bundling and driving its own `tor` process.

Tracks [logos-co/ecosystem#213](https://github.com/logos-co/ecosystem/issues/213) — "Module to wrap HTTP request over TOR".

## Why

Three of our own apps already roll their own Tor stack:

| app | rolls |
|---|---|
| **Radio** (booth) | onion HiddenService (broadcaster) + torsocks/SOCKS client |
| **Receiver** | torsocks/SOCKS client → onion stations |
| **node-remote** | onion + **v3 client authorization** |

That is three independent tor launches, three torrc templates, three SOCKS/auth code paths. `tor-basecamp` consolidates the **client** side into one module: **one tor instance, one client-auth implementation, one HTTP-over-Tor API**, shared by every caller.

## Layout

- **`tor/`** — the headless core module (`interface: universal`). No UI. Exposes the API in [docs/API.md](docs/API.md) over the module IPC.
- **`tor_test_ui/`** — a small **dev-only** UI plugin to exercise the calls and inspect responses (not shipped to operators).

## Status

**v0.1.0** — Phase 1 (client) + Phase 2 (onion hosting & v3 pairing) complete. Headless harness passes 9/9 (`tor/tests/tor_test.sh`), including a self-loop and the full pairing round-trip; verified live in Basecamp (`http_request` → `IsTor:true`). See [SPEC.md](SPEC.md) for the design and [docs/API.md](docs/API.md) for the API calls.

## Install (Linux x86-64)

Grab the two `.lgx` from the [v0.1.0 release](https://github.com/xAlisher/tor-basecamp/releases/tag/v0.1.0) (signed by xAlisher) and install `tor` first (it's the dependency), then the harness:

```
lgpm install --file tor-0.1.0-linux-amd64.lgx
lgpm install --file tor_test_ui-0.1.0-linux-amd64.lgx
```

`tor` bundles its own `tor` + `curl` + CA bundle — no system `tor`/`torsocks` needed.

**Build/signing note:** the current `mkLogosModule` publishes each module's LIDL to a root `assets/lidl/`, but `liblgx`'s validator forbids a root `assets/` entry, so a freshly-built `.lgx` can't be signed as-is. Release artifacts are conformed with [`tools/conform-lgx.py`](tools/conform-lgx.py) (strips root `assets/`, recomputes the `root` Merkle hash exactly as liblgx does) then signed. The LIDL is a build-time codegen aid, not needed at runtime. Reconciling `nix-bundle-lgx` (emits root `assets/`) with `logos-package` `liblgx` (forbids it) is tracked upstream.

## Scope

Full Tor capability, **client and server**, enough to replace every consumer's tor:
- **Client:** HTTP/stream over Tor (clearnet + `.onion`), connect to v3 client-authorized onions, shared SOCKS endpoint, circuit isolation.
- **Server:** host persistent v3 HiddenServices (`.onion` -> local port), and server-side client authorization (mint keypair, authorize/deauthorize clients, hot-reload).

Delivered in phases (client first, then hosting) — see [SPEC.md](SPEC.md) §5.

**Out (sibling work):** pluggable transports / Snowflake ([ecosystem#94](https://github.com/logos-co/ecosystem/issues/94)); app-level protocols that run *over* the tunnel (SAS pairing, station identity) stay in the apps.
