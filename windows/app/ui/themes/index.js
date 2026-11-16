'use strict';
/* Theme packs (docs/THEMES.md 2). Made by tools/themes/make_index.py from themes/<id>/manifest.json: do not edit by hand. */
window.ApolloThemes = { manifests: [
  {
    "id": "classic",
    "name": "Classic",
    "order": 0,
    "mode": "dark",
    "metaphor": "No metaphor: a conventional digital mixer app, laid out the way console software usually is.",
    "desc": "A standard mixer layout: channel strips, flat knobs and faders, lit buttons, plain meters.",
    "layout": {
      "browser": "list",
      "params": "grid",
      "composition": "strips"
    }
  },
  {
    "id": "studio",
    "name": "Studio",
    "order": 1,
    "mode": "dark",
    "metaphor": "A large-format analog console with an outboard rack.",
    "desc": "Machined knobs and long-throw faders, backlit switches, LED ladders, a rack of plug-in faceplates.",
    "layout": {
      "browser": "rack",
      "params": "faceplate",
      "composition": "console"
    }
  },
  {
    "id": "cyber",
    "name": "Cyberpunk",
    "order": 2,
    "mode": "dark",
    "metaphor": "A netrunner's augmented-reality HUD over the audio system.",
    "desc": "Arc gauges with a numeric core, blade sliders, cut-corner pills, neon meters with ghost peaks, plug-ins as mod cards.",
    "layout": {
      "browser": "cards",
      "params": "module",
      "composition": "hud"
    }
  },
  {
    "id": "lab",
    "name": "Lab",
    "order": 3,
    "mode": "dark",
    "metaphor": "An audio analyzer and oscilloscope bench.",
    "desc": "Numeric encoders, precision sliders with graticules, soft-keys with lamps, peak and average bar graphs, a catalog table.",
    "layout": {
      "browser": "table",
      "params": "table",
      "composition": "panels"
    }
  },
  {
    "id": "mono",
    "name": "Mono",
    "order": 4,
    "mode": "dark",
    "metaphor": "A data terminal: text and numbers, nothing decorative.",
    "desc": "Value bars you drag anywhere, [x] toggles, thin line meters with numbers, plug-ins as a text list.",
    "layout": {
      "browser": "text",
      "params": "rows",
      "composition": "rows"
    }
  },
  {
    "id": "day",
    "name": "Day",
    "order": 5,
    "mode": "light",
    "metaphor": "A calm, friendly tablet app for daylight sessions.",
    "desc": "Large round knobs, pill sliders and toggle switches, plain words, level zones, plug-ins by purpose.",
    "layout": {
      "browser": "purpose",
      "params": "cards",
      "composition": "focus"
    }
  },
  {
    "id": "arcade",
    "name": "Arcade",
    "order": 6,
    "mode": "dark",
    "metaphor": "An 80s arcade cabinet; the units are PLAYER 1 and PLAYER 2.",
    "desc": "Pixel knobs with 8 directions, block faders, buttons that press down, health-bar meters, a power-up inventory.",
    "layout": {
      "browser": "tiles",
      "params": "item",
      "composition": "cards"
    }
  }
] };
for (const m of window.ApolloThemes.manifests) {
  document.write(`<link rel="stylesheet" href="themes/${m.id}/theme.css"><script src="themes/${m.id}/renderers.js"><\/script>`);
}
