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

  // 5b. The console-app layout. Per strip, top to bottom: number, input select, 48V / PAD / LO CUT / polarity, the gain,
  //     the Unison slot right under it, the inserts, REC / MON, the sends (AUX 1, AUX 2, CUE 1, CUE 2), pan, solo then
  //     mute, the fader with the meter beside it, the label. Around them: the View column on the left, each unit's monitor
  //     section on its right (meters above the knob), the control room column on the far right, the info bar (rate) at the
  //     bottom, the plug-in browser closed until an insert asks for it, no meter bridge.
  await fresh.evaluate(() => showView('control'));
  await fresh.locator('.brand').click();
  await fresh.waitForTimeout(500);
  const lay = await fresh.evaluate(() => {
    const s = document.querySelector('#inputs .strip[data-unit="0"][data-ch="0"]');
    const box = q => { const n = s.querySelector(q); return n ? n.getBoundingClientRect() : null; };
    const r = { num: box('.strip-num'), ml: box('.st-pre .seg'), sw: box('.sw-grid'), gain: box('.st-pre .knob'), uni: box('.ins-uni'),
      ins: box('.ins-row:not(.ins-uni-row) .ins-slot'), recmon: box('.ins-recmon'), aux: box('.aux-send[data-aux="1"]'), cue: box('.cl-cue[data-cue="0"]'),
      pan: box('.mix-top .knob'), ms: box('.mix-ms'), fader: box('.fader'), name: box('.strip-name') };
    const order = Object.keys(r);
    const ok = order.every((k, i) => r[k] && (i === 0 || r[k].top >= r[order[i - 1]].bottom - 1));
    const solo = s.querySelector('.mix-ms [aria-label$="solo in the monitor mix"]').getBoundingClientRect();
    const mute = s.querySelector('.mix-ms [aria-label$="mute in the monitor mix"]').getBoundingClientRect();
    const labels = [...s.querySelectorAll('.st-aux .k-label')].map(n => n.textContent).join(',');
    const meter = s.querySelector('.fd-row .meter-wrap').getBoundingClientRect();
    const rc = n => n.getBoundingClientRect();
    const col = rc(document.getElementById('clViewCol')), desk = rc(document.getElementById('desk')), mon = rc(document.getElementById('monitor'));
    const sg = segs.get(0), last = rc(sg.tb.node), mo = sg.master.node;
    const moOk = rc(mo.querySelector('.mo-meters')).bottom <= rc(mo.querySelector('.mo-ctl > .knob')).top + 1;
    const rate = document.getElementById('rateBar'), inputs = rc(document.getElementById('inputs'));
    const plb = getComputedStyle(document.getElementById('plb'));
    return { ok, seq: order.map(k => (r[k] ? Math.round(r[k].top) : 'x')).join(' '), comp: document.getElementById('view-control').dataset.comp,
      soloFirst: solo.right <= mute.left + 1 && Math.abs(solo.top - mute.top) < 2, labels,
      beside: meter.left >= r.fader.right - 1 && meter.top < r.fader.bottom && meter.bottom > r.fader.top,
      around: col.right <= desk.left + 1 && mon.left >= desk.right - 1, master: rc(mo).left >= last.right - 1 && moOk,
      info: !rate.hidden && rc(rate).top >= inputs.bottom - 1, plbClosed: plb.opacity === '0' && plb.pointerEvents === 'none',
      faderH: Math.round(r.fader.height), bridge: !!document.getElementById('meterBridge').offsetParent };
  });
  check(lay.ok && lay.soloFirst && lay.beside && lay.comp === 'strips' && lay.faderH >= 100 && /^AUX 1,AUX 2,CUE 1,CUE 2/.test(lay.labels),
    `Classic strip: number, MIC/LINE, 48V..., gain, Unison, inserts, REC/MON, AUX 1/2, CUE 1/2, pan, solo then mute, fader (${lay.faderH} px, meter beside it), label (tops ${lay.seq}; ${lay.labels})`);
  check(lay.around && lay.master && lay.info && lay.plbClosed && !lay.bridge,
    `Classic window: View column left, control room right, each unit's monitor section after its strips (meters over the knob), info bar under the strips, browser closed, no meter bridge (${JSON.stringify({ a: lay.around, m: lay.master, i: lay.info, p: lay.plbClosed })})`);
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

  // 5h. The View column. INPUTS hides the inserts and sends (Unison stays), INSERTS shows every insert row, SENDS every
  //     cue send, OVERVIEW all of it again, and the choice is saved; CTRL ROOM hides and shows the control room column;
  //     CLEAR SOLO clears every solo of both units and a second press brings them back; CLEAR CLIPS; SETTINGS.
  const col = fresh.locator('#clViewCol');
  const shown = q => fresh.evaluate(x => { const n = document.querySelector(x); return !!n && !!n.offsetParent; }, q);
  const strip1 = '#inputs .strip[data-unit="0"][data-ch="0"]';
  await col.getByRole('button', { name: 'INPUTS', exact: true }).click();
  await fresh.waitForTimeout(500);
  const vIn = [await shown(`${strip1} .ins-uni`), !(await shown(`${strip1} .ins-row[data-n="0"] .ins-slot`)), !(await shown(`${strip1} .st-aux`)), await shown(`${strip1} .fader`),
    await fresh.evaluate(() => JSON.parse(localStorage.getItem('apollo-control')).classic.view)];
  await col.getByRole('button', { name: 'INSERTS', exact: true }).click();
  const vIns = [!(await shown(`${strip1} .st-pre`)), await shown(`${strip1} .ins-row[data-n="3"] .ins-slot`), await shown(`${strip1} .fader`)];
  await col.getByRole('button', { name: 'SENDS', exact: true }).click();
  const vSe = [!(await shown(`${strip1} .st-pre`)), !(await shown(`${strip1} .st-ins`)), await shown(`${strip1} .cl-cue[data-cue="3"]`)];
  await col.getByRole('button', { name: 'OVERVIEW', exact: true }).click();
  const vOv = [await shown(`${strip1} .st-pre`), await shown(`${strip1} .ins-row[data-n="0"] .ins-slot`), await shown(`${strip1} .cl-cue[data-cue="1"]`), !(await shown(`${strip1} .cl-cue[data-cue="3"]`))];
  check([...vIn.slice(0, 4), ...vIns, ...vSe, ...vOv].every(Boolean) && vIn[4] === 'inputs' && await fresh.evaluate(() => cfg.classic.view) === 'overview',
    `View column: INPUTS / INSERTS / SENDS / OVERVIEW show their sections, the choice saved (${JSON.stringify([vIn, vIns, vSe, vOv])})`);
  await col.getByRole('button', { name: 'Show the control room column', exact: true }).click();
  const crOff = !(await shown('#monitor'));
  await col.getByRole('button', { name: 'Show the control room column', exact: true }).click();
  check(crOff && await shown('#monitor'), 'View column: CTRL ROOM hides and shows the control room column');
  await fresh.evaluate(() => { setMix(0, 'solo', true, 0); setMix(1, 'solo', true, 1); });
  await fresh.waitForTimeout(400);
  const solo = col.getByRole('button', { name: /^Clear solo/ });
  const lit = [await solo.getAttribute('aria-label'), await solo.evaluate(n => n.classList.contains('cl-lit'))];
  await solo.click();
  const cleared = await fresh.evaluate(() => [mixOfBus(0, 0).solo, mixOfBus(1, 1).solo].join());
  await solo.click();
  const back = await fresh.evaluate(() => [mixOfBus(0, 0).solo, mixOfBus(1, 1).solo].join());
  await solo.click();
  check(lit[0] === 'Clear solo: 2 soloed' && lit[1] && cleared === 'false,false' && back === 'true,true' && await fresh.evaluate(() => !mixOfBus(0, 0).solo),
    `View column: CLEAR SOLO lights with the count, clears every solo of both units, a second press brings them back (${lit.join()} ${cleared} ${back})`);
  await fresh.evaluate(() => segs.get(0).strips[0].meter.update(0, 0.03, performance.now()));
  const clip0 = await fresh.evaluate(() => segs.get(0).strips[0].meter.clip);
  await col.getByRole('button', { name: 'CLEAR CLIPS', exact: true }).click();
  const clip1 = await fresh.evaluate(() => segs.get(0).strips[0].meter.clip);
  await col.getByRole('button', { name: 'SETTINGS', exact: true }).click();
  const setv = await fresh.evaluate(() => S.view);
  await fresh.evaluate(() => showView('control'));
  check(clip0 && !clip1 && setv === 'settings', `View column: CLEAR CLIPS clears the clip lights, SETTINGS opens Settings (${clip0},${clip1},${setv})`);

  // 5i. The plug-in browser opens from an empty insert and closes once the plug-in is in (focus back on the insert);
  //     Ctrl+F opens it and Esc closes it; SHOW PLUG-INS keeps it open until pressed again.
  const plbOpen = async () => { await fresh.waitForTimeout(250); return fresh.evaluate(() => { const c = getComputedStyle(document.getElementById('plb')); return c.opacity === '1' && c.pointerEvents !== 'none'; }); };
  const slot5 = fresh.locator('#inputs .ins-slot[data-unit="0"][data-ch="4"][data-n="0"]');
  await slot5.click();
  const pickOpen = await plbOpen();
  await fresh.locator('#plb .plb-item[data-name="Demo Compressor"]').focus();
  await fresh.keyboard.press('Enter');
  await fresh.waitForTimeout(400);
  const placed = [await plbOpen(), await fresh.evaluate(() => document.activeElement.classList.contains('ins-slot') && document.activeElement.dataset.ch === '4'),
    await slot5.getAttribute('data-state')];
  check(pickOpen && !placed[0] && placed[1] && placed[2] !== 'empty',
    `browser: an empty insert opens it, the pick goes in, it closes and the insert has the focus (${pickOpen},${placed.join()})`);
  await fresh.locator('.brand').click();
  await fresh.keyboard.press('Control+f');
  const ctrlF = await plbOpen();
  await fresh.keyboard.press('Escape');
  const escClosed = !(await plbOpen());
  await col.getByRole('button', { name: 'Show the plug-in browser', exact: true }).click();
  const pinned = await plbOpen();
  await fresh.locator('.brand').click();
  const stays = await plbOpen();
  await col.getByRole('button', { name: 'Show the plug-in browser', exact: true }).click();
  await fresh.locator('.brand').click();
  check(ctrlF && escClosed && pinned && stays && !(await plbOpen()), `browser: Ctrl+F opens it, Esc closes it, SHOW PLUG-INS keeps it open (${[ctrlF, escClosed, pinned, stays].join()})`);

  // 5j. The strips' cue sends drive the cue mixes (the same sends as the Cues page) and follow changes made there.
  const cue = fresh.getByRole('slider', { name: 'Input 2 send to CUE 1', exact: true });
  await cue.focus();
  await fresh.keyboard.press('PageUp');
  await fresh.waitForTimeout(200);
  const sent = await fresh.evaluate(() => sendOf(0, 0, 1).level);
  await fresh.locator('.brand').click();
  await fresh.evaluate(() => setSend(1, 1, 'level', -20, 0));
  await fresh.waitForTimeout(400);
  const cue2 = await fresh.getByRole('slider', { name: 'Input 2 send to CUE 2', exact: true }).getAttribute('aria-valuenow');
  check(sent === -109 && cue2 === '-20', `strip cue sends: PageUp on CUE 1 sets the cue send (${sent}); a change from the Cues page shows on CUE 2 (${cue2})`);

  // 5k. The monitor section: the knob's ring turns red while the monitor is muted.
  const knobRing = () => fresh.evaluate(() => getComputedStyle(segs.get(0).master.node.querySelector('.mo-ctl > .knob .cl-k-val')).stroke);
  const ring0 = await knobRing();
  await fresh.evaluate(() => setMon('mute', true, 0));
  await fresh.waitForTimeout(300);
  const ring1 = await knobRing();
  await fresh.evaluate(() => setMon('mute', false, 0));
  check(ring0 !== ring1 && /255, 107, 97/.test(ring1), `monitor section: the knob's ring turns red while muted (${ring0} -> ${ring1})`);

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

  // 5l. Another look takes the Classic pieces away and is laid out as before (its browser docked, no View column, no strip
  //     cue sends, no Classic attributes); back on Classic they return.
  await fresh.evaluate(() => setLook('studio'));
  await fresh.waitForTimeout(300);
  const away = await fresh.evaluate(() => {
    const v = document.getElementById('view-control');
    return { col: !!document.getElementById('clViewCol'), cues: document.querySelectorAll('.cl-cue').length, attrs: Object.keys(v.dataset).filter(k => /^cl/.test(k)).join(),
      plb: getComputedStyle(document.getElementById('plb')).position, bridge: !!document.getElementById('meterBridge').offsetParent };
  });
  await fresh.evaluate(() => setLook('classic'));
  await fresh.waitForTimeout(400);
  const again = await fresh.evaluate(() => [!!document.getElementById('clViewCol'), document.querySelectorAll('#inputs .cl-cue').length > 0, document.getElementById('view-control').dataset.clView]);
  check(!away.col && !away.cues && !away.attrs && away.plb !== 'absolute' && away.bridge && again.join() === 'true,true,overview',
    `Classic -> Studio: no View column, cue sends or Classic attributes, Studio's browser docked and its meter bridge on; back again (${JSON.stringify(away)} ${again.join()})`);
  await fresh.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
