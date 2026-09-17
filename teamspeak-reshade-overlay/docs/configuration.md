# Configuration

Everything is configurable in game: **Home** → Add-ons → TeamSpeak Overlay. Nothing here requires
editing a file, and the overlay never requires a restart to pick up a change.

Files live in `%APPDATA%\TeamSpeakReShadeOverlay\`:

```
profiles\<name>.json     one complete configuration each
game-profiles.json       which profile each executable uses
tsro-overlay.log         the add-on's log
```

## How loading works

Loading never fails and never throws. A value that is out of range is clamped, one of the wrong
type falls back to its default, and **every repair is listed in the Diagnostics tab** rather than
applied silently. A file that cannot be parsed at all yields defaults plus a visible error — your
file is not overwritten until you save.

Keys this build does not recognise are **preserved**, so a configuration written by a newer
version survives a round trip through an older one.

Saving writes a temporary file and renames it, so a crash or a full disk leaves the previous
configuration intact rather than a truncated one.

Profiles may be **partial**: list only what differs from the defaults, as the shipped
`minimal.json` and `competitive.json` do.

Check a file without launching a game:

```
tsro validate myprofile.json     # every problem, with its dotted path
tsro diff a.json b.json          # which settings actually differ
tsro migrate old.json            # upgrade to the current version
```

## Common types

**Colour** — `#RGB`, `#RGBA`, `#RRGGBB` or `#RRGGBBAA`, with or without the `#`. Stored
canonically as `#RRGGBBAA`. The UI has a picker and a hexadecimal field.

**Placement** — every positionable element has one:

| Field | Meaning |
|---|---|
| `visible` | Draw this element at all |
| `anchor` | Which corner or edge the offsets are measured from, so the element keeps that relationship at any resolution |
| `x`, `y` | Offset in pixels, or a fraction of the viewport when `percent` is true |
| `percent` | Treat `x`/`y` as 0–1 fractions |
| `align` | `left`, `center`, `right` |

Anchors: `top_left`, `top_center`, `top_right`, `center_left`, `center`, `center_right`,
`bottom_left`, `bottom_center`, `bottom_right`.

**Fade** — `in_ms`, `hold_ms`, `out_ms`, `start_opacity`, `end_opacity`, `easing`.
Easings: `linear`, `ease_in`, `ease_out`, `ease_in_out`, `ease_out_back`, `ease_out_elastic`.

**Icons** — `none`, `dot`, `circle`, `ring`, `square`, `diamond`, `triangle`, `star`, `chevron`,
`microphone`, `microphone_muted`, `speaker`, `speaker_muted`, `moon`, `record`, `crown`,
`whisper`, `bars`. All vector-drawn, so any size stays sharp. Image icons are not supported in
this version — see [compatibility](compatibility.md).

## `general`

| Key | Range | Default | |
|---|---|---|---|
| `enabled` | bool | `true` | Master switch |
| `master_opacity` | 0–1 | `0.92` | Multiplies everything |
| `scale` | 0.25–4 | `1.0` | Multiplies every size |
| `show_when_disconnected` | bool | `true` | Show while not on a server |
| `show_when_plugin_unavailable` | bool | `true` | Show while TeamSpeak is closed |
| `auto_profile_by_executable` | bool | `true` | Pick a profile per game |

## `appearance`

| Key | Range | Default |
|---|---|---|
| `font_size` | 6–96 | `16` |
| `icon_size` | 2–96 | `12` |
| `row_height` | 6–160 | `22` |
| `row_spacing` | 0–64 | `2` |
| `padding_x` / `padding_y` | 0–128 | `10` / `8` |
| `corner_radius` | 0–32 | `4` |
| `panel_background` | colour | `#10161AAA` |
| `panel_border` | colour | `#FFFFFF1E` |
| `show_panel_background` | bool | `true` |
| `text_shadow` | bool | `true` |
| `text_shadow_offset` | 0–8 | `1` |
| `text_default` / `text_secondary` / `accent` | colour | — |

**Leave `text_shadow` on.** It is the difference between readable and not over bright game
content. Font *family* follows ReShade's own setting; only the size is ours to change.

