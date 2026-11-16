# Apollo Control themes: complete design languages, not palettes

Aaron (Nov 15 2026): "different colors arent different themes... a different theme is a complete redesign so faders
look different, knobs look different, buttons, interactions" and "think about it more than just what i thought of,
deeper". This document is the brief every theme is built from.

## 1. What a theme is

A theme is a complete design language built on one **metaphor**: a real or imagined instrument the app pretends to be.
The metaphor decides every layer below. Two themes that differ only in layers 6-7 are one theme in two colours.

| Layer | What the theme decides |
|---|---|
| 1. Metaphor | What the app *is* in this theme (a console, a HUD, an analyzer, a terminal, a tablet app, a game) |
| 2. Composition | How screens are arranged: channel columns, rows, panels, cards, focus mode; density; grouping of the two units |
| 3. Controls | Knob, fader, button/toggle, selector, numeric field, meter, plug-in slot: how each **looks** and how it **responds** (drag direction, fine mode, detents, reset gesture, wheel, typing, long-press) |
| 4. Feedback | How running / waiting / refused / bypassed / busy / clip / disconnected are shown, and how errors are announced |
| 5. Motion | Timing and physics: inertia, easing, stepped animation, transitions between screens, meter ballistics display |
| 6. Typography & numbers | Faces, sizes, number formatting, units, how values are read |
| 7. Colour & material | Palette, surfaces, light, texture (the only layer the old "looks" changed) |
| 8. Visualization | The theme's native way to show sound and load: VU needles, LED ladders, segmented bars, scopes, sparklines, health bars |
| 9. Plug-in experience | How the browser is presented, what dragging feels like, what a loaded slot looks like, how the parameter view is laid out |
| 10. Signature feature | One capability that only this theme has, so choosing a theme is choosing a way of working, not a colour |

## 2. Rules every theme follows

- **One model, many views.** Every control is defined once (parameter id, range, unit, step, reset value, readback
  state, refusal). A theme provides a *view* and an *interaction adapter* for it. Sends, readback, refusals and tests
  live at the model layer and do not change per theme.
- **Nothing is lost.** Every function is reachable in every theme; the keyboard map is identical everywhere
  (Ctrl+Shift+T cycles themes, Ctrl+K opens the command palette in every theme, not only Mono).
- **State is never colour alone.** Running, waiting, refused, bypassed, clip and disconnected each have a shape, glyph or
  word in every theme, so they survive colour blindness and the Mono theme.
- **Switching is live and lossless.** Changing theme keeps selection, scroll, open drawers, drag state and meter peaks.
- **Budgets.** 60 fps meters with 10+ strips and an open parameter view on Aaron's PC; renderers reuse canvases/SVG
  and never rebuild the DOM per frame. `prefers-reduced-motion` turns off shake, glitch and inertia in every theme.
- **Readable and reachable.** 125% zoom, focus always visible, hit targets at least 24 px (Day: 44 px), the screen
  reader labels are the model's and do not change per theme.
- **Theme packs.** Each theme is a folder: `manifest.json` (name, metaphor, defaults), `theme.css`, `renderers.js`
  (view + interaction per control type), optional fonts and sprites. New themes can be added without touching app.js.
- **Style-guide page.** A demo-mode page shows every control in every state for the current theme. It is how themes
  are reviewed and how the screenshots are taken.
- **UA art stays local.** Plug-in thumbnails come from Aaron's own UAD install at runtime; no theme ships UA art.

## 3. The themes

