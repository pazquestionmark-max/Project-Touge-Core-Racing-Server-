# Architecture

**Document status:** normative. Every claim about a third-party API in this document was
verified against that project's source, at the versions recorded in
[`compatibility.md`](compatibility.md). Statements about capabilities that do *not* exist are
called out explicitly rather than glossed over.

---

## 1. The constraint that determines the whole design

The project brief asks for a live TeamSpeak overlay "in ReShade". ReShade offers two
completely different extension mechanisms, and only one of them can do this.

### 1.1 ReShade effects (`.fx` shaders) — cannot implement this feature

A ReShade effect is HLSL compiled to a pixel/compute shader and executed on the GPU inside the
game's present chain. A shader:

* has no access to process memory outside the resources ReShade binds for it;
* cannot open a file, a socket, a named pipe, or any other OS handle;
* cannot allocate or lay out dynamic text — it has no string type and no font system;
* receives only the uniforms ReShade itself defines (timers, frame counters, key states,
  random values) plus the back buffer and depth buffer.

There is no supported path by which a `.fx` file can learn a TeamSpeak nickname. Any design that
claims otherwise is wrong. Approaches such as encoding IPC data into a texture would require a
host component to write that texture — at which point the host component is doing the real work
and the shader is a pointless intermediate step that also cannot lay out text.

**Conclusion: the overlay is not a shader, and this repository ships no `.fx` file for it.**

### 1.2 ReShade add-ons — the mechanism actually used

ReShade 5.0 introduced an add-on API: a native DLL (`.addon` / `.addon64`) loaded into the game
process by ReShade, which registers C++ callbacks against ReShade's rendering events and is
handed ReShade's own Dear ImGui context through an exported function table. An add-on is ordinary
native code: it can create threads, open named pipes, and draw arbitrary geometry and text.

Two registration mechanisms exist, and this project uses both, for different jobs. Both were
verified in ReShade's source, not assumed:

| Mechanism | When ReShade invokes it | Used here for |
|---|---|---|
| `reshade::register_event<reshade::addon_event::reshade_overlay>(cb)` | **Every frame**, between `ImGui::NewFrame()` and `ImGui::EndFrame()` | The always-on HUD |
| `reshade::register_overlay("title", cb)` | Only while the ReShade menu is open | The settings UI |

The always-on property is not an assumption. `runtime_gui.cpp` contains an early-out that skips
all ImGui work when nothing needs drawing, and that early-out explicitly excludes the case where
an add-on has subscribed to `reshade_overlay`:

```cpp
if (!show_splash_window && !show_message_window && !show_statistics_window && !_show_overlay
    && _preview_texture == std::numeric_limits<size_t>::max()
#if RESHADE_ADDON
    && !has_addon_event<addon_event::reshade_overlay>()
#endif
    )
{
    ...
    return; // Early-out to avoid costly ImGui calls when no GUI elements are on the screen
}
```

Because we subscribe, ReShade builds an ImGui frame every frame and calls us inside it. We draw
into `ImGui::GetBackgroundDrawList()`, which emits into the same draw data ReShade already
submits — so we add **no** extra draw call batches, render passes, render targets, or resource
creation of our own. Conversely, `register_overlay` callbacks are gated on `!_show_overlay`
being false, so the settings window appears only when the user opens ReShade's menu. That is
exactly the behaviour we want for each.

Consequences that follow from this and are honoured throughout the implementation:

* **Input is never captured by the HUD.** The HUD only appends vertices to a draw list; it opens
  no ImGui window, so it cannot take keyboard or mouse focus. ReShade blocks game input only
  while its own menu is open, which is the moment the user is deliberately configuring.
* **We do not own any GPU resource.** Device loss, resolution change and swapchain recreation are
  ReShade's problem, and it already solves them. Our HUD is stateless per frame apart from
  animation timers, which are wall-clock based and therefore survive a device reset intact.