## `channel_title`

`placement`, plus: `show_parent`, `show_user_count`, `show_server_name`, `show_topic`,
`font_scale` (0.2–6), `opacity` (0–1), `text`/`background`/`border` colours,
`show_background`, `show_border`, `icon`, `icon_color`.

Format strings take `{channel}`, `{parent}`, `{server}`, `{count}`. `format` is used when there is
no parent channel, `parent_format` when there is. `disconnected_text` is shown when not connected.
An unrecognised placeholder is drawn literally, so a typo is visible rather than swallowed.

## `user_list`

| Key | Range | Default | |
|---|---|---|---|
| `show_local_user` | bool | `true` | Include yourself |
| `highlight_local_user` | bool | `true` | Colour yourself differently |
| `show_muted_users` | bool | `true` | You are always shown regardless |
| `speaking_first` | bool | `false` | Talkers to the top |
| `sort` | `channel_order`, `alphabetical`, `talk_power`, `speaking_first` | `alphabetical` | |
| `name_overflow` | `clip`, `ellipsis`, `wrap`, `shrink`, `scroll` | `ellipsis` | How a long name is handled |
| `max_name_width` | 20–2000 | `180` | |
| `min_font_scale` | 0.2–1 | `0.75` | Floor for `shrink` |
| `max_visible_users` | 1–512 | `24` | |
| `show_overflow_count` | bool | `true` | The "+N more" line |
| `indicator_gap` | 0–64 | `6` | |

## `indicators`

Eleven independent states: `speaking`, `whispering`, `mic_muted`, `speaker_muted`,
`mic_hardware_off`, `away`, `recording`, `commander`, `priority_speaker`, `suppressed`,
`locally_muted`.

Each has the same shape: `enabled`, `show_icon`, `icon`, `icon_scale` (0.1–8), `icon_color`,
`override_text_color`, `text_color`, `show_background`, `background`, `show_border`, `border`,
`border_thickness` (0–12), `glow`, `glow_color`, `glow_radius` (0–64), `opacity` (0–1),
`dim_entry`, `dim_amount` (0–1).

Two worth reading twice:

* **`commander`** defaults to an orange circle drawn immediately before the name. Both the colour
  and the shape are configurable here, and per user on the Users tab.
* **`mic_muted` and `speaker_muted` are separate states.** They ship with different icons *and*
  different colours deliberately. Making them look alike removes the distinction entirely.

A state TeamSpeak cannot report for a given user is hidden, not drawn as "off".

## `notifications`

`placement`, `max_visible` (1–20), `stack` (`down`/`up`), `spacing`, `width` (80–2000),
`min_height`, `merge_duplicates`, `suppress_after_connect_ms` (0–60000).

`suppress_after_connect_ms` is what stops a burst of join notifications when you connect to a
channel that already has people in it.

Six independent categories: `join`, `leave`, `channel_switch`, `connection`, `whisper`, `chat`.
Each has `enabled`, `format`, `prefix`, `icon`, `icon_color`, `text`, `name_color`,
`background`, `border`, `show_background`, `show_border`, `fade`, `sound`, `sound_file`.

Placeholders by category:

| Category | Available |
|---|---|
| join / leave | `{name}` `{channel}` `{count}` |
| channel_switch | `{channel}` `{previous}` `{count}` |
| connection | `{status}` `{server}` |
| whisper | `{name}` |
| chat | `{name}` `{message}` `{channel}` |

## `chat`

| Key | Range | Default | |
|---|---|---|---|
| `show_channel_messages` | bool | `true` | |
| `show_server_messages` | bool | `false` | |
| `show_private_messages` | bool | **`false`** | See below |
| `order` | `newest_bottom`, `newest_top` | `newest_bottom` | |
| `max_visible_messages` | 1–50 | `6` | Cannot exceed `history_size` |
| `history_size` | 1–500 | `50` | |
| `max_message_length` | 1–1024 | `200` | |
| `retention_seconds` | 0–86400 | `300` | 0 = keep until evicted |
| `show_timestamp` / `show_sender` / `show_channel_name` / `show_category_icon` | bool | | |
| `wrap` | bool | `true` | |
| `width` | 80–4000 | `420` | |
| `font_scale` | 0.2–6 | `0.95` | |
| `use_sender_color` | bool | `true` | Use each sender's per-user colour |

