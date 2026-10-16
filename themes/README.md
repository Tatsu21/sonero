# Themes

A theme is a JSON file with thirteen colours, a font, and an optional block of Qt
style sheet. Drop one in `~/.config/Sonero/themes/` — or use **Profiles →
Import…**, which copies it there — and pick it from the list on that page.

The two files here are examples. `nord.json` is a straight recolour;
`daylight.json` is a light theme, and the one worth reading if you are writing
your own, because it shows the one place a light theme needs the escape hatch.

## What the thirteen colours are

| key | what it paints |
|---|---|
| `bg` | the window behind everything |
| `sidebar` | the top bar and the tray volume window |
| `surface` | cards and channel strips |
| `input` | fields, buttons, combo boxes, meter tracks |
| `hover` | what the pointer lightens |
| `border` | card and control outlines |
| `text` | body text |
| `textDim` | captions, hints, secondary text |
| `accent` | selection, focus, the equalizer curve |
| `accentHi` | the accent when hovered, and links |
| `danger` | mute, errors, a clipping meter |
| `warn` | solo, warnings, a meter approaching full scale |
| `ok` | connected, healthy, a meter in its comfortable range |

Everything else — the shade of a hover against its surface, the colour of a
slider groove, the tint behind a soloed channel — is derived from those thirteen.
That is deliberate: thirteen decisions produce a coherent interface, and
thirty-six produce a negotiation.

Anything missing from a theme file falls back to the built-in theme, so a file
naming three colours is valid and changes three things.

## The `qss` block

Colours cannot express a corner radius, a spacing, or "this one widget should be
different". For that, `qss` holds a Qt style sheet appended **after** the built-in
one, so it wins where the two overlap.

It is an escape hatch, not the supported surface: a sheet can break its own
layout, and nothing validates it. **Profiles → Export stylesheet…** writes out the
entire built-in sheet with your theme's colours already resolved — that is the
reference for what there is to override, and the object names to target.

The built-in theme is compiled into the binary and is never replaced, only
applied over. However wrong a theme file is, selecting the built-in one in
Profiles always gets you back.

## Importing a bare `.qss`

**Import…** also accepts a plain `.qss` file. It becomes a theme that keeps the
built-in colours and layers your sheet on top — which is what you want when you
only mean to restyle one thing.
