# TeamSpeak ReShade Overlay

See who is in your TeamSpeak channel, and who is talking, without leaving the game.

A TeamSpeak 3 plugin publishes your channel and its members over a local named pipe; a ReShade
add-on draws them inside the game's own render pipeline. Inspired by TSNotifier in purpose, not
in implementation — no assets or code are taken from it.

> **Status: the logic is thoroughly tested; the Windows binaries are not yet field-tested.**
> 284 automated tests pass on every push, covering the protocol, state machine, configuration,
> layout, notifications and end-to-end IPC over a real socket, plus cross-validation between two
> independent implementations of the wire format. The Windows build is defined in CI but has not
> been run on hardware by the author, and the overlay has not yet been rendered in a real game
> against a live TeamSpeak client. [`docs/compatibility.md`](docs/compatibility.md) marks every
> configuration **verified**, **expected** or **untested**.

## What it shows

* The current channel, its parent, and how many people are in it — updating the moment you move.
* Everyone in the channel, with who is speaking.
* Microphone mute and speaker mute as **separate** states, never conflated.
* Channel Commander, by default an orange circle immediately before the name.
* Away, recording, priority speaker, locally muted, and suppressed.
* Incoming whispers, shown distinctly from ordinary channel speech.
* Join, leave, channel-change and connection notifications, with fade in and out.
* Channel and server chat, opt-in per category. **Private messages are off by default.**

Every colour, position, size, icon, animation and visibility is configurable in game, per user
and per channel, without editing a file.

## Why an add-on and not a shader

A ReShade effect (`.fx`) is a GPU shader. It cannot open a pipe, read a file, hold a string or
lay out text, so it cannot display a TeamSpeak nickname — no amount of cleverness changes that,
and this repository ships no `.fx` file pretending otherwise.

A ReShade **add-on** is native code loaded into the game process, handed ReShade's own Dear ImGui
context. It can open a pipe and draw text. The always-on HUD uses the `reshade_overlay` event,
which ReShade invokes every frame when an add-on subscribes — verified in ReShade's source, not
assumed. The settings window uses `register_overlay`, which ReShade calls only while its menu is
open. Full reasoning: [`docs/architecture.md`](docs/architecture.md).

## Install

You need **ReShade with add-on support** (not the "addon-free" download).

1. `plugin\tsro_overlay_win64.dll` → `%APPDATA%\TS3Client\plugins\`, then enable it in
   TeamSpeak → Tools → Options → Addons → Plugins.
2. `addon\TeamSpeakOverlay.addon64` → next to the game's ReShade DLL.
3. In game: Home → Add-ons → TeamSpeak Overlay.

Full instructions, including building from source:
[`docs/installation.md`](docs/installation.md).

## Layout

```
shared/               platform-independent C++17 core — no TeamSpeak, ReShade, ImGui or Win32
teamspeak-plugin/     Component B: the TeamSpeak 3 plugin (Plugin API 26)
reshade-integration/  Component A: the ReShade add-on (renderer, settings UI, icons)
tools/config-tool/    tsro-config: defaults, validation, schema probing
tools/tsro-cli/       TypeScript: configuration tooling and the mock-plugin test harness
tests/               C++ unit and integration tests
examples/profiles/   five example configurations, validated in CI
docs/                architecture, protocol, configuration, compatibility, testing, troubleshooting
```

`shared/` has no third-party dependency: every byte of it is loaded into someone else's game and
voice client. That is also what makes the state machine, protocol, configuration and layout
testable on any platform, which is why the test suite genuinely runs in CI rather than being
aspirational.

## Design commitments

These are testable claims, not aspirations. Each one is enforced by a test:

* **The renderer never blocks.** All IPC, parsing and state application happen on a background
  thread; the render thread does one atomic exchange against a triple buffer.
* **TeamSpeak's callback thread never blocks.** Events are queued, never written synchronously.
  A stalled overlay causes a bounded queue to drop and resynchronise, not the voice client to
  stutter.
* **Absent is not false.** A state the SDK cannot report is omitted, and the overlay hides that
  indicator rather than rendering a confident wrong answer.
* **A stale user list is never presented as live.** After the configured window with no update,
  the overlay changes presentation and requests a resynchronisation.
* **Chat you did not ask for is never sent.** Categories are filtered inside the plugin, so a
  disabled category never crosses the pipe and cannot be recovered from a trace.
* **Nothing reaches the network.** Neither component opens a socket.

## Development

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
scripts/check-windows-sources.sh          # cross-compile the Windows-only sources with MinGW
cd tools/tsro-cli && npm install && npm test
```

Try the overlay without TeamSpeak running:

```bash
cd tools/tsro-cli && npx tsx src/cli.ts mock --scenario chatter
```

`scripts/fetch-deps.sh` (or `.ps1`) retrieves the pinned ReShade SDK, Dear ImGui and TeamSpeak
Plugin SDK headers. None of them are redistributed here. ReShade and ImGui must be a matched
pair — including ImGui's **docking** branch — and CMake fails with an explanation if they are not.

## Documentation

| | |
|---|---|
| [architecture.md](docs/architecture.md) | Why an add-on, component topology, threading, IPC, security |
| [protocol.md](docs/protocol.md) | The wire format, and exactly what the TeamSpeak API does and does not expose |
| [configuration.md](docs/configuration.md) | Every setting, with its range and default |
| [compatibility.md](docs/compatibility.md) | Version matrix and known limitations |
| [testing.md](docs/testing.md) | What is automated, and the manual matrix that is not |
| [troubleshooting.md](docs/troubleshooting.md) | Symptom-first diagnosis |
| [installation.md](docs/installation.md) · [uninstallation.md](docs/uninstallation.md) | Setup and clean removal |

## Licence

MIT — see [LICENSE](LICENSE). Not affiliated with TeamSpeak Systems GmbH, the ReShade project, or
TSNotifier.