* **Font family is not freely selectable.** ReShade owns and rebuilds the ImGui font atlas
  (`build_font_atlas()`); an add-on that pushed its own atlas would fight ReShade for it and
  break on every rebuild. We therefore render with ReShade's atlas and use
  `ImDrawList::AddText(font, size, ...)`, which takes an arbitrary pixel size. **Font *size* is
  fully configurable; font *family* follows ReShade's own font setting.** This is a real
  limitation and is documented as such in [`compatibility.md`](compatibility.md) rather than
  papered over.

### 1.3 Why not an external transparent window?

A separate always-on-top layered window is the classic alternative. It was rejected because it
does not work in exclusive fullscreen, it is captured by screen recorders differently, it
introduces a second process to install and crash, and it needs its own input-passthrough
handling. The ReShade add-on renders inside the game's own present chain and therefore behaves
identically in exclusive fullscreen, borderless and windowed modes. The companion process in
this repository is a *tool*, not a renderer — see §4.

---

## 2. Component topology

```
  ┌──────────────────────────────┐                  ┌─────────────────────────────────┐
  │ TeamSpeak 3 client process   │                  │ Game process                    │
  │                              │                  │                                 │
  │  ┌────────────────────────┐  │                  │  ┌───────────────────────────┐  │
  │  │ tsro_ts3plugin.dll     │  │                  │  │ ReShade (dxgi.dll/…)      │  │
  │  │  Component B           │  │                  │  │                           │  │
  │  │                        │  │                  │  │  ┌─────────────────────┐  │  │
  │  │  ts3plugin_on*Event    │  │                  │  │  │ TeamSpeakOverlay    │  │  │
  │  │        ↓               │  │   named pipe     │  │  │  .addon64           │  │  │
  │  │  event normaliser      │  │  ────────────►   │  │  │  Component A        │  │  │
  │  │        ↓               │  │  NDJSON, v1      │  │  │                     │  │  │
  │  │  TsState (authority)   │  │  ◄────────────   │  │  │  IPC client thread  │  │  │
  │  │        ↓               │  │  client_hello,   │  │  │        ↓ (queue)    │  │  │
  │  │  IpcServer (N clients) │  │  config, ping    │  │  │  StateStore         │  │  │
  │  └────────────────────────┘  │                  │  │  │        ↓            │  │  │
  └──────────────────────────────┘                  │  │  │  Renderer (ImGui)   │  │  │
                                                    │  │  └─────────────────────┘  │  │
  ┌──────────────────────────────┐                  │  └───────────────────────────┘  │
  │ tsro CLI (Node/TypeScript)   │                  └─────────────────────────────────┘
  │  config validate/migrate/    │
  │  profiles, protocol mock     │
  └──────────────────────────────┘
```

The plugin is the **pipe server** and the overlay is the **pipe client**. This is deliberate and
not arbitrary: there is exactly one TeamSpeak client but an unbounded, churning set of game
processes. A server in the long-lived process and clients in the transient ones means game
launches and crashes require no coordination, and several games (or a game plus the CLI
diagnostic tool) can observe the same state simultaneously.

**The rendering layer never talks to TeamSpeak.** It has no TeamSpeak headers, no knowledge of
`anyID`, and no ability to send anything to the server. It consumes a normalised model.

---

## 3. Layering

Responsibilities are separated as the brief requires, with the module that owns each:

| # | Responsibility | Module | Runs in |
|---|---|---|---|
| 1 | TeamSpeak event collection | `teamspeak-plugin/src/ts_events.cpp` | TS client |
| 2 | TeamSpeak state management | `teamspeak-plugin/src/ts_state.cpp` | TS client |
| 3 | IPC communication | `shared/src/transport_*.cpp`, `ipc_server.cpp`, `ipc_client.cpp` | both |
| 4 | Overlay state management | `shared/src/state_store.cpp` | game |
| 5 | UI layout and rendering | `reshade-integration/src/renderer.cpp`, `shared/src/layout.cpp` | game |
| 6 | Configuration management | `shared/src/config.cpp`, `profile_store.cpp` | game + CLI |
| 7 | User-facing configuration UI | `reshade-integration/src/settings_ui.cpp` | game |
| 8 | Logging / diagnostics | `shared/src/log.cpp`, `shared/src/diagnostics.cpp` | both |

