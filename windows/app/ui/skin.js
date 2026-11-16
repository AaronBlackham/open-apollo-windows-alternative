'use strict';
/*
 * Skin: the theme-pack loader and the view layer of every control (docs/THEMES.md).
 *
 * One model, many views. A control (Knob, Fader, Meter in app.js; switches, selectors and the plug-in slot) keeps its
 * model: value, range, step, format, readback state, refusal, its accessible name and the keyboard map, all identical in
 * every theme. A theme pack supplies the *view* and the *interaction adapter* for each control type:
 *
 *   themes/<id>/manifest.json   name, metaphor, mode (dark/light), layout names, defaults (themes/index.js inlines them)
 *   themes/<id>/theme.css       tokens and styles, scoped by [data-look="<id>"]
 *   themes/<id>/renderers.js    Skin.register('<id>', { knob, fader, meter, button, selector, slot, browser, params, ... })
 *
 * themes/index.js (made by tools/themes/make_index.py from the manifests) writes the packs' <link> and <script> tags
 * while the page parses, so every pack is ready before app.js boots and switching is instant. Adding a theme adds a folder
 * and a line in index.js; app.js does not change.
 *
 * Renderer contract (a pack may leave any type out; BASE then draws it):
 *   knob.build(k, R)   -> { draw(s), destroy() }   s = { p, p0, text, value }. Must append k.readout (class k-readout).
 *                         Installs its own pointer interaction with R.vertical / R.circular / R.horizontal / R.stepped.
 *   fader.build(f, R)  -> { draw(s), destroy() }   Must create f.track (class fd-track: the drag surface).
 *   meter.build(m, R)  -> { paint(f, h, g, a), destroy() }   f level, h hold, g ghost, a average (0..1 of the scale).
 *   button.decorate(b, R), selector.decorate(segNode, R), slot.decorate(b, R): add aria-hidden children only (the
 *                         accessible names stay the model's).
 *   browser: { layout, groups(items, ctx), item(node, it, ctx, R) }   params: { layout }   on: { drop(slotNode), refuse(node), clip(node) }
 *
 * Switching is live and lossless: Skin.use(id) rebuilds every live control's view in its own host element (focus,
 * value, pending and refused state, open drawers and meter peaks are the model's and stay).
 */