### Classic: the familiar console (default for new users, Nov 16 2026)
- **Why:** people coming from the hardware maker's own console app should find their way at once (Aaron: "the normal theme that works just like the original", "same layout literally"). The other themes stay for everyone who picks them.
- **Composition (`strips`, its own pack hooks: `themes/classic/renderers.js` ClassicDesk):** a View column on the left (OVERVIEW / INPUTS / INSERTS / SENDS, SHOW AUX / CTRL ROOM / PLUG-INS, CLEAR CLIPS, CLEAR SOLO with restore, SETTINGS); input strips top to bottom: number, MIC/LINE, 48V, PAD, LO CUT, polarity, gain, the Unison slot, inserts with REC/MON, AUX 1/2 and CUE 1/2 sends, pan, SOLO, MUTE, fader with its meter, label; the aux returns after the inputs; each unit's monitor section (meters, MUTE ALL, MUTE, DIM, the large monitor knob); a control-room column on the far right; the plug-in browser opens from an empty insert instead of being docked; the rate bar along the bottom.
- **Controls:** flat knobs with a value arc, flat faders with a dB scale, rectangular lit buttons, solid green/yellow/red meters; condensed small-caps labels (Bahnschrift).
- **Artwork:** all drawn here (CSS/SVG). No logo, product name, colour scheme, icon or bitmap of the original app.
- **Signature:** none on purpose: it is the plain one.

### Studio: the console (default until Nov 16)
- **Metaphor:** a large-format analog console with an outboard rack.
- **Composition:** full-height channel strips; a **meter bridge** across the top; scribble strips with channel names;
  each unit is a console section with its own master section; the plug-in browser is the outboard rack beside it.
- **Controls:** knurled metal knobs with a pointer line and printed scale (vertical drag, Shift = fine, a soft detent
  at unity and centre pan, double-click = reset); long-throw faders with a ribbed cap and printed dB legend (slight
  inertia, click on the scale jumps there); latching backlit push buttons vs momentary rubber buttons (talkback);
  rotary selector switches for sources.
- **Feedback:** lamps; a red FAULT lamp plus scribble-strip text for refusals; a dim "waiting" lamp.
- **Motion:** physical: faders glide, needles move with true VU ballistics (300 ms).
- **Visualization:** VU needle meter bridge (switchable VU / PPM / K-14), LED ladders on channels.
- **Plug-ins:** each plug-in is an outboard **faceplate** (its own art scaled into a 1U/2U face); drag the faceplate onto
  a channel's insert patch point; the parameter view is the faceplate opened with its controls in this theme's style.
- **Signature:** the meter bridge with selectable metering standard and a gain-staging view (every stage's level at
  a glance: preamp, insert, monitor mix, main out).

### Cyberpunk: the HUD
- **Metaphor:** a netrunner's augmented-reality HUD over the audio system.
- **Composition:** channels as stacked HUD cards with angled headers; panels slide in from the edges; the two units
  are two "nodes" with a link line between them (the combined device).
- **Controls:** segmented arc gauges with a numeric core (circular drag around the dial, wheel = fine, tick marks at
  key values); thin blade sliders with a value tag that rides the blade; angular cut-corner buttons that scan on;
  hex-cell selectors.
- **Feedback:** alert banners with a short glitch on refusal (off with reduced motion), each with a code and a reason.
- **Motion:** snappy, counters roll to new values, scanline sweep on screen change.
- **Visualization:** neon segmented meters with ghost peaks; per-channel spectrum strip; DSP load as heat on the node.
- **Plug-ins:** a **mod loadout**: each plug-in is a module card with stat bars (DSP code, pool use, latency, params),
  sockets light up while dragging with a targeting reticle; installed mods show on the channel as chips.
- **Signature:** the **live signal-flow overlay**: animated packets travel input -> preamp -> insert -> monitor mix ->
  outputs, density and speed following level, so you can see where sound is and where it stops.

### Lab: the analyzer
- **Metaphor:** an audio analyzer / oscilloscope bench (test-and-measurement gear).
- **Composition:** a grid of instrument panels; each channel is a measurement panel with numbers first and graphics
  second; panels can be docked, resized, stacked.
- **Controls:** encoders shown as numeric fields with a small arc indicator (click to type, Up/Down with selectable step
  0.1 / 1 / 10, wheel); precision sliders with major/minor graticule and numeric entry; bezel soft-keys along panel
  edges; selectors as drop lists with values.