Everything in `shared/` is platform-independent C++17 with no dependency on TeamSpeak, ReShade,
ImGui or Win32. That is what makes layers 2–6 unit-testable on any platform, and it is why the
test suite in this repository genuinely runs in CI on Linux rather than being aspirational.

The dependency graph is acyclic and points inward:

```
teamspeak-plugin ─┐                 ┌─ reshade-integration
                  ├──► shared ◄─────┤
tools/tsro-cli ───┘   (no deps on   └─ tests
                       anything)
```

---

## 4. Technology selection, with justification

| Technology | Where | Why this and not something else |
|---|---|---|
| **C++17** | `shared/`, plugin, add-on | Forced, not chosen. The TeamSpeak plugin ABI is a C DLL with C-linkage exports; the ReShade add-on ABI is a native DLL exchanging an ImGui function table. Neither can be produced by a managed or interpreted runtime. C++17 specifically because ReShade's `reshade.hpp` requires ≥C++17, and it gives us `string_view`, `optional` and `variant` without pulling in a dependency. |
| **No third-party C++ libraries** | `shared/` | Every byte of the add-on is loaded into someone else's game process and every byte of the plugin into their voice client. A vendored JSON library would be ~25k lines of someone else's code in that address space to parse a message format we define ourselves. The hand-written parser in `shared/src/json.cpp` is ~600 lines, is bounded by construction (depth, length, element count), and is fuzz-testable. Dependencies here are a liability, not a convenience. |
| **Dear ImGui (via ReShade's table)** | add-on | Not a dependency we choose to add — it is the drawing surface ReShade hands us. We link no ImGui implementation; we compile ImGui's *headers* for the type definitions and call through `imgui_function_table_instance()`, which ReShade populates. See §7.3 for the version pinning this forces. |
| **TypeScript / Node 20+** | `tools/tsro-cli` | The user-facing configuration tooling is text processing over JSON — schema validation, migration between config versions, profile management, import/export. That is TypeScript's home ground, it is the stated language preference, and it keeps this work *out* of the game process entirely. It also lets the protocol mock (§9.2) be written quickly enough to be worth having. |
| **NDJSON over named pipes** | IPC | See §6.1. |
| **CMake ≥3.20** | build | The only build system all three C++ targets, MSVC, and GCC/Clang-on-Linux-for-tests agree on. |

Languages deliberately *not* introduced: C#/.NET (would add a runtime dependency for a config
editor that ImGui already provides in-game, where the user actually is), Rust (no ABI advantage
here, and a second toolchain for contributors), Python (not shippable to end users).

---

## 5. The normalised data model

Defined in `shared/include/tsro/model.hpp`. This is the contract between the two components. The
plugin converts TeamSpeak's vocabulary into it; the renderer knows only this.

```
ServerState   { handler_id, unique_id, name, connection: ConnectionState }
ChannelState  { id, name, parent_id, parent_name, path, topic }
UserState     { client_id, unique_id, nickname, display_name,
                talking, whispering_to_me, input_muted, output_muted,
                input_hardware, output_hardware, input_deactivated,
                away, away_message, recording, channel_commander,
                priority_speaker, is_talker, talk_power, has_avatar,
                locally_muted, is_self, country }
ChatMessage   { id, category: Channel|Server|Private, sender_unique_id,
                sender_name, channel_name, text, timestamp_ms }
OverlayState  { server, channel, users[], self_unique_id, last_event_ms }
```

Three deliberate modelling decisions:

1. **`unique_id` is the identity key, not `nickname` and not `client_id`.** TeamSpeak's
   `CLIENT_UNIQUE_IDENTIFIER` is stable for the lifetime of a user's identity; `client_id`
   (`anyID`) is recycled within a session and meaningless across reconnects; nicknames change and
   collide. Per-user styling is therefore keyed on `unique_id`, and per-channel styling on
   `virtualserver_unique_identifier + channel_id` — a channel id alone is not unique across
   servers. `config.cpp` enforces this key shape.
