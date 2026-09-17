# IPC protocol — version 1

Transport: Windows named pipe `\\.\pipe\tsro.v1.<user-sid>` (byte mode, overlapped).
Test transport: `AF_UNIX` stream socket (POSIX, test suite only).
Encoding: UTF-8 NDJSON — one JSON object per line, `\n` terminated.

The **plugin is the server**; the **overlay add-on is the client**. Any number of clients may be
connected; each gets its own sequence stream and its own snapshot on connect.

## 1. Limits (enforced by both sides, not advisory)

| Limit | Value | Enforced where |
|---|---|---|
| Max bytes per message | 65536 | `LineFramer` — while accumulating, so an unterminated flood is cut off early |
| Max JSON nesting depth | 16 | `json::parse` |
| Max array elements | 4096 | `json::parse` |
| Max object members | 256 | `json::parse` |
| Max string length | 8192 bytes | `json::parse` |
| Max chat text length | 1024 chars (after clamp) | plugin, before serialising |
| Max users per snapshot | 512 | plugin |
| Server→client queue depth | 256 messages | `IpcServer`, oldest dropped + resync |

Exceeding a limit is a protocol error: the offending message is dropped, a counter is
incremented, and — for framing and depth violations, which indicate a malformed or hostile peer
rather than one bad event — the connection is closed. A peer is never trusted to be well-behaved.

## 2. Envelope

Every message, in both directions:

```json
{
  "v":      1,
  "seq":    42,
  "ts":     1737072000123,
  "type":   "user_joined",
  "server": "T0aXnBgSj9PA0z2r9bDGfSAmqOU=",
  "data":   { }
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `v` | int | yes | Protocol version. Client and server must agree; see §5. |
| `seq` | int ≥ 0 | yes | Monotonic per connection, per direction. Starts at 0 with `hello`. |
| `ts` | int | yes | Unix milliseconds, sender's clock. Display only — ordering uses `seq`. |
| `type` | string | yes | One of §3 / §4. Unknown types are ignored, never dispatched. |
| `server` | string | no | `virtualserver_unique_identifier` the message concerns. Absent for connection-independent messages. |
| `data` | object | no | Type-specific payload. Absent is equivalent to `{}`. |

Unknown **fields** are ignored so a newer sender can add them without breaking an older receiver.
Unknown **types** are counted and dropped. Neither is an error.

## 3. Server → client messages

### `hello`
First message on every connection. `seq` is 0.
```json
{ "protocol_min": 1, "protocol_max": 1, "plugin_version": "1.0.0",
  "ts_client_version": "3.6.2", "plugin_api_version": 26,
  "capabilities": ["chat","whisper_incoming","commander","priority_speaker",
                   "recording","away","talk_power"] }
```
`capabilities` lists what this build can actually supply. A client must not render an indicator
whose capability is absent — this is the mechanism by which unsupported states are omitted
gracefully rather than rendered as permanently-false.

### `state_snapshot`
Complete authoritative state. Sent after `hello`, after any resync request, and whenever the
server had to drop queued events. Replaces all client state for that server.
```json
{ "connection": "connected",
  "server": { "handler_id": 1, "unique_id": "T0aX…", "name": "Example TS" },
  "channel": { "id": 42, "name": "Racing #1", "parent_id": 7,
               "parent_name": "Games", "path": "Games/Racing #1", "topic": "" },
  "self_unique_id": "kZ9…",
  "users": [ UserState, … ] }
```

`UserState`:
```json
{ "client_id": 17, "unique_id": "kZ9…", "nickname": "Alice",
  "display_name": "Alice", "talking": false, "whispering_to_me": false,
  "input_muted": false, "output_muted": false, "input_hardware": true,
  "output_hardware": true, "input_deactivated": false,
  "away": false, "away_message": "", "recording": false,
  "channel_commander": true, "priority_speaker": false,
  "is_talker": true, "talk_power": 75, "has_avatar": true,
  "locally_muted": false, "is_self": false, "country": "GB",
  "is_friend": true, "is_blocked": false, "friend_nickname": "Chief" }
```
**Fields the SDK could not supply for this client are omitted entirely**, not defaulted. A
receiver must treat absent as "unknown — hide the indicator", not as `false`.

### `connection_changed`
```json
{ "connection": "disconnected", "reason": "connection_lost", "error": 1796 }
```
`connection` ∈ `disconnected | connecting | connected`.
`reason` ∈ `user | timeout | kicked | banned | server_shutdown | connection_lost | unknown`.

`connecting` and the intermediate TeamSpeak establishing states are collapsed into a single
`connecting`, and the plugin suppresses repeats, so reconnect storms do not produce a
notification per internal transition.

### `channel_changed`
Local user moved. Sent for the local client only; other clients' moves are `user_joined`/`user_left`.
```json
{ "from": { "id": 7, "name": "Lobby", "path": "Lobby" },
  "to":   { "id": 42, "name": "Racing #1", "parent_name": "Games",
            "path": "Games/Racing #1", "topic": "" },
  "user_count": 4 }
