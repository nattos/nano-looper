# Design: Resolume WebSocket Client & Mock Server

## Goal

Build an FFGL plugin ("looper") that connects back to its host Resolume process via the Resolume WebSocket API. This codebase provides the foundational layers: a mock server to develop against, a protocol library, a composition model, and a WebSocket client — all tested end-to-end before the plugin itself is built.

## Resolume WebSocket Protocol

Discovered by probing a live Resolume Arena instance (documented in `tmp/resolume/PROBE.md`). The REST API is largely non-functional; the WebSocket is the primary control surface.

### Connection

- Endpoint: `ws://<host>:<port>/api/v1`
- On connect, the server sends a single large JSON message containing the entire composition state (layers, clips, parameters, effects, etc.)
- The only working REST endpoint is `GET /api/v1/product` (returns name and version)

### Parameter Model

Every controllable value in Resolume is a **parameter** with a numeric `id` and a `valuetype`:

| Type | Description | Control action |
|------|-------------|----------------|
| `ParamRange` | Continuous value with min/max (sliders, faders) | `set` |
| `ParamString` | Text value | `set` |
| `ParamBoolean` | On/off | `set` |
| `ParamChoice` | Enum with named options | `set` |
| `ParamTrigger` | Fire-once button | `trigger` |
| `ParamEvent` | Fire-once event (no state) | `trigger` |
| `ParamState` | Read-only status (e.g. clip connected state) | — |

### Messages

**Client → Server:**

```
subscribe:  { "action": "subscribe", "parameter": "/parameter/by-id/<id>" }
set:        { "action": "set", "parameter": "<path>", "id": <id>, "value": <v> }
trigger:    { "action": "trigger", "parameter": "<path>", "value": true/false }
```

**Clip trigger** (connect a clip): uses `/composition/clips/by-id/<clip.id>/connect` and requires a true/false pulse:
```
{ "action": "trigger", "parameter": "/composition/clips/by-id/12345/connect", "value": true }
{ "action": "trigger", "parameter": "/composition/clips/by-id/12345/connect", "value": false }
```
The `clip.id` is the clip's top-level object ID from the initial state, not the `connected` parameter's ID.

**Server → Client:**

```
initial state:         (full composition JSON, no wrapper)
parameter_subscribed:  { "type": "parameter_subscribed", "id": ..., "valuetype": ..., "value": ..., "path": ... }
parameter_update:      { "type": "parameter_update", "id": ..., "valuetype": ..., "value": ..., "path": ... }
error:                 { "error": "...", "path": "..." }
```

### Key Quirks

- Paths are **1-indexed**: `/composition/layers/1/clips/1/connected`, not `/layers/0/clips/0/...`
- `set` requires both `parameter` (path string) and `id` (numeric) — path alone is rejected
- `ParamTrigger` parameters are read-only to `set`; use `trigger` instead
- You must `subscribe` to a parameter by ID before you receive `parameter_update` messages for it
- The subscribe response returns the canonical path, which you need for subsequent `set`/`trigger` calls
- Clip triggering uses `/composition/clips/by-id/<id>/connect`, **not** the layer/clip index path — the index path silently fails
- Triggers require a **true/false pulse** (send `value: true` then `value: false` immediately)
- The initial state JSON uses the key `connected` for the clip state param, but Resolume's canonical path for it is `.../connect` (without the "ed")

## Architecture

```
┌─────────────────────────────────────────────────┐
│  FFGL Plugin (future)                           │
│  runs on OpenGL thread                          │
│                                                 │
│  ┌──────────────┐     poll()    ┌─────────────┐ │
│  │ Plugin logic  │◄────────────│ WsClient    │ │
│  │               │─────────────►│ (bg thread) │ │
│  └──────────────┘  set/trigger  └──────┬──────┘ │
│                                        │ ws     │
└────────────────────────────────────────┼────────┘
                                         │
                              ws://127.0.0.1:8080/api/v1
                                         │
                               ┌─────────┴─────────┐
                               │  Resolume Arena    │
                               │  (or mock server)  │
                               └───────────────────┘
```

### Threading Model

IXWebSocket manages its own background thread. Communication between the WS thread and the main thread uses a `std::mutex`-guarded `std::deque`:

- **Inbound** (WS thread → main): WS callback parses JSON, pushes `IncomingMessage` into the inbox. Main thread calls `poll()` to drain it.
- **Outbound** (main → WS thread): `send_json()` calls `ix::WebSocket::send()` directly (IXWebSocket handles internal thread safety for sends).

This is simple and sufficient for the current message rates. Can be replaced with a lock-free SPSC ring buffer later if profiling shows contention on the render path.

## Code Layout

