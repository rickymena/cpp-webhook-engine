# cpp-webhook-engine

A self-hosted webhook / automation engine written in C++17 from scratch —
standard library and POSIX only, no third-party dependencies. Webhooks
come in over HTTP; automations run in response, either as config-defined
actions (shell commands, HTTP requests) or as **your own code in
TypeScript/JavaScript or Python**, connected through thin SDKs. Think of
it as a headless, single-binary take on the core of tools like n8n, with a
Temporal-style worker model for custom logic:

```ts
import { Engine } from "webhook-engine-sdk";

const engine = new Engine("http://localhost:8080");
engine.on("deploy-blog", async (event) => {
  if (event.json.ref === "refs/heads/main") await deploy();
});
engine.run(); // subscribes over WebSocket — no public URL needed
```

**Status:** early development. The hardened HTTP server receives, logs,
and stores webhook payloads today (binary-safe bodies, size caps, read
timeouts, graceful shutdown, tests); signature verification, the config
file, the WebSocket integration API, and the SDKs are in progress.

## Why from scratch?

The point of this project is depth: the HTTP server, request parsing, and
(coming) WebSocket server (RFC 6455), TOML/JSON parsers, and
HMAC-SHA256 verification are all hand-written rather than pulled in as
libraries. The SDKs follow the same spirit — zero runtime dependencies
(Node 22–24 supported via the native `WebSocket` global; Python stdlib
only).

## Building

Requires CMake ≥ 3.10 and a C++17 compiler (GCC or Clang) on Linux.

```sh
mkdir -p build && cd build
cmake ..
make
```

The binary lands in `build/bin/webhook_server`.

### Tests

Plain-assert test binaries (no framework) are wired into CTest —
request parsing, route matching, binary-body integrity, and live-socket
integration tests (fragmented large uploads, oversize/chunked
rejection, graceful shutdown):

```sh
cd build && ctest --output-on-failure
```

## Running

```sh
./bin/webhook_server -p 8080 -v --save-payloads
```

| Flag | Description |
|------|-------------|
| `-p`, `--port <n>` | Port to listen on (default 8080) |
| `-v`, `--verbose` | Verbose logging |
| `--save-payloads` | Save each webhook body to `./logs/payloads/` as a timestamped file (`.json` or `.bin` by content type) |

Logs go to stdout and `./logs/webhook_server.log`. `SIGINT`/`SIGTERM`
shut down gracefully, draining in-flight requests first.

### Current server hardening

- Requests are read with a proper framing loop (headers, then exactly
  `Content-Length` bytes) — bodies are treated as opaque binary and
  survive byte-for-byte.
- Limits enforced before parsing: 16 KiB header cap (431), 1 MiB body
  cap (413), per-connection read timeouts and a whole-request deadline,
  and a concurrent-connection cap (503).
- `Transfer-Encoding: chunked` is rejected with 501 rather than
  half-implemented (request-smuggling surface).
- Header lookups are case-insensitive per RFC 7230; payload files are
  written `0600` in a `0700` directory.

## Endpoints

| Route | Method | Description |
|-------|--------|-------------|
| `/webhook` | POST | Receives a webhook; logs it and returns `{"status":"received"}` |
| `/health` | GET | Health check, returns `{"status":"healthy"}` |

Try it:

```sh
curl -X POST localhost:8080/webhook -d '{"event":"test"}'
curl localhost:8080/health
```

## Roadmap

- **Config-driven endpoints** — define routes in a single TOML file
  (hand-rolled parser), no recompiling to add an automation.
- **HMAC-SHA256 signature verification** — GitHub-style per-endpoint
  webhook authentication, implemented from scratch.
- **WebSocket integration API** — RFC 6455 from scratch; apps subscribe,
  receive events, and ack over one connection.
- **SDKs** — `sdk/typescript` (npm, Node 22–24, zero deps) and
  `sdk/python` (PyPI, stdlib only) implementing the worker model:
  `engine.on(name, handler)` + `engine.run()`.
- **Built-in steps** — run commands with timeouts, make outbound HTTP
  requests, template payload fields into actions — for automations that
  don't need custom code.
- **Reliability** — event queue with redelivery, retries with backoff,
  run history.

## Notes

- Saved payloads and logs may contain secrets, so `logs/` is git-ignored.
- TLS is intentionally out of scope; run behind a reverse proxy (nginx,
  Caddy) in real deployments.