```
`from` is absent on the first join after connecting.

### `user_joined` / `user_left`
Concerns the local user's current channel only.
```json
{ "user": UserState, "cause": "moved" }
```
`cause` ∈ `moved | connected | disconnected | timeout | kicked | banned`. `user_left` additionally
carries `"to_channel_id"` when the user moved elsewhere rather than leaving the server.

### `user_updated`
One or more tracked properties changed. `data.user` carries the **full** `UserState`, and
`data.changed` lists the changed field names so a client can animate only what moved.
```json
{ "user": UserState, "changed": ["away","talk_power"] }
```

### `speaking_changed`
Separate from `user_updated` because it is by far the highest-frequency event and clients
special-case it.
```json
{ "client_id": 17, "unique_id": "kZ9…", "talking": true, "whisper": false }
```
`whisper: true` means **this user is whispering to me** — see §6.1 for what that does and does
not mean.

### `mute_changed`
```json
{ "client_id": 17, "unique_id": "kZ9…",
  "input_muted": true, "output_muted": false,
  "input_hardware": true, "output_hardware": true, "input_deactivated": false }
```
`input_muted` and `output_muted` are independent and must not be collapsed: `output_muted` is
derived from `CLIENT_OUTPUTONLY_MUTED`, so speaker-mute does not masquerade as mic-mute.

### `commander_changed`
```json
{ "client_id": 17, "unique_id": "kZ9…", "channel_commander": true }
```

### `whisper_changed`
```json
{ "client_id": 17, "unique_id": "kZ9…", "direction": "incoming", "active": true }
```
`direction` is always `"incoming"` in v1. See §6.1.

### `chat_message`
Only sent for categories the client enabled via `configuration_updated`. A disabled category is
filtered **in the plugin** and never reaches the pipe.
```json
{ "id": 91, "category": "channel", "sender_unique_id": "kZ9…",
  "sender_name": "Alice", "channel_name": "Racing #1",
  "text": "gg", "timestamp_ms": 1737072000123, "outgoing": false }
```
`category` ∈ `channel | server | private`. `text` is clamped to 1024 characters and stripped of
control characters other than none — no escape sequence, BBCode or URL in it is ever interpreted
by the renderer; it is drawn as literal text.

### `heartbeat`
Every 2000 ms, regardless of activity.
```json
{ "uptime_ms": 918273, "connected_clients": 1 }
```

### `error`
```json
{ "code": "unsupported_version", "message": "client requested v2; server supports 1..1",
  "fatal": true }
```
`code` ∈ `unsupported_version | malformed | too_large | rate_limited | internal`.
`fatal: true` means the server closes the connection after sending this.

## 4. Client → server messages

### `client_hello`
Must be the client's first message. `seq` 0.
```json
{ "protocol": 1, "client": "reshade-addon", "client_version": "1.0.0",
  "process": "game.exe", "pid": 12345 }
```

### `configuration_updated`
Declares what the client wants delivered. Sent after `client_hello` and whenever settings change.
Chat categories default to **all false** — the server sends no chat at all until asked, so
private messages cannot leak through a client that simply never sent this message.
```json
{ "chat": { "channel": true, "server": false, "private": false },
  "max_chat_length": 1024, "want_speaking_events": true }
