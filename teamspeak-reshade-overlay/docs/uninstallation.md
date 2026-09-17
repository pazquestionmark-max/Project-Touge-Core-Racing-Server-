# Uninstalling

Removal is deleting files. Nothing in this project writes to the registry, installs a service or
a driver, modifies TeamSpeak's or ReShade's own files, or leaves a background process.

## 1. Remove the ReShade add-on

Delete `TeamSpeakOverlay.addon64` from the game folder.

Leave everything else alone. ReShade's own DLL, its `.ini`, your presets and your shaders are not
ours and removing them would break ReShade for that game.

## 2. Remove the TeamSpeak plugin

1. Close TeamSpeak. (Deleting the DLL while TeamSpeak has it loaded will fail with a file-in-use
   error — that is Windows protecting you, not a fault.)
2. Delete `%APPDATA%\TS3Client\plugins\tsro_overlay_win64.dll`.
3. Delete `%APPDATA%\TS3Client\tsro-plugin.log` if present.

Do not delete the `plugins` folder itself: other TeamSpeak plugins live there.

## 3. Remove your settings (optional)

Settings live entirely in `%APPDATA%\TeamSpeakReShadeOverlay\`:

```
%APPDATA%\TeamSpeakReShadeOverlay\
├── profiles\*.json        your saved profiles
├── game-profiles.json     which profile each game uses
└── tsro-overlay.log       the add-on's log
```

Delete that one folder and every trace of your configuration is gone. Keep it if you might
reinstall — a fresh install will pick your profiles straight back up.

## What is left behind

Nothing. To confirm:

* no registry keys — the project makes no registry calls at all;
* no services or scheduled tasks;
* no files outside the three locations above;
* no network configuration, firewall rule or certificate — no socket is ever opened;
* the named pipe is a kernel object that ceases to exist the moment TeamSpeak closes.

## If TeamSpeak will not start after removing the plugin

That is not something this plugin can cause once its DLL is deleted, but if it happens: start
TeamSpeak, go to **Tools → Options → Addons → Plugins** and press **Reload All**. TeamSpeak
caches the plugin list, and a stale entry for a deleted plugin is harmless but can look alarming.
