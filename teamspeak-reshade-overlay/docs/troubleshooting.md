# Troubleshooting

Start with the **Diagnostics** tab: ReShade menu → Add-ons → TeamSpeak Overlay → Diagnostics. It
states the connection status, the last message received, counters, and any configuration repair.
Most of what follows is confirming what that tab already told you.

Logs:
* add-on: `%APPDATA%\TeamSpeakReShadeOverlay\tsro-overlay.log`
* plugin: `%APPDATA%\TS3Client\tsro-plugin.log`

Neither contains chat message text unless you switched on *Include chat message text in the log*.

---

## Nothing appears in game

**Is ReShade itself working?** Press Home. If ReShade's own menu does not open, the problem is
ReShade, not this overlay.

**Is the add-on loaded?** ReShade menu → Add-ons. If *TeamSpeak Overlay* is not in the list:

* You may have the **addon-free** ReShade build, which cannot load add-ons at all. Re-run
  ReShade's installer and choose the full version.
* The `.addon64` must be in the same folder as ReShade's DLL (`dxgi.dll` etc.), not in a
  subfolder.
* 32-bit games need `TeamSpeakOverlay.addon` (no `64`).
* ReShade's log, next to its DLL, records why an add-on was rejected. A version mismatch appears
  there explicitly.

**Is it loaded but drawing nothing?** Check General → *Overlay enabled*, and that *Overall
opacity* is not 0. If Diagnostics says *connected* but you are not in a TeamSpeak channel, there
is nothing to draw — that is correct behaviour.

## Diagnostics says "waiting to retry"

The add-on cannot reach the plugin. In order of likelihood:

1. **TeamSpeak is not running.** Start it.
2. **The plugin is not enabled.** TeamSpeak → Tools → Options → Addons → Plugins. It must be
   present *and* ticked.
3. **TeamSpeak is running as administrator and the game is not** (or the reverse). The pipe is
   restricted to one user account, and an elevated process has a different token. Run both the
   same way.
4. **They are running as different Windows users.** Same cause; this is deliberate isolation.

## The plugin is not listed

* The DLL must be directly in `%APPDATA%\TS3Client\plugins\`, not in a subfolder.
* Use the DLL matching your client: `tsro_overlay_win64.dll` for 64-bit TeamSpeak.
* **Plugin API mismatch.** TeamSpeak refuses to load a plugin built for a different API version
  and says so in its client log. This build targets **API 26** (TeamSpeak 3.6.x). TeamSpeak 5
  and 6 do not load TeamSpeak 3 plugins at all.
* Press **Reload All** on the Plugins page — TeamSpeak caches that list.

## The overlay shows people who left, or misses people who joined

Diagnostics → *Stale: yes* means no update has arrived recently; the overlay stops presenting the
list as live and asks the plugin to resend. If it stays stale, the plugin has stopped responding:
restart TeamSpeak.

*Sequence gaps* above zero means messages were lost; each gap triggers a resynchronisation
automatically. A steadily climbing count suggests the overlay is not reading fast enough — check
whether the game is running at a very low frame rate.

## Speaking indicators do not light up

* Confirm TeamSpeak itself shows the person speaking. If it does not, this is a TeamSpeak problem.
* Indicators → Speaking must be enabled.
* Someone speaking in a *different* channel is not shown; the overlay tracks your own channel.
* *Talking while disabled* — their microphone is live but their transmission is not being sent —
  is deliberately not shown as speaking, because they cannot be heard.

## Channel Commander is not shown

Indicators → Channel Commander must be enabled. Check Diagnostics → *What this plugin can report*
lists `commander`. If it does not, the plugin is older than the add-on.

## Mic mute and speaker mute look the same

They are separate states with separate settings, but you can configure them identically by
accident. Indicators → Microphone muted and Speakers muted — give them different icons and
colours. The defaults already differ in both.

## Private messages are not showing

Intended: they are off by default. Chat → *Private messages*. Note the warning — they will be
visible to anyone watching your screen or your stream. When the setting is off the plugin does
not send them at all, so switching it on takes effect for *new* messages only.

## My settings vanished

The overlay never silently resets a configuration. Check Diagnostics → Configuration: if the file
could not be read, it says so there.

* Your settings are in `%APPDATA%\TeamSpeakReShadeOverlay\profiles\`.
* A save writes a temporary file and renames it, so a crash mid-save leaves the *previous*
  settings intact rather than a truncated file.
* If a profile was hand-edited into invalid JSON, the overlay falls back to defaults and reports
  it rather than starting with a broken configuration. Run
  `tsro validate <file>` to see exactly what is wrong.
* A profile written by a newer version loads with a warning; unknown settings are preserved, but
  saving rewrites the file in this version's format.

## Performance

The overlay adds no render pass and creates no GPU resource: it appends to a draw list ReShade
already submits. If frame time changed noticeably after installing it, check Diagnostics →
Rendering for the measured layout and draw times before assuming the overlay is the cause. To
reduce its work: lower *Maximum users shown*, turn off glow, and set Animation → Speaking to
*color_fade* or *none*.

## Testing without TeamSpeak

`tsro mock --scenario chatter` runs a mock plugin that speaks the real protocol, so you can check
rendering, layout and animation with TeamSpeak closed. Available scenarios: `idle`, `chatter`,
`churn`, `channels`, `chat`, `stress`. It is a test harness — it produces no real voice data and
is not part of the shipped overlay.

## Reporting a problem

Include: Windows version, TeamSpeak version, ReShade version, the game and its graphics API, both
log files, and a screenshot of the Diagnostics tab. Check first that the log contains nothing you
would not want to share.
