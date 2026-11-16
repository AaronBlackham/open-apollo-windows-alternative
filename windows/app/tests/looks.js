// Looks (themes.css): seven looks on one set of tokens, picked in Settings (each item drawn in its own look) or with
// Ctrl+Shift+T, remembered; every screen follows; text contrast and the state colours (running, waiting, refused /
// clip, bypassed) checked per look; Arcade's own faces load and its scanlines are off until asked for. Classic is the
// default for a new config (a saved look is kept), lays the strips out like console software, keeps 24 px targets,
// holds still with reduced motion, fits at 125 %, and switches to Studio and back without losing anything.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };
const LOOKS = ['classic', 'studio', 'cyber', 'lab', 'mono', 'day', 'arcade'];
const VIEWS = ['control', 'status', 'cues', 'units', 'correction', 'channels', 'device', 'settings'];

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(ui + '?view=settings');
  await page.waitForTimeout(500);

  /* the tokens of the look on screen, as the browser resolves them */
  const tokens = () => page.evaluate(() => {
    const probe = document.createElement('div');
    document.body.append(probe);
    const get = v => { probe.style.color = `var(${v})`; return getComputedStyle(probe).color; };
    const t = {};
    for (const v of ['--bg', '--view-bg', '--panel', '--text', '--text-dim', '--text-faint', '--accent', '--accent-ink', '--red', '--red-ink', '--ok', '--warn', '--idle']) t[v] = get(v);
    probe.remove();
    return t;
  });
  const rgb = c => c.match(/[\d.]+/g).slice(0, 3).map(Number);
  const lum = c => { const [r, g, b] = rgb(c).map(x => { x /= 255; return x <= 0.03928 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; }); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
  const ratio = (a, b) => { const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p); return (x + 0.05) / (y + 0.05); };
  const dist = (a, b) => { const [p, q] = [rgb(a), rgb(b)]; return Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]); };

  // 1. Settings lists the seven looks (Classic first), each drawn in its own look; a click switches the whole app.
  const items = page.getByRole('radiogroup', { name: 'Look' }).getByRole('radio');
  check(await items.count() === 7 && (await items.evaluateAll(b => b.map(x => x.dataset.look))).join() === LOOKS.join(), 'Settings: seven looks, Classic first');
  for (const look of LOOKS) {
    await page.locator(`.look-item[data-look="${look}"]`).click();
    const t = await tokens();
    const views = await page.evaluate(([vs, bg]) => vs.map(v => { const n = document.getElementById(`view-${v}`); n.hidden = false; const c = getComputedStyle(n).backgroundColor; return c === bg; }), [VIEWS, t['--view-bg']]);
    await page.evaluate(() => showView('settings'));
    check(await page.evaluate(() => document.documentElement.dataset.look) === look && views.every(Boolean) &&
      await page.locator(`.look-item[data-look="${look}"]`).getAttribute('aria-checked') === 'true', `${look}: on, and every screen takes its background (${views.filter(Boolean).length}/${VIEWS.length})`);
    const bad = [];
    for (const k of ['--text', '--text-dim', '--text-faint']) for (const on of ['--panel', '--bg']) if (ratio(t[k], t[on]) < 4.5) bad.push(`${k}/${on} ${ratio(t[k], t[on]).toFixed(2)}`);
    if (ratio(t['--accent-ink'], t['--accent']) < 4.5) bad.push(`accent-ink ${ratio(t['--accent-ink'], t['--accent']).toFixed(2)}`);
    if (ratio(t['--red-ink'], t['--red']) < 4.5) bad.push(`red-ink ${ratio(t['--red-ink'], t['--red']).toFixed(2)}`);
    for (const k of ['--ok', '--warn', '--red']) if (ratio(t[k], t['--panel']) < 3) bad.push(`${k}/panel ${ratio(t[k], t['--panel']).toFixed(2)}`);
    check(!bad.length, `${look}: text 4.5:1 on panel and background, inks on their fills, state colours 3:1 (${bad.join(', ') || 'all pass'})`);
    const st = ['--ok', '--warn', '--red', '--idle'], close = [];
    for (let i = 0; i < st.length; i++) for (let j = i + 1; j < st.length; j++) if (dist(t[st[i]], t[st[j]]) < 90) close.push(`${st[i]}~${st[j]}`);
    if (dist(t['--accent'], t['--red']) < 90) close.push('--accent~--red');
    check(!close.length, `${look}: running, waiting, refused / clip and bypassed stay apart (${close.join(', ') || 'distinct'})`);
  }

  // 2. Ctrl+Shift+T steps to the next look and it is remembered.
  await page.locator('.look-item[data-look="studio"]').click();
  await page.locator('body').click({ position: { x: 5, y: 300 } });
  await page.keyboard.press('Control+Shift+T');
  await page.waitForTimeout(500);
  const kept = await page.evaluate(() => JSON.parse(localStorage.getItem('apollo-control') || '{}').appearance);
  check(await page.evaluate(() => document.documentElement.dataset.look) === 'cyber' && kept && kept.look === 'cyber', 'Ctrl+Shift+T: Studio -> Cyberpunk, saved');
  await page.reload();
  await page.waitForTimeout(500);
  check(await page.evaluate(() => document.documentElement.dataset.look) === 'cyber', 'after a restart: still Cyberpunk');

  // 3. Arcade: its own pixel and segment faces load; scanlines off until asked for.
  await page.evaluate(() => { showView('settings'); setLook('arcade'); });
  await page.evaluate(() => document.fonts.ready);
  check(await page.evaluate(() => document.fonts.check('12px "Avium Pixel"') && document.fonts.check('12px "Avium Segment"')) &&
    /Avium Pixel/.test(await page.locator('#view-settings .sect-title').first().evaluate(e => getComputedStyle(e).fontFamily)), 'Arcade: headings in Avium Pixel, both faces loaded');
  check(await page.evaluate(() => document.documentElement.dataset.scan) === '0', 'Arcade: no scanlines by default');
  await page.getByRole('checkbox', { name: 'Scanlines in the Arcade look' }).check();
  check(await page.evaluate(() => document.documentElement.dataset.scan) === '1', 'scanlines on when asked for');
  await page.getByRole('checkbox', { name: 'Scanlines in the Arcade look' }).uncheck();

  // 3b. Theme packs: switching swaps the renderer of every control type (knob, fader, meter, switch, selector, plug-in
  //     slot) and the seven renderers differ; switching is lossless (an open parameter view, the focus, the values stay).
  await page.evaluate(() => { setLook('studio'); showView('control'); });
  await page.locator('#plb .plb-item[data-name="Demo Compressor"]').focus();
  await page.keyboard.press('Enter');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(150);
  await page.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="0"]').click();
  await page.getByRole('slider', { name: 'Input 3 gain', exact: true }).focus();
  const before = await page.evaluate(() => [document.activeElement.getAttribute('aria-valuenow'), !document.getElementById('plDrawer').hidden]);
  const sigs = {};
  for (const look of LOOKS) {
    await page.evaluate(l => setLook(l), look);
    const r = await page.evaluate(l => {
      const types = { knob: '.knob', fader: '.fader', meter: '#inputs .meter-wrap', button: '#inputs .sw', selector: '.seg', slot: '#inputs .ins-slot' };
      const out = {};
      for (const [t, q] of Object.entries(types)) { const all = [...document.querySelectorAll(q)]; out[t] = all.length > 0 && all.every(n => n.dataset.skin === l); }
      const k = document.querySelector('#inputs .knob'), f = document.querySelector('#inputs .fader');
      out.sig = [k.firstElementChild.getAttribute('class'), f.lastElementChild.getAttribute('class')].join('|');
      out.focus = document.activeElement.getAttribute('aria-label');
      out.value = document.activeElement.getAttribute('aria-valuenow');
      out.drawer = !document.getElementById('plDrawer').hidden;
      return out;
    }, look);
    sigs[look] = r.sig;
    const bad = Object.entries(r).filter(([t, v]) => ['knob', 'fader', 'meter', 'button', 'selector', 'slot'].includes(t) && !v).map(([t]) => t);
    check(!bad.length && r.focus === 'Input 3 gain' && r.value === before[0] && r.drawer === before[1],
      `${look}: every control type drawn by its pack (${bad.join(', ') || 'knob, fader, meter, switch, selector, slot'}); focus, value and the open parameter view kept`);
  }
  check(new Set(Object.values(sigs).map(x => x.split('|')[0])).size === LOOKS.length && new Set(Object.values(sigs).map(x => x.split('|')[1])).size === LOOKS.length,
    `seven different knob and fader renderers (${Object.values(sigs).join(' ; ')})`);
  await page.evaluate(() => setLook('studio'));

  // 4. An old config (theme light, accent amber) becomes the Day look with its own accent.
  const old = await browser.newPage();
  await old.addInitScript(() => localStorage.setItem('apollo-control', JSON.stringify({ appearance: { theme: 'light', accent: 'amber', scale: 100 } })));
  await old.goto(ui);
  await old.waitForTimeout(300);
  check(await old.evaluate(() => [document.documentElement.dataset.look, cfg.appearance.accent].join()) === 'day,look', 'a config from before the looks: light -> Day, the old default accent -> the look\'s own');
  await old.close();

  // 5. Classic, the standard mixer layout. A fresh config starts on it; a saved look (and a dark config from before
  //    the looks) is kept. It is first in Settings with a plain description.
  const fresh = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  fresh.on('pageerror', e => errors.push(e.message));
  fresh.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await fresh.goto(ui + '?view=settings');
  await fresh.waitForTimeout(500);
  const first = fresh.getByRole('radiogroup', { name: 'Look' }).getByRole('radio').first();
  check(await fresh.evaluate(() => [document.documentElement.dataset.look, cfg.appearance.look, defaultConfig().appearance.look].join()) === 'classic,classic,classic' &&
    await first.getAttribute('data-look') === 'classic' && await first.getAttribute('aria-checked') === 'true' &&
    await first.locator('.look-name').textContent() === 'Classic' && /^Classic: A standard mixer layout/.test(await first.getAttribute('aria-label')),
    'a fresh config: Classic, first in Settings and chosen ("Classic: A standard mixer layout...")');
  const kept5 = [];
  for (const a of [{ look: 'studio' }, { look: 'day' }, { look: 'mono' }, { theme: 'dark', accent: 'amber' }]) {
    const p = await browser.newPage();
    await p.addInitScript(x => localStorage.setItem('apollo-control', JSON.stringify({ version: 1, appearance: { scale: 100, ...x } })), a);
    await p.goto(ui);
    await p.waitForTimeout(300);
    kept5.push(await p.evaluate(() => document.documentElement.dataset.look));
    await p.close();
  }
  check(kept5.join() === 'studio,day,mono,studio', `a saved look is kept, and a dark config from before the looks stays Studio (${kept5.join()})`);

  // 5b. The layout console software uses: per strip number, gain, mic / line, 48V..., inserts, sends, pan, the fader with
  //     the meter beside it, mute / solo, the name; each unit's master section on its right; Studio's meter bridge off.
  await fresh.evaluate(() => showView('control'));
  await fresh.waitForTimeout(400);
  const lay = await fresh.evaluate(() => {
    const s = document.querySelector('#inputs .strip[data-unit="0"][data-ch="0"]');
    const box = q => { const n = s.querySelector(q); return n ? n.getBoundingClientRect() : null; };
    const r = { num: box('.strip-num'), gain: box('.st-pre .knob'), ml: box('.st-pre .seg'), sw: box('.sw-grid'), ins: box('.st-ins'), aux: box('.st-aux'),
      pan: box('.mix-top .knob'), fader: box('.fader'), ms: box('.mix-ms'), name: box('.strip-name') };
    const order = Object.keys(r);
    const ok = order.every((k, i) => r[k] && (i === 0 || r[k].top >= r[order[i - 1]].bottom - 1));
    const meter = s.querySelector('.fd-row .meter-wrap').getBoundingClientRect();
    const out = [];
    for (const sg of segs.values()) out.push(sg.master.node.getBoundingClientRect().left >= sg.strips[sg.strips.length - 1].node.getBoundingClientRect().right - 1);
    return { ok, seq: order.map(k => (r[k] ? Math.round(r[k].top) : 'x')).join(' '), comp: document.getElementById('view-control').dataset.comp,
      beside: meter.left >= r.fader.right - 1 && meter.top < r.fader.bottom && meter.bottom > r.fader.top, masters: out.length === 2 && out.every(Boolean),
      faderH: Math.round(r.fader.height), bridge: !!document.getElementById('meterBridge').offsetParent };
  });
  check(lay.ok && lay.beside && lay.masters && !lay.bridge && lay.comp === 'strips' && lay.faderH >= 120,
    `Classic: number, gain, mic/line, 48V..., inserts, sends, pan, fader (${lay.faderH} px, meter beside it), mute / solo, name; masters on the right; no meter bridge (tops ${lay.seq})`);
  check(await fresh.evaluate(() => ['knob', 'fader', 'meter'].every(t => [...document.querySelectorAll(t === 'meter' ? '#inputs .meter-wrap' : `#inputs .${t}`)].every(n => n.dataset.skin === 'classic'))) &&
    await fresh.locator('#inputs .cl-knob').count() > 0 && await fresh.locator('#inputs .cl-track').count() > 0 && await fresh.locator('#inputs .cl-meter').count() > 0,
    'Classic: its own knob, fader and meter views on every strip');

  // 5c. Every control on the Control view is at least 24 x 24 px (filled chains, the AUX strips open).
  await fresh.evaluate(() => { Plug.demoFill(); setAuxOpen(true); });
  await fresh.waitForTimeout(300);
  const small = await fresh.evaluate(() => {
    const bad = [];
    for (const n of document.querySelectorAll('#view-control button, #view-control [role="slider"], #view-control select, #view-control .strip-name, #view-control label.check')) {
      if (!n.offsetParent) continue;
      const b = n.getBoundingClientRect();
      if (b.width && (b.width < 23.5 || b.height < 23.5)) bad.push(`${(n.getAttribute('aria-label') || n.textContent).trim().slice(0, 24)} ${Math.round(b.width)}x${Math.round(b.height)}`);
    }
    return bad;
  });
  check(!small.length, `Classic: every control on the Control view at least 24 x 24 px (${small.slice(0, 6).join(', ') || 'all'})`);

  // 5d. Live and lossless: Classic -> Studio -> Classic keeps values, mute, the AUX strips, an open parameter view, the
  //     focus, the strip scroll and the meter peaks.
  await fresh.evaluate(() => { setInput(0, 'gain', 44, { unit: 0 }); setMix(2, 'mute', true, 0); });
  await fresh.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="1"]').click();
  await fresh.waitForTimeout(400);
  await fresh.getByRole('slider', { name: 'Input 1 mix level', exact: true }).focus();
  await fresh.evaluate(() => { document.getElementById('inputs').scrollLeft = 120; });
  const snap = () => fresh.evaluate(() => ({
    look: document.documentElement.dataset.look, gain: S.ctl[0].inputs[0].gain, mute: mixOfBus(0, 2).mute,
    aux: getComputedStyle(document.querySelector('.aux-sec[data-unit="0"]')).display !== 'none', drawer: !document.getElementById('plDrawer').hidden,
    focus: document.activeElement.getAttribute('aria-label'), scroll: document.getElementById('inputs').scrollLeft,
    hold: segs.get(0).strips[0].meter.hold > -200, knob: document.querySelector('#inputs .strip[data-unit="0"][data-ch="0"] .st-pre .knob').getAttribute('aria-valuenow'),
  }));
  const s0 = await snap();
  await fresh.evaluate(() => setLook('studio'));
  await fresh.waitForTimeout(150);
  const s1 = await snap();
  await fresh.evaluate(() => setLook('classic'));
  await fresh.waitForTimeout(150);
  const s2 = await snap();
  const same = (x, y) => ['gain', 'mute', 'aux', 'drawer', 'focus', 'knob'].every(k => x[k] === y[k]) && Math.abs(x.scroll - y.scroll) <= 2 && y.hold;
  check(s0.look === 'classic' && s1.look === 'studio' && s2.look === 'classic' && same(s0, s1) && same(s0, s2) && s0.gain === 44 && s0.knob === '44' &&
    s0.mute && s0.aux && s0.drawer && s0.focus === 'Input 1 mix level' && s0.scroll > 0,
    `Classic -> Studio -> Classic: values, mute, AUX strips, the open parameter view, focus, scroll and meter peaks kept (${JSON.stringify(s2)})`);

  // 5e. The same keys as every theme: Ctrl+K, M, Ctrl+Shift+T (Arcade wraps round to Classic), "theme classic".
  await fresh.evaluate(() => { Plug.openDrawer(null); setAuxOpen(false); });
  await fresh.locator('.brand').click();
  await fresh.keyboard.press('Control+k');
  const pal5 = await fresh.getByRole('dialog', { name: 'Command palette' }).isVisible();
  await fresh.keyboard.press('Escape');
  await fresh.locator('.brand').click();
  const m0 = await fresh.evaluate(() => S.mon.mute);
  await fresh.keyboard.press('m');
  await fresh.waitForTimeout(200);
  const m1 = await fresh.evaluate(() => S.mon.mute);
  await fresh.keyboard.press('m');
  await fresh.evaluate(() => setLook('arcade'));
  await fresh.locator('.brand').click();
  await fresh.keyboard.press('Control+Shift+T');
  const wrap = await fresh.evaluate(() => document.documentElement.dataset.look);
  await fresh.evaluate(() => Cmd.run('theme studio'));
  const viaCmd = await fresh.evaluate(() => document.documentElement.dataset.look);
  await fresh.evaluate(() => Cmd.run('theme classic'));
  check(pal5 && m1 === !m0 && wrap === 'classic' && viaCmd === 'studio' && await fresh.evaluate(() => document.documentElement.dataset.look) === 'classic',
    `Classic: Ctrl+K opens the palette, M mutes the monitor, Ctrl+Shift+T from Arcade lands on Classic, "theme classic" (${[pal5, m1 === !m0, wrap, viaCmd].join()})`);

  // 5f. Reduced motion: Classic's short state transitions are off; without it they are short (80 ms).
  const dur = () => fresh.evaluate(() => getComputedStyle(document.querySelector('#inputs .sw')).transitionDuration);
  await fresh.emulateMedia({ reducedMotion: 'reduce' });
  const still = await dur();
  await fresh.emulateMedia({ reducedMotion: 'no-preference' });
  const moving = await dur();
  check(still.split(',').every(t => parseFloat(t) === 0) && moving.split(',').every(t => parseFloat(t) > 0 && parseFloat(t) <= 0.15),
    `Classic: no transitions with reduced motion (${still}), short ones without (${moving})`);

  // 5g. 125 %: nothing past an edge but the strip scroller, every strip row whole; focus visible on a knob.
  await fresh.evaluate(() => { cfg.appearance.scale = 125; applyAppearance(); showView('control'); });
  await fresh.waitForTimeout(400);
  const z = await fresh.evaluate(() => {
    const bad = [];
    if (document.documentElement.scrollWidth > document.documentElement.clientWidth) bad.push('document');
    for (const q of ['#view-control', '#monitor']) { const n = document.querySelector(q); if (n && n.scrollWidth > n.clientWidth + 1) bad.push(q); }
    for (const s of document.querySelectorAll('#inputs .strip')) {
      const h = q => s.querySelector(q).getBoundingClientRect().height;
      const fd = s.classList.contains('follower') ? h('.fd-linked') : h('.fader');      /* a linked pair's second input shows LINKED */
      if (h('.seg') < 24 || h('.sw-grid') < 51 || h('.strip-name') < 24 || h('.mix-ms') < 24 || fd < 60) bad.push(`input ${+s.dataset.ch + 1}`);
    }
    return bad;
  });
  await fresh.getByRole('slider', { name: 'Input 2 gain', exact: true }).focus();
  await fresh.keyboard.press('ArrowUp');
  const ring = await fresh.evaluate(() => { const svg = document.activeElement.querySelector('svg'); const cs = getComputedStyle(svg); return document.activeElement.matches(':focus-visible') && cs.outlineStyle !== 'none' && parseFloat(cs.outlineWidth) > 0; });
  check(!z.length && ring, `Classic at 125 %: no overflow past the strip scroller, every strip row whole, the focus ring shows (${z.join(', ') || 'none'})`);
  await fresh.evaluate(() => { cfg.appearance.scale = 100; applyAppearance(); });
  await fresh.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