- **Feedback:** a status line with codes plus an event log panel (every refusal and state change with timestamp).
- **Motion:** none beyond what is needed; instant.
- **Visualization:** numeric RMS, peak, crest factor per channel; bar graphs with peak and RMS markers; DSP pool table.
- **Plug-ins:** a **catalog table** with sortable columns (category, code words, pools, latency, params, licence); drag a
  row onto a slot; the parameter view is a table (name, value, range, unit, readback state) with inline editing.
- **Signature:** the **measurement panel**: level history, peak/RMS logging and an Avium Measure hand-off (open the
  current routing in Avium Measure for a sweep or a correction check).

### Mono: the terminal
- **Metaphor:** a data terminal / spreadsheet; text and numbers, nothing decorative.
- **Composition:** dense rows: each input is a **row** (not a column) with its settings as cells; units are sections.
- **Controls:** horizontal value bars (drag anywhere on the bar), `[ON]` / `[OFF]` text toggles, inline-editable numbers;
  every control has a typed form.
- **Feedback:** inline text in the row plus a log tail at the bottom.
- **Motion:** none.
- **Visualization:** thin line meters with numbers and a sparkline of the last minute.
- **Plug-ins:** a text list with columns; insert by dragging a row or by typing.
- **Signature:** the **command line**: `in 3 gain 32`, `in 1-2 link`, `insert 1176 on 2`, `mute all`, `rate 96k`,
  `theme cyberpunk`; history, completion, and a script file of commands that can be run as a preset.

### Day: the guide
- **Metaphor:** a calm, friendly tablet app for daylight sessions.
- **Composition:** fewer things at once: an overview of all inputs, tap one to open a **focus view** with everything
  about that input; advanced settings behind "More".
- **Controls:** large round knobs with a value arc, pill sliders, toggle switches, segmented choices; 44 px targets;
  plain words beside every value.
- **Feedback:** plain-language messages that say what to do next ("Turn the Twin X off and on to use plug-ins").
- **Motion:** gentle easing, nothing sudden.
- **Visualization:** level bars with "too quiet / good / too loud" zones.
- **Plug-ins:** browse by **purpose** ("Clearer vocals", "Control peaks", "Add space", "Warmth", "Guitar tone"), mapped
  from categories; large previews; one-tap "add to this input".
- **Signature:** **guided gain**: "speak or play normally for 5 seconds" -> it measures the level and suggests (or sets)
  the preamp gain for good headroom, per input.

### Arcade: the cabinet (the one deliberately retro theme)
- **Metaphor:** an 80s arcade cabinet; the two units are PLAYER 1 and PLAYER 2.
- **Composition:** each input is a character card; the master section is the score panel.
- **Controls:** pixel-art knobs with an 8-direction pointer (stepped values), chunky block fader caps, buttons that
  press down 2 px, level-select menus for sources.
- **Feedback:** banner text in the game's voice but always clear ("NO ROOM ON DSP 1", "POWER CYCLE TO CONTINUE").
- **Motion:** stepped sprite animation (12 fps), a small screen shake on clip (off with reduced motion), a coin-blip
  animation when a plug-in drops into a slot (visual only, no sound).
- **Visualization:** health-bar meters, 7-segment readouts, DSP room as an energy bar.
- **Plug-ins:** a **power-up inventory** of pixel-framed icons (a tile grid is right here, and only here).
- **Signature:** **HI-SCORE**: per-input peak since reset shown as a high score, with the time it happened, so clipping
  moments are easy to find.

## 4. Build order

1. Model/view split for every control type and the theme-pack loader; the style-guide page.
2. Layers 3-9 for all six themes on the existing composition (controls, feedback, motion, visualization, plug-ins).
3. Composition per theme (rows for Mono, panels for Lab, focus view for Day, cards for Cyberpunk and Arcade).
4. The six signature features.

Each step ships on its own; after step 2 every theme already looks and behaves like itself.
