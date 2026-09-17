# Fonts

These are shipped so you have something to point ReShade at. The overlay draws with **whatever
font ReShade is configured to use** — it cannot load a typeface by itself, because ReShade owns
the Dear ImGui font atlas and rebuilds it, and reaching into that atlas from an add-on is not
safe across ReShade versions. (An earlier build tried; it crashed the game.)

## Using one

1. Copy this `fonts` folder next to the game's ReShade DLL (beside `TeamSpeakOverlay.addon64`).
2. In game, press **Home** → **Settings** tab.
3. Find ReShade's font option and point it at `fonts\Roboto-Medium.ttf`.
4. The overlay picks it up immediately — no restart, nothing to set on our side.

This changes ReShade's own UI font as well. That is inherent to the approach, not a bug.

## What is here

| File | Licence |
|---|---|
| `Roboto-Medium.ttf` | Apache License 2.0 — © Google Inc. |
| `Cousine-Regular.ttf` | Apache License 2.0 — © Steve Matteson |
| `Karla-Regular.ttf` | SIL Open Font License 1.1 — © Jonathan Pinhorn |
| `DroidSans.ttf` | Apache License 2.0 — © Google Inc. |

All four are redistributable under those terms and are distributed unmodified, as bundled with
Dear ImGui. Their licences are unaffected by this project's MIT licence.