const Skin = (() => {
  const packs = new Map();               /* id -> { manifest, r } */
  const live = new Set();                /* controls with a view: { node, skinRebuild() } */
  let cur = 'studio';
  const reduced = () => window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  function register(id, renderers) {
    const m = (window.ApolloThemes && window.ApolloThemes.manifests || []).find(x => x.id === id) || { id, name: id, mode: 'dark' };
    packs.set(id, { manifest: m, r: renderers || {} });
  }
  const manifest = (id = cur) => (packs.get(id) || {}).manifest || { id, name: id, mode: 'dark' };
  const has = id => packs.has(id);
  const id = () => cur;
  /* the active pack's renderer of a type, else the base one */
  function r(type) {
    const p = packs.get(cur);
    return (p && p.r[type]) || BASE[type] || null;
  }

  function attach(ctl) { live.add(ctl); }

  /* Every live control takes the new pack's view; detached ones (closed drawers, dropped segments) are forgotten. */
  function use(next) {
    if (!packs.has(next)) next = packs.has('studio') ? 'studio' : [...packs.keys()][0];
    const changed = next !== cur;
    cur = next;
    for (const c of [...live]) {
      if (!c.node.isConnected) { live.delete(c); continue; }
      if (changed || c.skinId !== cur) c.skinRebuild();
    }
    decorateAll();
    return changed;
  }

  /* ---------------------------------------------------------------- decorations (buttons, selectors, slots) */

  function clearDeco(node) { for (const d of node.querySelectorAll(':scope > .sk-deco')) d.remove(); }
  function decorate(kind, node) {
    clearDeco(node);
    const rr = r(kind);
    if (rr && rr.decorate) rr.decorate(node, R);
    node.dataset.skin = cur;
  }
  function decorateAll() {
    for (const b of document.querySelectorAll('.sw')) decorate('button', b);
    for (const s of document.querySelectorAll('.seg')) { for (const b of s.querySelectorAll(':scope > button')) clearDeco(b); decorate('selector', s); if (s._skinSet) s._skinSet(); }
    for (const b of document.querySelectorAll('.ins-slot')) decorate('slot', b);
  }
  /* a decoration element: never in the accessible name */
  const deco = (tag, cls, text) => { const n = document.createElement(tag); n.className = `sk-deco ${cls || ''}`; n.setAttribute('aria-hidden', 'true'); if (text) n.textContent = text; return n; };

  /* events a pack may animate (visual only): a plug-in dropped, a refusal, a clip */
  function on(ev, node) {
    const rr = r('on');
    if (!rr || !rr[ev] || !node || reduced()) return;
    try { rr[ev](node, R); } catch (e) { /* a pack's animation never breaks the control */ }
  }
  /* a short-lived class (animations), removed after ms */
  function pulse(node, cls, ms) {
    if (!node || reduced()) return;
    node.classList.remove(cls);
    void node.offsetWidth;
    node.classList.add(cls);
    setTimeout(() => node.classList.remove(cls), ms);
  }

  /* ---------------------------------------------------------------- interaction adapters */

  /* Vertical drag (the classic): travel px for the whole range, Shift / Ctrl / Alt fine, optional soft detents
     (positions 0..1 that hold the value for a few pixels while dragging through them). */
  function vertical(c, surface, o = {}) {
    const travel = o.travel || 220, detents = o.detents || [], hold = o.hold || 8;
    let y0 = 0, p0 = 0, stuck = 0;
    const down = e => {
      if (e.button !== 0 || c.blocked()) return;
      surface.setPointerCapture(e.pointerId);
      c.dragging = true;
      y0 = e.clientY;
      p0 = clamp(c.o.toPos(c.value), 0, 1);
      stuck = 0;
      c.node.focus();
      e.preventDefault();
    };
    const move = e => {
      if (!c.dragging) return;
      const fine = e.shiftKey || e.ctrlKey || e.altKey;
      let p = clamp(p0 + (y0 - e.clientY) / (fine ? travel * 4 : travel), 0, 1);
      for (const d of detents) {
        const near = Math.abs(p - d) * travel;
        if (near < hold && !fine) { p = d; stuck = 1; break; }
      }
      c.commit(c.snap(c.o.fromPos(p)));
    };
    const up = () => { c.dragging = false; };
    surface.addEventListener('pointerdown', down);
    surface.addEventListener('pointermove', move);
    surface.addEventListener('pointerup', up);
    surface.addEventListener('pointercancel', up);
    return () => { surface.removeEventListener('pointerdown', down); surface.removeEventListener('pointermove', move); surface.removeEventListener('pointerup', up); surface.removeEventListener('pointercancel', up); };
  }

  /* Circular drag around the dial's centre (the HUD): the angle under the pointer is the value, within the sweep. */
  function circular(c, surface, o = {}) {
    const a0 = o.from ?? -135, a1 = o.to ?? 135;
    const at = e => {
      const r0 = surface.getBoundingClientRect();
      const x = e.clientX - (r0.left + r0.width / 2), y = e.clientY - (r0.top + r0.height / 2);
      let a = Math.atan2(x, -y) * 180 / Math.PI;
      a = clamp(a, a0, a1);
      return (a - a0) / (a1 - a0);
    };
    let fineFrom = null;
    const down = e => { if (e.button !== 0 || c.blocked()) return; surface.setPointerCapture(e.pointerId); c.dragging = true; c.node.focus(); fineFrom = null; c.commit(c.snap(c.o.fromPos(at(e)))); e.preventDefault(); };
    const move = e => {
      if (!c.dragging) return;
      if (e.shiftKey || e.ctrlKey || e.altKey) {            /* fine: the angle moves the value a quarter as far */
        const p = at(e), base = clamp(c.o.toPos(c.value), 0, 1);
        if (fineFrom === null) fineFrom = p;
        c.commit(c.snap(c.o.fromPos(clamp(base + (p - fineFrom) / 4, 0, 1))));
        fineFrom = p;
      } else { fineFrom = null; c.commit(c.snap(c.o.fromPos(at(e)))); }
    };
    const up = () => { c.dragging = false; };
    surface.addEventListener('pointerdown', down);
    surface.addEventListener('pointermove', move);
    surface.addEventListener('pointerup', up);
    surface.addEventListener('pointercancel', up);
    return () => { surface.removeEventListener('pointerdown', down); surface.removeEventListener('pointermove', move); surface.removeEventListener('pointerup', up); surface.removeEventListener('pointercancel', up); };
  }

  /* Horizontal bar (the terminal): press anywhere on the bar, the value is where the pointer is. */
  function horizontal(c, surface, o = {}) {
    const at = e => { const b = surface.getBoundingClientRect(); return clamp((e.clientX - b.left) / Math.max(1, b.width), 0, 1); };
    const down = e => { if (e.button !== 0 || c.blocked()) return; surface.setPointerCapture(e.pointerId); c.dragging = true; c.node.focus(); c.commit(c.snap(c.o.fromPos(at(e)))); e.preventDefault(); };
    const move = e => { if (c.dragging) c.commit(c.snap(c.o.fromPos(at(e)))); };
    const up = () => { c.dragging = false; };
    surface.addEventListener('pointerdown', down);
    surface.addEventListener('pointermove', move);
    surface.addEventListener('pointerup', up);
    surface.addEventListener('pointercancel', up);
    return () => { surface.removeEventListener('pointerdown', down); surface.removeEventListener('pointermove', move); surface.removeEventListener('pointerup', up); surface.removeEventListener('pointercancel', up); };
  }

  /* Stepped vertical drag (the cabinet): the value moves in `steps` clicks over the range; each click pulses. */
  function stepped(c, surface, o = {}) {
    const steps = o.steps || 16, px = o.px || 10;
    let y0 = 0, i0 = 0, last = -1;
    const idx = () => Math.round(clamp(c.o.toPos(c.value), 0, 1) * steps);
    const down = e => { if (e.button !== 0 || c.blocked()) return; surface.setPointerCapture(e.pointerId); c.dragging = true; c.node.focus(); y0 = e.clientY; i0 = idx(); last = i0; e.preventDefault(); };
    const move = e => {
      if (!c.dragging) return;
      const i = clamp(i0 + Math.round((y0 - e.clientY) / px), 0, steps);
      if (i === last) return;
      last = i;
      c.commit(c.snap(c.o.fromPos(i / steps)));
      pulse(c.node, 'sk-click', 90);
    };
    const up = () => { c.dragging = false; };
    surface.addEventListener('pointerdown', down);
    surface.addEventListener('pointermove', move);
    surface.addEventListener('pointerup', up);
    surface.addEventListener('pointercancel', up);
    return () => { surface.removeEventListener('pointerdown', down); surface.removeEventListener('pointermove', move); surface.removeEventListener('pointerup', up); surface.removeEventListener('pointercancel', up); };
  }

  /* Click to type (the analyzer's numeric fields), plus the vertical drag on the indicator. */
  function field(c, surface, o = {}) {
    const click = e => { if (e.button === 0 && !c.blocked()) c.edit(); };
    surface.addEventListener('click', click);
    return () => surface.removeEventListener('click', click);
  }

  /* SVG helpers shared by packs */
  const NS = 'http://www.w3.org/2000/svg';
  const s = (tag, attrs = {}) => { const n = document.createElementNS(NS, tag); for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v); return n; };
  const polarXY = (cx, cy, r0, deg) => { const a = deg * Math.PI / 180; return [cx + r0 * Math.sin(a), cy - r0 * Math.cos(a)]; };
  const arc = (cx, cy, r0, a0, a1) => {
    const [x0, y0] = polarXY(cx, cy, r0, a0), [x1, y1] = polarXY(cx, cy, r0, a1);
    return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r0} ${r0} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
  };
  const R = { vertical, circular, horizontal, stepped, field, svg: s, polar: polarXY, arc, deco, pulse, reduced };

  /* ---------------------------------------------------------------- BASE renderers (any pack may leave a type out) */

  const BASE = {
    knob: {
      build(k) {
        const sz = k.o.size;
        const g = s('svg', { width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true', class: 'sk-knob-svg' });
        const track = s('path', { class: 'k-track', 'stroke-width': 7, d: arc(50, 50, 42, -135, 135) });
        const val = s('path', { class: 'k-value', 'stroke-width': 7 });
        const body = s('circle', { class: 'k-body', cx: 50, cy: 50, r: 31, 'stroke-width': 1.5 });
        const ptr = s('line', { class: 'k-ptr', 'stroke-width': 4 });
        g.append(track, val, body, ptr);
        if (k.o.label) k.node.append(deco('div', 'k-label', k.o.label));
        k.node.append(g, k.readout);
        const off = vertical(k, g);
        return {
          draw({ p, p0 }) {
            const a = -135 + p * 270, b = -135 + p0 * 270;
            val.setAttribute('d', Math.abs(p - p0) > 0.002 ? arc(50, 50, 42, Math.min(a, b), Math.max(a, b)) : '');
            const [x0, y0] = polarXY(50, 50, 12, a), [x1, y1] = polarXY(50, 50, 27, a);
            ptr.setAttribute('x1', x0); ptr.setAttribute('y1', y0); ptr.setAttribute('x2', x1); ptr.setAttribute('y2', y1);
          },
          destroy: off,
        };
      },
    },
    fader: {
      build(f) {
        f.cap = el('div', { class: 'fd-cap' });
        f.fill = el('div', { class: 'fd-fill' });
        f.track = el('div', { class: 'fd-track' }, f.fill, f.cap);
        f.node.append(f.scaleNode(), f.track);
        const off = f.trackDrag(f.track);
        return {
          draw({ p }) { f.cap.style.bottom = `${(p * 100).toFixed(2)}%`; f.fill.style.height = `${(p * 100).toFixed(2)}%`; },
          destroy: off,
        };
      },
    },
    meter: {
      build(m) { return { paint() {} }; },
    },
  };

  return { register, manifest, has, id, r, use, attach, decorate, decorateAll, deco, on, pulse, R, BASE, reduced };
})();
