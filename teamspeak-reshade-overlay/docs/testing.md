# Testing

Two categories, kept strictly apart: what runs automatically on every push, and what a person has
to sit down and do. The second list is not aspirational filler — it is the part that is genuinely
**not yet done**, and [`compatibility.md`](compatibility.md) reflects that.

## 1. Automated

284 tests, all green, on every push. `ctest --test-dir build --output-on-failure` and
`npm test` in `tools/tsro-cli`.

### C++ (237 tests)

| Suite | What it pins down |
|---|---|
| `json` | Parsing, escapes, surrogate pairs, invalid UTF-8, and every bound (depth, size, element count). The serialiser must emit valid UTF-8 even from damaged input. |
| `protocol` | Envelope round trips; absent-vs-false on the wire; malformed input classified as fatal or droppable; unknown types and versions tolerated; chat subscription defaulting to everything off. |
| `framing` | Line splitting, reassembly across reads, CRLF, and that the size cap trips **while accumulating** rather than after a flood has been buffered. |
| `state_store` | Identity keyed on `unique_id` not the recycled `client_id`; duplicate and gap handling; staleness; speaking state surviving a `user_updated`; mute fields not clobbering each other; disconnection clearing the roster. |
| `config` | Defaults round-tripping byte-for-byte; clamping; wrong types keeping defaults; unknown keys preserved; a newer version flagged but not downgraded; per-channel keys required to be server-scoped. |
| `layout` | All nine anchors; eight resolutions from 800×600 to 5120×1440; every overflow mode; UTF-8 never split; indicator resolution and ordering; sorting. |
| `notifications` | The full lifecycle, that an entry is never removed mid-animation, the hard cap, duplicate merging, and post-connect suppression. |
| `triple_buffer` | A 200 000-iteration two-thread run asserting the consumer never observes a torn value. |
| `ipc` | **End to end over a real socket**: connect, snapshot, event stream, reconnect after the server restarts, a client that starts before the server, chat withheld until subscribed, oversized frames closing only the offending connection, version rejection, multi-client isolation, 500-event bursts. |
| `plugin` | The plugin driven against a scripted TeamSpeak client with a real overlay client on the far end: joins, leaves, channel switching, speaking, incoming whisper, commander, independent mute states, away, chat gating, disconnect and reconnect. |
| `profiles` | Atomic saves, corrupt and truncated files, traversal and reserved-name rejection, import/export, per-executable mapping. |

### TypeScript (47 tests)

`protocol` and `mock-plugin` mirror the C++ suites against an independent implementation.
`schema-parity` runs the **real `tsro-config` binary** and asserts the two schema
implementations agree on version, limits, every numeric range, colour normalisation, and which
files are broken. Without it the two could drift and tell a user different things about the same
file.

### Build coverage

* Linux, GCC, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`, warnings as
  errors.
* MinGW cross-compile of every Windows-only source (`scripts/check-windows-sources.sh`). This has
  already caught real bugs: a non-copyable type passed by value, a missing include, and the ImGui
  docking-branch requirement.
* MSVC on `windows-latest`, which is authoritative and caught three more that neither GCC nor
  MinGW could: `C4065`, `C4996`, and a `/MD` vs `/MT` CRT mismatch.

## 2. Manual — not yet done

Nothing below has been performed. Anyone who runs a row should record the result here and update
[`compatibility.md`](compatibility.md).

### 2.1 TeamSpeak integration

Needs a real client and a second person (or a second identity) for anything involving another
user.

- [ ] Plugin appears in Tools → Options → Addons → Plugins and enables cleanly
- [ ] Enabling the plugin while already sitting in a channel picks up the existing state
- [ ] Connect to a server; overlay shows the channel and its members
- [ ] Switch channels; **the title updates immediately**, and membership follows
- [ ] Another user joins, then leaves; notifications appear and the list tracks
- [ ] Speaking starts and stops; indicator follows in real time
- [ ] Another user mutes their microphone → mic indicator only
- [ ] Another user mutes their speakers → speaker indicator only, **visibly different**
- [ ] Channel Commander toggled on another client
- [ ] Away, recording, priority speaker
- [ ] Someone whispers you; shown distinctly from channel speech
- [ ] Channel message appears; server message appears
- [ ] Private message does **not** appear until explicitly enabled
- [ ] Disconnect from the server: user list empties rather than freezing
- [ ] Reconnect: state returns without a burst of join notifications
- [ ] Kill TeamSpeak outright: overlay reports the loss, then reconnects when it restarts
- [ ] Switch TeamSpeak tabs between two servers; the overlay follows the active one

### 2.2 Rendering

- [ ] 1920×1080, 2560×1440, 3840×2160, and an ultrawide
- [ ] Exclusive fullscreen, borderless, windowed
- [ ] Alt-tab out and back
- [ ] Resize a windowed game while the overlay is visible
- [ ] Change resolution while running
- [ ] D3D11, D3D12, Vulkan, OpenGL — at least one title each
- [ ] A 20+ person channel, with the cap and "+N more" behaving
- [ ] A very long nickname in each overflow mode
- [ ] Several people talking at once, starting and stopping rapidly
- [ ] Five notifications at once
- [ ] Change settings while in game and confirm the HUD follows without a restart
- [ ] Confirm the HUD never takes keyboard or mouse focus

### 2.3 Failure handling

- [ ] Kill TeamSpeak mid-session
- [ ] Kill the game mid-session; TeamSpeak stays healthy
- [ ] Corrupt a profile by hand → defaults plus a visible warning, never a crash
- [ ] Hand-edit a profile to a newer `config_version` → loads with a warning
- [ ] Delete the configuration directory while running
- [ ] Run TeamSpeak elevated and the game not → the overlay reports it cannot connect
- [ ] Two games running at once, both connected
- [ ] Trigger a device reset (alt-tab in exclusive fullscreen, driver reset)

### 2.4 Performance

Measure with the overlay disabled, then enabled, on the same scene.

- [ ] Frame time delta, idle channel
- [ ] Frame time delta, eight people with four talking
- [ ] CPU of the TeamSpeak process, before and after
- [ ] Memory of both processes over an hour
- [ ] Event-to-display latency (speak into a mic, count frames)
- [ ] IPC round trip (the Diagnostics tab reports this directly)
- [ ] Sustained behaviour under `tsro mock --scenario stress`

No performance budget is asserted until these have been measured on real hardware. The design
targets are in [`architecture.md`](architecture.md) §7.4; a target is not a measurement.

## 3. Testing without TeamSpeak

```bash
cd tools/tsro-cli && npx tsx src/cli.ts mock --scenario chatter
```

Scenarios: `idle`, `chatter`, `churn`, `channels`, `chat`, `stress`. The mock speaks the real
protocol, so everything except the TeamSpeak-side event collection is exercised. It is a test
harness: it produces no real voice data and is not part of the shipped overlay.
