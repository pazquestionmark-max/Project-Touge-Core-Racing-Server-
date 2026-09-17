# Compatibility and known limitations

Each row is marked **verified** (run and observed), **expected** (follows from a documented API
and compiles, but not yet run on that configuration) or **untested**. Nothing is claimed as
supported on the strength of merely compiling.

## Build and test status

| Check | Runs where | Status |
|---|---|---|
| Core library, protocol, state, config, layout, IPC — 237 tests | Linux, CI | **verified** |
| Tooling and cross-implementation schema parity — 47 tests | Linux, CI | **verified** |
| Windows-only sources cross-compiled (MinGW) | Linux, CI | **verified** |
| Plugin and add-on built with MSVC | Windows, CI | **expected** — the CI job is defined and is the authoritative build; it has not been run by the author on a Windows machine |
| Rendering in a real game | manual | **untested** — see [`testing.md`](testing.md) for the matrix to work through |
| Against a live TeamSpeak client | manual | **untested** — same |

This is the honest position: the logic is tested thoroughly and automatically, the Windows
binaries are not yet proven on hardware. Do not treat the table below as field-tested.

## Versions

| Component | Target | Notes |
|---|---|---|
| Windows | 10 (1809+) and 11, x64 | Uses only long-standing Win32 APIs: named pipes, SDDL, overlapped I/O. |
| TeamSpeak client | 3.6.x, Plugin API **26** | The API version is a hard match: TeamSpeak refuses to load a plugin whose `ts3plugin_apiVersion` differs from the one the client implements. |
| TeamSpeak 5 / 6 | **not supported** | Those clients do not load TeamSpeak 3 native plugins. Their "remote apps" interface is a different mechanism; supporting it would be a second plugin, not a port of this one. |
| ReShade | 6.4.1 and newer, **with add-on support** | The "addon-free" ReShade download deliberately omits the add-on API and cannot load this. Compatibility floor is set by the SDK pin below. |
| Dear ImGui | **v1.91.8-docking** (`IMGUI_VERSION_NUM` 19180) | Must be the docking branch: ReShade's add-on ImGui function table declares `DockSpace`, `ImGuiDockNodeFlags` and `ImGuiWindowClass`, which exist only there. CMake checks both the version and the branch and fails with an explanation. |
| Graphics APIs | D3D9, D3D10, D3D11, D3D12, OpenGL, Vulkan | We draw through ReShade's ImGui layer, so whichever backends your ReShade build supports, the overlay supports. We add no API-specific code. |
| Display modes | Exclusive fullscreen, borderless, windowed | We render inside the game's own present chain, so all three behave identically. |
| VR | **not supported** | ReShade does not invoke `reshade_overlay` for VR effect runtimes. Documented in ReShade's own header. |

Why the SDK is pinned to 6.4.1 rather than the newest release: ReShade accepts add-ons built
against an API version at or below its own, so pinning older widens the range of ReShade builds
that can load a single binary. `scripts/fetch-deps` takes `--reshade-tag` and `--imgui-tag` to
build against a different pair, which must be changed together.

## What the TeamSpeak plugin API does and does not expose

The full table is in [`protocol.md`](protocol.md) §6. The limitations that will be noticed:

| Wanted | Status |
|---|---|
| Channel Commander in real time | **Supported** — `CLIENT_IS_CHANNEL_COMMANDER` via `onUpdateClientEvent`. |
| Microphone mute separate from speaker mute | **Supported** — read from `CLIENT_INPUT_MUTED` and `CLIENT_OUTPUTONLY_MUTED` respectively, and never conflated. |
| Incoming whisper | **Supported** — the `isReceivedWhisper` parameter of `onTalkStatusChangeEvent`. |
| **Outgoing** whisper | **Not exposed.** No callback reports that *you* are whispering, or to whom. The Whispering settings show only the incoming direction and say so. |
| Whisper targets, or whispers between other people | **Not exposed.** Not modelled. |
| Avatar **images** | **Not obtainable** through the plugin API. `CLIENT_FLAG_AVATAR` says an avatar exists; fetching the bitmap is not part of the plugin API surface. |
| Per-user audio level / waveform | **Deliberately not implemented.** `onEditPlaybackVoiceDataEvent` does deliver PCM, but tapping the voice path to drive a decoration would add per-sample work to the audio thread. The speaking animation is timer-driven instead. |
| `CLIENT_INPUT_DEACTIVATED` for other users | **Own client only**, per the SDK. Omitted for others rather than guessed. |
| `CLIENT_IS_MUTED` for yourself | **Other clients only**, per the SDK. Omitted for yourself. |

Where a state is unavailable the field is **absent**, not false, and the overlay hides that
indicator rather than rendering a confident wrong answer.

## Overlay limitations in this version

| Limitation | Why | Workaround |
|---|---|---|
| Font *family* follows ReShade's setting | ReShade owns the ImGui font atlas and rebuilds it; an add-on that pushed its own would fight it. | Font *size* is fully configurable. Change the family in ReShade's own settings. |
| No user-supplied image icons | Indicators are vector-drawn, which avoids texture lifetimes across device resets and ships no third-party artwork. | 18 built-in shapes, each freely colourable, per state and per user. The `icon_image` key is reserved so a later version can add this without a schema break. |
| One TeamSpeak connection at a time | The overlay follows the tab you are actually on. | Switching tabs in TeamSpeak switches the overlay. |
| Drag-and-drop positioning is numeric, not mouse-dragged | The HUD takes no input by design, so it cannot be dragged directly. | The Layout tab has anchors, pixel and percentage offsets, and a live preview that updates as you type. |
| Notification sounds need a `.wav` path | No bundled audio. | Point the setting at any `.wav` on disk. |

## Security assumptions

* The trust boundary is **your Windows user account**. The pipe's DACL grants only your SID and
  SYSTEM, and `PIPE_REJECT_REMOTE_CLIENTS` blocks network access. Cross-user and remote access
  are genuinely prevented.
* Any process running as *you* could connect. This is stated rather than papered over: a
  same-user process on Windows can already debug the TeamSpeak client outright, so a stricter IPC
  boundary would be theatre.
* **No network socket is opened by either component, ever.** Chat never leaves the machine.
* Private messages are off by default and, when off, are filtered inside the plugin — they are
  never serialised onto the pipe, so they cannot be recovered from a pipe trace.
* Chat content is not written to the log unless `logging.include_message_content` is switched on,
  which defaults to false and carries a warning in the UI.
