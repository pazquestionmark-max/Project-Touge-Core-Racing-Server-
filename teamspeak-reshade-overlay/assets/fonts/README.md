# Fonts

The overlay rasterises these itself. Picking one here changes the overlay only — ReShade's own
interface keeps whatever font it was set to.

## Using one

Drop `.ttf`, `.otf` or `.ttc` files into either folder the settings window prints under
**Typography & font engine**:

* `%APPDATA%\TeamSpeakReShadeOverlay\fonts` — created for you on first run. Files here win.
* `<game folder>\fonts` — beside `TeamSpeakOverlay.addon64`, which is where this folder goes if
  you copy it out of the release.

Then pick it from the **Font** list. **Rescan fonts folder** picks up anything added since,
without a restart.

## Weight

The **Weight** control emboldens as the glyphs are rasterised, so a face that ships a single
weight still gives a convincing Bold. The overlay ships Roboto Medium and defaults to it at
weight 700, which is the look most people are after; drop a real `Roboto-Bold.ttf` in the folder
above and select it if you would rather have the designed bold.

## What is here

| File | Licence |
|---|---|
| `Roboto-Medium.ttf` | Apache 2.0 |
| `Cousine-Regular.ttf` | Apache 2.0 |
| `Karla-Regular.ttf` | SIL Open Font Licence 1.1 |
| `DroidSans.ttf` | Apache 2.0 |

Licence texts travel with the fonts upstream; none of them require attribution in the UI.