```

### `request_snapshot`
Asks for a fresh `state_snapshot`. Sent on sequence gap, on stale detection, and after resume.
```json
{ "reason": "sequence_gap" }
```
`reason` ∈ `sequence_gap | stale | manual | resumed`.

### `ping`
```json
{ "nonce": 7 }
```
Server replies `heartbeat` with the same `nonce` echoed in `data.nonce`. Used for the round-trip
latency figure in the Diagnostics tab.

## 5. Version negotiation

1. Server sends `hello` with `protocol_min` / `protocol_max`.
2. Client sends `client_hello` with the single `protocol` version it will speak.
3. If that is outside the server's range, the server replies `error` / `unsupported_version` /
   `fatal: true` and closes. It does **not** attempt to guess a compatible dialect.
4. A client receiving a `hello` whose range excludes its own version closes the connection itself
   and reports the mismatch in Diagnostics, rather than sending messages that will be rejected.

Additive changes (new optional fields, new message types, new capabilities) do not bump `v`;
receivers ignore what they do not know. `v` is bumped only for a change that would make an old
receiver misinterpret an existing field.

## 6. Capability truthfulness

This section exists because the brief forbids inventing SDK capabilities. Each row states what
TeamSpeak's Plugin API 26 actually exposes.

| Model field | Source | Status |
|---|---|---|
| `talking` | `ts3plugin_onTalkStatusChangeEvent` | Supported, event-driven |
| `whispering_to_me` | `isReceivedWhisper` parameter of the same callback | Supported, **incoming only** |
| outgoing whisper | — | **Not exposed.** No callback reports that the local user is whispering, nor to whom. Omitted from the model; the Whispering settings section shows only the incoming direction and says why. |
| whisper target list | — | **Not exposed** for other clients. Not modelled. |
| `input_muted` | `CLIENT_INPUT_MUTED` | Supported |
| `output_muted` | `CLIENT_OUTPUTONLY_MUTED` | Supported, independent of mic mute |
| `input_hardware` / `output_hardware` | `CLIENT_INPUT_HARDWARE` / `CLIENT_OUTPUT_HARDWARE` | Supported |
| `input_deactivated` | `CLIENT_INPUT_DEACTIVATED` | **Local client only** — the SDK documents it as "available only for own client". Omitted for others. |
| `away` / `away_message` | `CLIENT_AWAY` / `CLIENT_AWAY_MESSAGE` | Supported |
| `recording` | `CLIENT_IS_RECORDING` | Supported |
| `channel_commander` | `CLIENT_IS_CHANNEL_COMMANDER` | Supported, real-time via `ts3plugin_onUpdateClientEvent` |
| `priority_speaker` | `CLIENT_IS_PRIORITY_SPEAKER` | Supported |
| `is_talker` / `talk_power` | `CLIENT_IS_TALKER` / `CLIENT_TALK_POWER` | Supported. "Suppressed" is derived: `!is_talker` in a moderated channel. |
| `output_muted` | `CLIENT_OUTPUT_MUTED` **or** `CLIENT_OUTPUTONLY_MUTED` | Supported. TeamSpeak sets only one of the two: the speaker button sets `CLIENT_OUTPUT_MUTED` (which the SDK documents as implying microphone mute), while `CLIENT_OUTPUTONLY_MUTED` is the rarer speakers-off-mic-live case. Reading either alone misses the other, so both are read and ORed. |
| `locally_muted` | `CLIENT_IS_MUTED` | Supported for clients other than self |
| `has_avatar` | `CLIENT_FLAG_AVATAR` | Flag only. |
| avatar **image** | — | **Not obtainable through the plugin API.** The flag says an avatar exists; retrieving the bitmap is not part of the plugin API surface. The overlay shows an initial-letter badge instead, which is stated in the UI. |
| `country` | `CLIENT_COUNTRY` | Supported |
| `is_friend` / `is_blocked` / `friend_nickname` | — | **Not in the plugin API at all.** The whole SDK has no contact, friend or buddy call; the word appears only in a comment noting the client has already filtered a message. The list lives in the client's `settings.db`, keyed by the same unique identity used everywhere else, so the plugin reads it from there — read-only, never locking, never writing — and re-reads it on connect and when a channel roster is rebuilt, rate-limited to once every ten seconds. Absent when the file cannot be read, which the overlay treats as *unknown* rather than *not a friend*. |
| channel name / parent / topic | `CHANNEL_NAME`, `getParentChannelOfChannel`, `CHANNEL_TOPIC` | Supported |
| server name / uid | `VIRTUALSERVER_NAME`, `VIRTUALSERVER_UNIQUE_IDENTIFIER` | Supported |
| chat messages | `ts3plugin_onTextMessageEvent` | Supported for channel, server and private targets |
| per-user audio level / waveform | — | **Not exposed as an analysable stream** by the plugin API in a form we are willing to use. `ts3plugin_onEditPlaybackVoiceDataEvent` does deliver PCM, but tapping the audio path to drive a decoration would add per-sample work to the voice thread. Deliberately not implemented; the speaking animation is timer-driven. |

### 6.1 What `whisper: true` means

It means TeamSpeak told us this user's transmission reached us as a whisper rather than through
the channel. It does **not** mean we know the whisper's target list, that we can see whispers
between other people, or that we have any access to whisper content beyond the audio the client
already plays. The overlay renders it as a distinct indicator from ordinary channel speaking,
and that is the whole of the feature.