```
├── CMakeLists.txt                      # Build system (FetchContent for deps)
├── run_tests.sh                        # Run all tests (Python + C++ + integration)
├── mock_server/
│   ├── resolume_mock.py                # Mock Resolume server (aiohttp)
│   ├── test_resolume_mock.py           # pytest tests (18 tests)
│   ├── requirements.txt                # Python deps
│   ├── pytest.ini
│   └── fixtures/
│       └── resolume-ws-initial-state.json  # Captured from real Resolume (16k lines)
├── src/resolume/
│   ├── protocol.h / .cpp              # Message types, JSON serialization/parsing
│   ├── composition.h / .cpp           # Composition state model (Layer, Clip, Parameter)
│   └── ws_client.h / .cpp             # WebSocket client (IXWebSocket wrapper)
├── tests/
│   ├── test_protocol.cpp              # Protocol unit tests (9 tests)
│   ├── test_composition.cpp           # Composition parsing tests (6 tests)
│   └── test_ws_client.cpp             # Integration tests against mock server (3 tests)
└── tools/
    └── testme.cpp                     # Manual E2E tool: connect, list clips, trigger one
```

### Dependencies (fetched via CMake FetchContent)

| Library | Version | Purpose |
|---------|---------|---------|
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | v11.4.5 | WebSocket client with built-in threading |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | JSON parsing/serialization |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | C++ test framework |

TLS is disabled (`USE_TLS OFF`) since we only connect to localhost.

## C++ API

### Protocol Layer (`protocol.h`)

Typed message structs with `std::variant` for both directions:

```cpp
// Outgoing (client → server)
using OutgoingMessage = std::variant<SubscribeMessage, SetMessage, TriggerMessage>;

// Incoming (server → client)
using IncomingMessage = std::variant<CompositionState, ParameterSubscribed, ParameterUpdate, ErrorMessage>;

nlohmann::json to_json(const OutgoingMessage& msg);
IncomingMessage parse_incoming(const nlohmann::json& j);
```

Parsing dispatches on the presence of `"error"`, `"type"`, or `"layers"`/`"decks"` keys to distinguish message kinds.

### Composition Model (`composition.h`)

Extracts the fields we care about from the large initial state JSON:

```cpp
struct Parameter { int64_t id; std::string valuetype; json value; optional<double> min, max; };
struct Clip      { int64_t id; std::string name; std::string connected_state; int64_t connected_id; };
struct Layer     { int64_t id; std::string name; vector<Clip> clips; Parameter video_opacity; };
struct Composition { std::string name; vector<Layer> layers; };

Composition parse_composition(const nlohmann::json& state);
```

This is intentionally a partial view — we parse only what the looper plugin needs rather than modelling the full Resolume state.

### WebSocket Client (`ws_client.h`)

```cpp
class ResolumeWsClient {
  void connect(const std::string& url);
  void disconnect();
  bool is_connected() const;

  void subscribe(const std::string& parameter_path);
  void subscribe_by_id(int64_t id);
  void set(const std::string& path, int64_t id, const nlohmann::json& value);
  void trigger(const std::string& path, bool value = true);
  void trigger_clip(int64_t clip_id);    // true+false pulse on /clips/by-id/<id>/connect

  std::vector<IncomingMessage> poll();   // drain inbox from main thread
  void set_on_message(std::function<void(const IncomingMessage&)> callback);
};
```

## Mock Server

Python + aiohttp. Loads the captured initial state fixture, builds ID→parameter and ID→path lookup caches at startup.

Key behaviors:
- **Connect**: sends full initial state JSON
- **subscribe**: looks up parameter by ID, responds with `parameter_subscribed` including the canonical path
- **set**: updates the value in memory, broadcasts `parameter_update` to all clients subscribed to that parameter
- **trigger**: for `ParamState` parameters (like clip `connected`), changes value to `"Connected"` and broadcasts

The mock server can run standalone (`python resolume_mock.py`) for manual testing, or is started/stopped automatically by `run_tests.sh` for integration tests.

## Testing

`./run_tests.sh` runs everything:

1. **Mock server tests** (pytest, 18 tests) — `ResolumeState` unit tests, WebSocket protocol tests, HTTP endpoint tests
2. **C++ build** — incremental cmake build
3. **C++ unit tests** (15 tests) — protocol serialization/parsing, composition model parsing against the real fixture
4. **C++ integration tests** (3 tests) — launches mock server, connects with `ResolumeWsClient`, verifies initial state receipt, subscribe flow, and set+update flow

Integration tests auto-skip when the mock server isn't available (e.g. running `ctest` directly without the server).

## Future Work

- **FFGL plugin integration**: wrap `ResolumeWsClient` in the FFGL plugin, poll from the render callback
- **Lock-free queues**: replace mutex-guarded deque with SPSC ring buffer if render-thread contention appears
- **Reconnection**: IXWebSocket supports auto-reconnect; enable and handle re-subscription on reconnect
- **Broader composition model**: parse effects, transport, dashboard parameters as needed by the plugin
- **TLS support**: enable if connecting to remote Resolume instances