2. **Mic mute and speaker mute are separate fields and are never conflated.** TeamSpeak makes
   speaker-mute imply mic-mute in `CLIENT_OUTPUT_MUTED`, which is why we read
   `CLIENT_OUTPUTONLY_MUTED` for the true speaker state and keep `CLIENT_INPUT_MUTED`
   independent. A user with only their speakers muted must not render as mic-muted.
3. **Absent is distinct from false.** Where the SDK cannot supply a field for a given client, the
   plugin omits the key rather than sending a default. The renderer then hides that indicator
   instead of rendering a confident wrong answer. This is how "unsupported states are omitted
   gracefully" is actually achieved.

---

## 6. IPC

### 6.1 Transport choice

| Candidate | Verdict |
|---|---|
| **Windows named pipe** | **Chosen.** Kernel object with a real security descriptor, message-mode framing, overlapped I/O, multi-instance, no port allocation, no firewall prompt, cannot be reached from another machine once `PIPE_REJECT_REMOTE_CLIENTS` is set. |
| Loopback TCP | Rejected. Any local process of any user can connect; needs a port and thus collision handling; triggers firewall dialogs; visible to `netstat` for anyone looking. |
| Shared memory ring | Rejected. No connection lifecycle, no readiness signalling without a second primitive, and a crashed writer leaves a corrupt buffer with no clean way to detect it. |
| File polling | Rejected. Latency and disk churn, both unacceptable for a per-frame consumer. |

Pipe name: `\\.\pipe\tsro.v1.<user-sid>`. Including the SID keeps two users on the same machine
(fast user switching, shared PC) from colliding.

The POSIX `AF_UNIX` implementation in `transport_posix.cpp` exists **for the test suite**, so the
same `IpcServer`/`IpcClient` logic that ships on Windows is exercised end-to-end in CI. It is not
a shipping configuration.

### 6.2 Wire format

Newline-delimited JSON. One UTF-8 object per line, `\n` terminated, 64 KiB hard cap per message.

Chosen over a binary format because the entire message volume is a few hundred bytes per second
— the format is nowhere near being a bottleneck — while being able to `type` the pipe traffic
during a support conversation is worth a great deal. Framing is handled by `LineFramer`, which
enforces the size cap *while accumulating*, so an adversarial peer cannot grow our buffer by
never sending a newline.

Envelope:

```json
{ "v": 1, "seq": 42, "ts": 1737072000123, "type": "user_joined",
  "server": "xyz=", "data": { ... } }
```

Full message catalogue, field types, and validation rules: [`protocol.md`](protocol.md).

### 6.3 Threading contract

This is the part that determines whether the product is usable, so it is stated as a hard rule.

**In the TeamSpeak client:** `ts3plugin_on*Event` callbacks run on TeamSpeak's own thread. They
do the minimum — read the client variables they need, update `TsState`, serialise one message —
and push it into a bounded ring buffer. A dedicated `IpcServer` thread owns every pipe handle and
does all blocking. **No TeamSpeak callback ever performs a blocking write, waits on a mutex held
by an I/O thread, or touches the network.** If the ring is full (an overlay that stopped reading),
the oldest event is dropped and a `state_snapshot` is queued instead, so the overlay converges
rather than the plugin stalling.

**In the game:** a detached `IpcClient` thread owns the pipe handle and performs all reads. It
parses and decodes on that thread and publishes finished frames into a triple-buffered slot. The
render callback does exactly one acquire-load to pick up the newest published state. **The render
thread never blocks, never allocates in steady state, never parses JSON, and never touches a pipe
handle.** Reconnection, backoff and timeout all live on the IPC thread. If TeamSpeak is closed,
the render thread's worst case is reading the same pointer it read last frame.

### 6.4 State synchronisation

On every connect and reconnect the server sends `hello` then a complete `state_snapshot`, so a
client that missed events while disconnected does not need them. Beyond that:

* **Sequence numbers** are monotonic per connection. A gap means events were lost; the client
  requests a resynchronisation rather than continuing from a state it knows is wrong.
* **Duplicates** (`seq` ≤ last applied) are dropped.
* **Heartbeats** every 2 s from the server. The client marks state *stale* after
  `stale_after_ms` (default 6 s) without any message and, per the brief's explicit requirement,
  **never displays a stale user list indefinitely**: the HUD switches to a disconnected
  presentation whose appearance is configurable but whose triggering is not.
* **Reconnection** uses exponential backoff, 250 ms → 5 s, with jitter, unbounded in attempts but
  bounded in interval.
* **Version negotiation**: a client whose `v` the server does not support receives an `error`
  message naming the supported range and the connection closes cleanly, instead of both sides
  misparsing each other.

### 6.5 Security model

Stated as assumptions and limits, not as a claim of invulnerability.

* **Authorisation is the pipe ACL.** The pipe is created with an explicit security descriptor
  granting `GENERIC_READ|GENERIC_WRITE` to the creating user's SID and `SYSTEM` only, with no
  `Everyone` ACE and no NULL DACL. `PIPE_REJECT_REMOTE_CLIENTS` blocks remote access.
* **Trust boundary:** any process running as the same user. This is honest: on Windows, a
  same-user process can debug the TeamSpeak client outright, so a stricter IPC boundary would be
  security theatre. Cross-user and remote access are genuinely prevented.
* **Every inbound message is validated** before use: size cap, depth cap, type check per field,
  enum range check, UTF-8 validation, string length caps. Unknown fields are ignored; unknown
  message types are ignored with a counter, never dispatched.
* **The protocol has no verb that acts.** There is no "execute", no path, no command. The
  client→server surface is four messages that set booleans and integers within validated ranges.
  There is no code path from a chat message to anything but a bounded string in a draw list.
* **No network sockets are opened by either component, ever.** Chat content never leaves the
  machine. It is not written to the log unless the user sets `log.include_message_content`, which
  defaults to `false` and carries an in-UI warning.
* **Private messages are off by default** and, when off, are *filtered in the plugin* — they are
  never serialised onto the pipe at all, so they cannot be recovered from a pipe trace.

---

## 7. Rendering

### 7.1 Frame flow

```
reshade_overlay(runtime)                      [ReShade's ImGui frame, every frame]
  ├─ now = steady_clock::now()
  ├─ state = state_slot.acquire()             [one atomic load; no lock, no copy]
  ├─ if (!config_dirty && !state.changed) → reuse cached layout
  ├─ LayoutEngine::compute(state, config, viewport)   [pure; unit-tested]
  ├─ NotificationQueue::tick(now)             [advances lifecycles, evicts expired]
  └─ draw(ImGui::GetBackgroundDrawList(), layout, now)
```

Layout is recomputed only when the state or configuration actually changed; otherwise the cached
`LayoutResult` is redrawn. `LayoutEngine::compute` is in `shared/`, is pure, and is tested
directly against resolutions, aspect ratios, anchors, long names and overflowing user counts —
which is how "tested at different resolutions" is achieved without a GPU in CI.

### 7.2 Icons are vector-drawn, not textures

Every indicator (speaking ring, mic-muted, speaker-muted, away, recording, commander, priority,
whisper) is drawn with `ImDrawList` primitives. This is a considered choice:

* no texture upload, no `ImTextureID` lifetime to manage across device resets;
* scales cleanly to any `icon_size` without mipmaps or blurring;
* no image assets to ship, and therefore no possibility of shipping anyone else's artwork;
* `IconShape` is an enum with a table-driven painter, so adding a shape is one function.

The cost is that arbitrary user-supplied PNG icons are not supported in v1. That is recorded as a
limitation in [`compatibility.md`](compatibility.md); the configuration schema already reserves
`icon_image` for it so a later version can add it without a schema break.

### 7.3 Version pinning