**Private messages are off by default, and this is enforced at the source.** A category you have
not enabled is filtered inside the TeamSpeak plugin and never crosses the connection, so it
cannot be recovered from a pipe trace. Turning it on affects new messages only.

Message text is drawn as literal text. No markup, escape sequence or URL in a message is ever
interpreted.

## `animation`

`enabled`, `speaking` (`none`, `color_fade`, `pulse`, `glow`, `border_sweep`),
`speaking_attack_ms` / `speaking_release_ms` (0–5000), `speaking_pulse_hz` (0.1–20),
`speaking_pulse_depth` (0–1), `state_easing`, `state_transition_ms`, `animate_list_reorder`,
`list_reorder_ms`, `overlay_fade`, `fade_when_idle`, `idle_after_ms` (1000–3600000),
`idle_opacity`.

The attack/release ramp is why a one-word reply still registers visibly instead of flickering.

## `integration`

| Key | Range | Default | |
|---|---|---|---|
| `pipe_name` | string | *(empty)* | Empty uses the default, which includes your account identifier so two users on one machine do not collide. Only change this if support asks. |
| `auto_connect` | bool | `true` | |
| `reconnect_initial_ms` | 50–60000 | `250` | |
| `reconnect_max_ms` | 100–300000 | `5000` | Raised to `reconnect_initial_ms` if set lower |
| `stale_after_ms` | 1000–120000 | `6000` | Silence after which the list stops being shown as live |
| `ping_interval_ms` | 1000–600000 | `10000` | Drives the latency figure |

## `logging`

`level` (`trace`, `debug`, `info`, `warn`, `error`, `off`), `to_file`, `file_name`,
`max_file_kb` (16–102400), `include_message_content`, `show_diagnostics_overlay`.

**`include_message_content` defaults to `false`.** Switching it on writes the contents of chat
messages to a file on disk. Leave it off unless you are diagnosing a specific problem, and turn
it off again afterwards.

## `user_overrides`

Keyed on the TeamSpeak identity (`CLIENT_UNIQUE_IDENTIFIER`), which survives nickname changes and
reconnects — unlike a nickname, which can change and collide.

```json
"user_overrides": {
  "kZ9abcDEF/ghi=": {
    "enabled": true,
    "name_color": "#00FFFF",
    "speaking_color": "#7EE787",
    "muted_color": "#F06868",
    "commander_color": "#FF952B",
    "icon": "star",
    "icon_color": "#FFD666",
    "display_override": "Chief",
    "note": "your own reminder of who this is"
  }
}
```

Every field is optional; omitted ones fall through to the global settings. The Users tab adds an
entry for anyone currently in your channel with one click, so you never have to find an identity
string by hand.

## `channel_overrides`

Keyed `"<server_unique_id>:<channel_id>"`. **The server part is required** — a bare channel id is
not unique across servers, and an entry without it is rejected with a warning rather than silently
applied to the wrong channel.

```json
"channel_overrides": {
  "T0aXnBgSj9PA0z2r9bDGfSAmqOU=:42": {
    "enabled": true,
    "title_color": "#FF00FF",
    "background": "#10141AC0",
    "border": "#FFFFFF33",
    "user_list_color": "#E6E9EE",
    "icon": "diamond",
    "icon_color": "#58A6FF",
    "font_scale": 1.4,
    "opacity": 0.95,
    "display_override": "Home"
  }
}
```

## Profiles

A profile is a complete configuration. The Profiles tab saves, loads, duplicates, renames,
deletes, imports and exports them.

**Per-game:** select a profile and press *Use the selected profile here*. That executable loads it
from then on; anything unmapped falls back to `default`. The `default` profile cannot be deleted,
because that fallback needs somewhere to land.

Profile names become file names, so separators, traversal and reserved Windows device names are
rejected.
