# Installation

Two pieces go in two different places: a plugin inside TeamSpeak, and an add-on beside the game's
ReShade. They find each other over a local pipe; there is nothing to configure to connect them.

**Before you start:** you need ReShade **with add-on support**. The "addon-free" download on
reshade.me deliberately omits the add-on API and cannot load this overlay. If you already have
ReShade, re-run its installer and pick the full version.

## 1. Install the TeamSpeak plugin

1. Close TeamSpeak.
2. Copy `plugin\tsro_overlay_win64.dll` into
   `%APPDATA%\TS3Client\plugins\`
   (paste that path into Explorer's address bar; create `plugins` if it is missing).
3. Start TeamSpeak.
4. **Tools → Options → Addons → Plugins**, find *TeamSpeak ReShade Overlay*, and make sure it is
   enabled. If it is not listed at all, see [troubleshooting](troubleshooting.md#the-plugin-is-not-listed).

## 2. Install the ReShade add-on

1. Find the game folder containing ReShade's DLL (`dxgi.dll`, `d3d11.dll`, `opengl32.dll` or
   similar) — the same folder as the game's executable.
2. Copy `addon\TeamSpeakOverlay.addon64` into that folder, next to the ReShade DLL.
3. Start the game.

There is no third step: the add-on connects on its own.

## 3. Check it is working

In game, press **Home** (ReShade's default key) to open ReShade, then the **Add-ons** tab, then
**TeamSpeak Overlay**. The **Diagnostics** tab tells you the truth about the connection:

* *Status: connected* — working. Join a channel and the overlay appears.
* *Status: waiting to retry* — TeamSpeak is not running, or the plugin is not enabled. The line
  underneath says which.

On the **General** tab, tick **Show the overlay using example data** to see every indicator at
once without waiting for anyone to speak. Untick it when you are done; it is a preview, not a
mode.

## 4. Pick a profile

Copy any file from `profiles\` into
`%APPDATA%\TeamSpeakReShadeOverlay\profiles\` and load it from the **Profiles** tab.

| Profile | For |
|---|---|
| `default` | A sensible middle ground. |
| `minimal` | Names only, top-left, nothing else. |
| `competitive` | Least distraction: no panel, no notifications, fades out when idle. |
| `full` | Everything on, including the chat feed. |
| `streaming` | Larger and higher-contrast, with everything that could leak a private conversation switched off. |

**Per-game profiles:** on the Profiles tab, select a profile and press *Use the selected profile
here*. That game will load it from then on.

## Building from source

You do not need to build anything to use the overlay. If you want to:

```
scripts\fetch-deps.ps1
cmake -S . -B build -A x64 -DTSRO_BUILD_PLUGIN=ON -DTSRO_BUILD_ADDON=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload, CMake 3.20+, and Git.
`fetch-deps` retrieves the pinned ReShade SDK, Dear ImGui and TeamSpeak Plugin SDK headers; none
of them are redistributed in this repository. The ReShade and ImGui versions are a matched pair
and CMake refuses to build if they are mismatched — see
[compatibility](compatibility.md).

Outputs:

* `build\teamspeak-plugin\Release\tsro_overlay_win64.dll`
* `build\reshade-integration\Release\TeamSpeakOverlay.addon64`

## Uninstalling

See [uninstallation.md](uninstallation.md). Nothing here writes to the registry, installs a
service, or modifies TeamSpeak's or ReShade's own files.
