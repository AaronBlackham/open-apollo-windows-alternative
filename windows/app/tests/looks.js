// Looks (themes.css): six looks on one set of tokens, picked in Settings (each item drawn in its own look) or with
// Ctrl+Shift+T, remembered; every screen follows; text contrast and the state colours (running, waiting, refused /
// clip, bypassed) checked per look; Arcade's own faces load and its scanlines are off until asked for.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };
const LOOKS = ['studio', 'cyber', 'lab', 'mono', 'day', 'arcade'];
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

  // 1. Settings lists the six looks, each drawn in its own look; a click switches the whole app.
  const items = page.getByRole('radiogroup', { name: 'Look' }).getByRole('radio');
  check(await items.count() === 6 && (await items.evaluateAll(b => b.map(x => x.dataset.look))).join() === LOOKS.join(), 'Settings: six looks');
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
  //     slot) and the six renderers differ; switching is lossless (an open parameter view, the focus, the values stay).
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
    `six different knob and fader renderers (${Object.values(sigs).join(' ; ')})`);
  await page.evaluate(() => setLook('studio'));

  // 4. An old config (theme light, accent amber) becomes the Day look with its own accent.
  const old = await browser.newPage();
  await old.addInitScript(() => localStorage.setItem('apollo-control', JSON.stringify({ appearance: { theme: 'light', accent: 'amber', scale: 100 } })));
  await old.goto(ui);
  await old.waitForTimeout(300);
  check(await old.evaluate(() => [document.documentElement.dataset.look, cfg.appearance.accent].join()) === 'day,look', 'a config from before the looks: light -> Day, the old default accent -> the look\'s own');
  await old.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