An add-on calls into ReShade through two contracts: `ReShadeRegisterAddon(module, api_version)`
and `ReShadeGetImGuiFunctionTable(IMGUI_VERSION_NUM)`. The second is exact-match — ReShade
returns a table only for ImGui ABI versions it was built with. This repository pins **ReShade SDK
v6.4.1 (`RESHADE_API_VERSION` 16) + Dear ImGui v1.91.8 (`IMGUI_VERSION_NUM` 19180)**, because
current ReShade builds still export the 19180 table while older API versions remain accepted —
giving the widest compatible range from a single binary. `scripts/fetch-deps.sh|ps1` fetches
exactly these tags; `-DTSRO_RESHADE_TAG=` / `-DTSRO_IMGUI_TAG=` override them. The resulting
support floor is in [`compatibility.md`](compatibility.md).

### 7.4 Performance budget

Design targets, with the mechanism that delivers each. Measured figures belong in
[`performance.md`](performance.md) and are filled in from `scripts/benchmark.ps1` on real
hardware — this section states intent, not results.

| Budget | Mechanism |
|---|---|
| No added GPU work | We append to a draw list ReShade already submits; we create no resource and issue no draw call |
| No per-frame allocation | Layout buffers are reserved once and reused; all strings pre-formatted on the IPC thread |
| No render-thread blocking | Triple-buffered state slot; single relaxed atomic load per frame |
| Bounded memory | Chat history, notification count and user list are all capped by configuration with hard maxima |
| No polling | Every TeamSpeak input is a callback; the only timer is the 2 s heartbeat |

---

## 8. Configuration

Versioned JSON (`config_version`, currently 1), stored at
`%APPDATA%\TeamSpeakReShadeOverlay\`, with `profiles\<name>.json` beside `config.json`.

* **Validation is total.** Loading never throws and never fails: unknown keys are preserved for
  forward compatibility, invalid values are clamped or replaced with defaults, and every repair
  is reported in a `ConfigDiagnostics` list surfaced in the Diagnostics tab. A truncated or
  corrupt file yields defaults plus a visible warning, never a crash and never a silent reset.
* **Migration** runs `migrate_v(n)_to_v(n+1)` in sequence, so a v1 file opened by a future v4
  build upgrades stepwise. A file from a *newer* version than the binary understands is loaded
  read-only with a warning rather than being downgraded and destroyed.
* **Profiles** are complete configurations. Per-game switching matches on executable name, an
  exact-match-then-default lookup done once at add-on load.
* The same schema is implemented in TypeScript in `tools/tsro-cli`, and a schema-parity test
  asserts the two implementations agree on defaults and on migration output — so the CLI and the
  add-on cannot drift apart silently.

---

## 9. Testing strategy

### 9.1 What runs in CI on Linux

`shared/` has no platform dependencies, so the state machine, protocol, framing, config,
migration, layout, easing and notification lifecycle are all unit-tested on every push. The
POSIX transport lets `tests/test_ipc_integration.cpp` run a real server and client over a real
socket: connect, snapshot, event stream, peer kill, reconnect, malformed input, oversize frame.

### 9.2 The protocol mock

`tools/tsro-cli mock` implements the *server* side of the protocol in TypeScript and can replay a
scripted scenario (joins, leaves, talk bursts, mute toggles, commander changes, chat). This gives
two things: the add-on can be exercised in a real game without TeamSpeak running, and the
TypeScript and C++ protocol implementations are cross-validated against each other rather than
each being tested only against itself.

This is a test and development tool. It is not the product, it is not wired into the shipping
overlay, and the overlay has no code path that fabricates TeamSpeak data.

### 9.3 What CI cannot cover, and how it is covered instead

Building the add-on and plugin requires MSVC; a GitHub Actions Windows job does that on every
push, so both DLLs are proven to compile and link. Actually *rendering* in a game and running
against a live TeamSpeak client cannot be automated here and is covered by the manual matrix in
[`testing.md`](testing.md). Results from that matrix are recorded in
[`compatibility.md`](compatibility.md), which distinguishes **verified**, **expected** and
**untested** — no configuration is claimed as supported on the strength of it merely compiling.
