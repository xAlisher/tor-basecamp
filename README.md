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

Early. See [SPEC.md](SPEC.md) for the design and [docs/API.md](docs/API.md) for the API calls.

## Scope

**In:** client HTTP/stream over Tor (clearnet + `.onion`), v3 client-authorized onions, a shared SOCKS endpoint, circuit isolation.
**Out (sibling work):** hosting a HiddenService (the server side that Radio's broadcaster and node-remote's onion service need). Pluggable transports / Snowflake ([ecosystem#94](https://github.com/logos-co/ecosystem/issues/94)).
