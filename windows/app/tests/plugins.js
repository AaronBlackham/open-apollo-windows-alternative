// [Plug-ins] UAD plug-in inserts on the Control view (docs/PLUGINS.md 5, docs/apollo-control-units.md 1.5), through a stub
// host: the browser docked beside the strips (grouped by type from the host's UA categories, name rules otherwise, counts,
// search, FITS, previews with a placeholder, the detail card), the insert slot on every strip, drag and drop with the mouse
// (load, move, unload onto the browser) and the keyboard (pick up, place, Esc), every slot saying why it does not take a
// plug-in (not licensed, no room, power cycle, not on this driver build, mono only), the parameter drawer (the plug-in's own
// texts, pending until the host answers, RB OK / NOT CONFIRMED), bypass, refusals inline, saved inserts coming back, stereo
// on a linked pair, a second unit that hosts, Settings' hosting panel, the demo, and 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };
const DOCKED = JSON.stringify({ appearance: { look: 'studio' } });   /* [Classic] a saved Studio look: the docked browser */

const X8P = { id: 'SN22532017106180', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const TW_INFO = { ...INFO, deviceType: 0x99, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };
/* readback words; links: bit p = pair p linked */
const rb = (links = 0) => {
  const w = new Array(40).fill(0);
  for (let p = 0; p < 4; p++) if (links & (1 << p)) w[p < 2 ? 0 : 5] |= 1 << (6 * ((p * 2) & 3) + 2);
  w[2] = 140; w[6] = 0xC0000020; w[38] = 0x00010203;
  return w.map(x => x >>> 0);
};

const HOST = { driver: true, on: true, caller: true, stereo: true, staged: true, callerRule: true, why: 'enabled', admin: '' };
const DSP = { index: 1, room: 15390, total: 16384, largest: 15390, estimate: true, quarantined: false, owned: true, admitted: true, load: 1989, pools: [] };
const STRIPS = { live: true, started: 255, warm: false, state: 4 };
const MAPS = { ok: true, dir: 'C:\\maps', configured: '', why: '', python: true, catalog: 'ready', progress: [10, 10] };
const MODEL = { 0: { model: 'x8p', inputs: 8, pairs: [0, 2, 4, 6], hiz: [0, 1], supported: true }, 1: { model: 'twinx', inputs: 2, pairs: [0], hiz: [0], supported: true } };
const state = (inserts = [], over = {}, unit = 0) => ({ type: 'plugins', unit, present: true, unitId: X8P.id, ...MODEL[unit], ...(over.unit || {}), hosting: { ...HOST, ...(over.hosting || {}) },
  dsp: { ...DSP, ...(over.dsp || {}) }, strips: { ...STRIPS, ...(over.strips || {}) }, maps: { ...MAPS, ...(over.maps || {}) }, inserts });
/* a 1x1 PNG: a preview that loads */
const PNG = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==';
const ITEMS = [
  { name: 'UAD Precision De-Esser', label: 'Precision De-Esser', mono: true, stereo: true, latency: 0, codeWords: 2004, dataWords: 310, renderId: 2105, pages: 1, params: 7, licensed: true,
    category: 'Special Processing', tags: ['Special Processing', 'Dynamics'], art: PNG },
  { name: 'UAD Lexicon 480L', label: 'Lexicon 480L', mono: true, stereo: true, latency: 64, codeWords: 15066, pages: 64, params: 30, licensed: true,
    category: 'Reverb and Room', tags: ['Modulation', 'Reverb & Room', 'Delay'], art: 'file:///nowhere/lexicon.png' },
  { name: 'UAD UA 1176LN Legacy', label: 'UA 1176LN Legacy', mono: true, stereo: true, latency: 0, codeWords: 628, pages: 1, params: 7, licensed: true },
  { name: 'UAD Oxford SuprEsser DS', label: 'Oxford SuprEsser DS', mono: true, stereo: true, latency: 178, codeWords: 5000, pages: 1, params: 20, licensed: false,
    category: 'Special Processing', tags: ['Dynamics', 'Special Processing'], art: '' },
  { name: 'UAD Mono Only', label: 'Mono Only', mono: true, stereo: false, latency: 0, codeWords: 900, pages: 1, params: 2, licensed: true,
    category: 'Equalizers', tags: ['Equalization'], art: '' },
];
const LIST = { type: 'plugin-list', unit: 0, state: 'ok', why: '', licence: 'ok', licenceWhy: '', mapped: 5, authorized: 4, items: ITEMS };
const DEESSER = (over = {}) => ({ input: 0, stereo: false, bypass: false, name: 'UAD Precision De-Esser', label: 'Precision De-Esser', state: 'running',
  code: '', why: '', busy: '', latency: 0, codeWords: 2004, pages: 1,
  params: [{ name: 'Thresh', max: 32768, value: 0, text: '0.00 dB', steps: null }, { name: 'Speed', max: 1, value: 1, text: 'Fast', steps: ['Slow', 'Fast'] },
    { name: 'Freq', max: 32768, value: 10531, text: '6.50 k', steps: null }], ...over });

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const watch = p => {
    p.on('pageerror', e => errors.push(e.message));
    p.on('console', m => { if (m.type() === 'error' && !/nowhere\/lexicon|ERR_FILE_NOT_FOUND/.test(m.text())) errors.push(m.text()); });
  };
  const stub = () => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  };

  /* ---------------- A. Host path ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1536, height: 900 } });
  await ctx.addInitScript(stub);
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  const plugSent = n => page.evaluate(k => window.__sent.slice(k).filter(s => s.startsWith('{"cmd":"plug-')).map(s => JSON.parse(s)), n);
  const plb = page.locator('#plb');
  const notes = () => plb.locator('.plb-notes .ctl-note').allTextContents().then(t => t.join(' | '));
  const slot = (ch, unit = 0) => page.locator(`#inputs .ins-slot[data-unit="${unit}"][data-ch="${ch}"]`);
  const item = name => plb.locator(`.plb-item[data-name="${name}"]`);
  const drawer = page.locator('#plDrawer');
  const drops = () => page.evaluate(() => [...document.querySelectorAll('#inputs .ins-slot')].map(b => `${b.dataset.unit}:${b.dataset.ch}=${b.dataset.drop || '-'}:${b.querySelector('.ins-why').textContent}`));
  /* an HTML5 drag with the mouse, looking at the slots halfway */
  const dragTo = async (from, to, mid) => {
    await from.scrollIntoViewIfNeeded();
    const a = await from.boundingBox();
    await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
    await page.mouse.down();
    await page.mouse.move(a.x + a.width / 2 + 10, a.y + a.height / 2 + 4, { steps: 3 });
    await to.scrollIntoViewIfNeeded().catch(() => {});
    const b = await to.boundingBox();
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2, { steps: 6 });
    const seen = mid ? await mid() : null;
    await page.mouse.up();
    await page.waitForTimeout(80);
    return seen;
  };

  let n = await mark();
  /* [Classic] A and D drive the docked browser with the mouse (drags from it, onto it), so they run in Studio, whose browser
     is docked; Classic (the default) opens its browser from an insert instead (looks.js 5i) */
  await host({ type: 'hello', app: 'test', config: DOCKED, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: rb(0) });
  let s = await plugSent(n);
  check(s.some(x => x.cmd === 'plug-state' && x.units === true), "hello: plug-state with units:true (the page reads every unit's plug-in messages)");
  check(['plug-state', 'plug-watch', 'plug-list'].every(c => s.some(x => x.cmd === c)), `the Control view asks for the inserts, the watch and the list (${[...new Set(s.map(x => x.cmd))].join(', ')})`);
  check(await page.locator('#tab-plugins').count() === 0 && await plb.isVisible(), 'no Plug-ins tab: the browser is docked on the Control view');
  await host(state([]));
  check(await page.locator('#inputs .useg[data-unit="0"] .ins-slot').count() === 8 && (await slot(0).textContent()) === 'INSERT',
    'an INSERT slot on each of the x8p\'s 8 strips');
  check(await slot(0, 1).getAttribute('data-state') === 'na' && /not supported on this driver build/.test(await slot(0, 1).getAttribute('aria-label')),
    'the Twin X\'s slots: not supported on this driver build (no plug-in state for it from the host)');
  check(/15,390 w free/.test(await plb.locator('.plb-room').textContent()) && /15,390 w free/.test(await page.locator('.useg[data-unit="0"] .dsp-meter').textContent()),
    'DSP 1 room in the browser and the x8p\'s segment head');

  // 1. Categories from the host's UA data (name rules otherwise), counts, previews, search, FITS.
  await host(LIST);
  const groups = await plb.locator('.plb-group').evaluateAll(g => g.map(x => `${x.dataset.group}:${[...x.querySelectorAll('.plb-item')].map(i => i.dataset.name.replace('UAD ', '')).join('+')}`));
  check(groups.join(' ') === 'eq:Mono Only dyn:Precision De-Esser+UA 1176LN Legacy+Oxford SuprEsser DS verb:Lexicon 480L',
    `grouped by type: the De-Esser by its UA Dynamics tag, the 1176 by name, the 480L by UA's Reverb and Room (${groups.join(' ')})`);
  check(await plb.locator('.plb-group[data-group="dyn"] .plb-group-n').textContent() === '3' && await plb.locator('.plb-count').textContent() === '4/5',
    'counts per group, and 4 of 5 licensed');
  check(await item('UAD Oxford SuprEsser DS').getAttribute('data-lic') === '0' && /NO LIC/.test(await item('UAD Oxford SuprEsser DS').textContent()),
    'an unlicensed plug-in is listed, marked NO LIC');
  await page.waitForTimeout(200);
  check(await item('UAD Precision De-Esser').locator('.pl-prev').getAttribute('data-art') === 'ua' &&
    await item('UAD Lexicon 480L').locator('.pl-prev').getAttribute('data-art') === 'none' && await item('UAD Lexicon 480L').locator('svg.pl-glyph').count() === 1 &&
    await item('UAD Mono Only').locator('svg.pl-glyph').count() === 1, 'previews: the UAD install\'s art where it loads, the type glyph where it does not or there is none');
  await page.getByRole('searchbox', { name: 'Search plug-ins' }).fill('1176');
  check((await plb.locator('.plb-item').count()) === 1, 'search narrows the list');
  await page.getByRole('searchbox', { name: 'Search plug-ins' }).fill('');
  await host(state([], { dsp: { room: 9000, largest: 9000, estimate: false } }));
  await plb.locator('.plb-fits').click();
  const fit = await plb.locator('.plb-item').evaluateAll(x => x.map(i => i.dataset.name.replace('UAD ', '')));
  check(fit.join() === 'Mono Only,Precision De-Esser,UA 1176LN Legacy', `FITS DSP1: licensed and fits 9,000 words (${fit.join()})`);
  await plb.locator('.plb-fits').click();
  await item('UAD Precision De-Esser').focus();
  const card = page.locator('#plCard');
  const ct = await card.textContent();
  check(await card.isVisible() && /DYNAMICS/.test(ct) && /UA: Special Processing · Dynamics/.test(ct) && /2,004 w/.test(ct) && /9,000 of 16,384 w free → 6,996 after/.test(ct) &&
    /0 smp · 0\.000 ms @ 48 kHz/.test(ct) && /7/.test(ct) && /mono · stereo/.test(ct) && /1 page/.test(ct) && /licensed on this x8p/.test(ct) && /0x0839/.test(ct),
  'detail card on focus: type, UA\'s own category, DSP code, room against the free words, latency, parameters, I/O, startup, licence, render id');
  await item('UAD Lexicon 480L').focus();
  check(/NO ROOM/.test(await card.textContent()) && /64 pages, staged/.test(await card.textContent()), 'the 480L\'s card: no room, a staged 64-page startup');
  await page.keyboard.press('Escape');

  // 2. Drag and drop with the mouse: the slots say yes or why not while dragging; the drop loads it.
  const mid = await dragTo(item('UAD Lexicon 480L'), slot(1), drops);
  check(mid && mid.filter(x => x.startsWith('0:')).every(x => /=no:NO ROOM/.test(x)) && mid.filter(x => x.startsWith('1:')).every(x => /=no:NOT ON THIS BUILD/.test(x)),
    `dragging the 480L: every x8p slot says NO ROOM, the Twin X's NOT ON THIS BUILD (${mid && mid.slice(0, 2).join(' ')} ... ${mid && mid.slice(-1)})`);
  n = await mark();
  const mid2 = await dragTo(item('UAD Precision De-Esser'), slot(0), drops);
  s = await plugSent(n);
  check(mid2 && mid2.filter(x => x.startsWith('0:')).every(x => /=ok:DROP/.test(x)), 'dragging the De-Esser: every x8p slot lit (DROP)');
  check(s.length === 1 && s[0].cmd === 'plug-insert' && s[0].unit === 0 && s[0].input === 0 && s[0].name === 'UAD Precision De-Esser' && s[0].stereo === false,
    `the drop on input 1 sends plug-insert unit 0 input 0, mono (${JSON.stringify(s)})`);
  check(await page.locator('#inputs .ins-slot[data-drop]').count() === 0, 'after the drop the slots are plain again');

  // 3. The host loads it: the slot, its state light, the drawer with the plug-in's own texts.
  await host(state([DEESSER({ state: 'loading', busy: 'Loading' })], { dsp: { room: 6996, largest: 6996 } }));
  check(await slot(0).getAttribute('data-state') === 'loading' && /Loading/.test(await slot(0).getAttribute('aria-label')), 'while it loads: the slot says Loading');
  await host(state([DEESSER()], { dsp: { room: 6996, largest: 6996, estimate: false } }));
  await host({ type: 'plugin-result', unit: 0, op: 'insert', input: 0, ok: true, code: '', detail: '' });
  check((await slot(0).textContent()) === 'Precision De-Esser' && await slot(0).getAttribute('data-state') === 'running', 'running: the slot names it, its light on');
  await slot(0).click();
  check(await drawer.isVisible() && /Precision De-Esser/.test(await drawer.locator('.pl-dr-title').textContent()) &&
    await drawer.locator('.pl-state').textContent() === 'Running' && /IN 1 · INS 1 · DSP1/.test(await drawer.locator('.pl-dr-where').textContent()),
  'a click on the slot opens its parameters in place: name, Running, where it is');
  const thresh = page.getByRole('slider', { name: 'Precision De-Esser Thresh', exact: true });
  check(await thresh.getAttribute('aria-valuetext') === '0.00 dB' && await drawer.locator('.pl-param').first().getAttribute('data-rb') === 'ok' &&
    /RB OK/.test(await drawer.locator('.pl-rb').first().textContent()), 'a knob shows the plug-in\'s own text; the control read back (RB OK)');
  const speed = page.getByRole('group', { name: 'Precision De-Esser Speed' });
  check(await speed.getByRole('button', { name: 'Fast' }).getAttribute('aria-pressed') === 'true', 'a stepped parameter: Slow / Fast buttons, Fast on');

  // 4. Parameters: pending until the host answers, then its texts; numeric entry; a refusal: NOT CONFIRMED and a note.
  n = await mark();
  await thresh.focus();
  await page.keyboard.press('PageUp');
  await page.waitForTimeout(250);
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-param' && s[0].unit === 0 && s[0].input === 0 && s[0].index === 0 && s[0].value === 3277, `PageUp sends plug-param index 0 value 3277 (${JSON.stringify(s)})`);
  check(await drawer.locator('.pl-param').first().getAttribute('data-rb') === 'sent', 'the parameter waits for the host (SENT)');
  const changed = DEESSER();
  changed.params[0] = { ...changed.params[0], value: 3277, text: '-4.00 dB' };
  await host(state([changed], { dsp: { room: 6996, largest: 6996, estimate: false } }));
  await host({ type: 'plugin-result', unit: 0, op: 'param', input: 0, ok: true, code: '', detail: '' });
  check(await thresh.getAttribute('aria-valuetext') === '-4.00 dB' && await drawer.locator('.pl-param').first().getAttribute('data-rb') === 'ok', 'accepted: the plug-in\'s new text, RB OK');
  n = await mark();
  await page.getByRole('textbox', { name: 'Precision De-Esser Freq in percent' }).fill('50');
  await page.getByRole('textbox', { name: 'Precision De-Esser Freq in percent' }).press('Enter');
  await page.waitForTimeout(250);
  s = await plugSent(n);
  check(s.length === 1 && s[0].index === 2 && s[0].value === 16384, `numeric entry: 50 % sends 16384 (${JSON.stringify(s)})`);
  await host({ type: 'plugin-result', unit: 0, op: 'param', input: 0, ok: false, code: 'readback', detail: 'not changed: row 10 word 18 is not as written' });
  await host(state([changed], { dsp: { room: 6996, largest: 6996, estimate: false } }));
  check(/Input 1: parameter not changed\. not changed: row 10 word 18/.test(await notes()) && await drawer.locator('.pl-param').nth(2).getAttribute('data-rb') === 'refused' &&
    /NOT CONFIRMED/.test(await drawer.locator('.pl-param').nth(2).textContent()), 'a change the DSP did not confirm: a note, NOT CONFIRMED on that control');
  await plb.locator('.plb-notes').getByRole('button', { name: 'Dismiss' }).click();

  // 5. Bypass (the drawer, and B on the slot).
  n = await mark();
  await drawer.getByRole('button', { name: 'BYPASS' }).click();
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-bypass' && s[0].on === true, 'BYPASS sends plug-bypass on');
  await host(state([{ ...changed, bypass: true, state: 'bypassed' }]));
  check(await drawer.getByRole('button', { name: 'BYPASS' }).getAttribute('aria-pressed') === 'true' && await slot(0).getAttribute('data-state') === 'bypassed',
    'bypassed: lit in the drawer, the slot\'s light hollow');
  n = await mark();
  await slot(0).focus();
  await page.keyboard.press('b');
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-bypass' && s[0].on === false, 'B on the slot: bypass off');
  await host(state([changed]));

  // 6. Keyboard: Enter picks a plug-in up, focus goes to the first slot that takes it, Enter places it; Esc cancels.
  await item('UAD UA 1176LN Legacy').focus();
  await page.keyboard.press('Enter');
  const focused = await page.evaluate(() => { const b = document.activeElement; return `${b.className} ${b.dataset.unit}:${b.dataset.ch} ${b.dataset.drop}`; });
  check(/ins-slot .*0:1 ok/.test(focused) && await page.locator('#placeBar').isVisible() && /PLACING UA 1176LN Legacy/.test(await page.locator('#placeBar').textContent()),
    `Enter on a plug-in: picked up, focus on the first slot that takes it (${focused}), the bar says so`);
  check(await slot(0).getAttribute('data-drop') === 'ok' && /REPLACE/.test(await slot(0).locator('.ins-why').textContent()), 'a loaded slot offers to replace');
  await page.keyboard.press('Escape');
  check(await page.locator('#placeBar').isHidden() && await page.locator('#inputs .ins-slot[data-drop]').count() === 0, 'Esc cancels');
  await item('UAD UA 1176LN Legacy').focus();
  await page.keyboard.press('Enter');
  n = await mark();
  await page.keyboard.press('Tab');
  await page.keyboard.press('Shift+Tab');
  await page.keyboard.press('Enter');
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-insert' && s[0].input === 1 && s[0].name === 'UAD UA 1176LN Legacy', `Enter on the lit slot places it: input 2 (${JSON.stringify(s)})`);
  // not licensed, picked by keyboard: every slot says why
  await item('UAD Oxford SuprEsser DS').focus();
  await page.keyboard.press('Enter');
  check((await drops()).filter(x => x.startsWith('0:')).every(x => /=no:NOT LICENSED/.test(x)), 'an unlicensed plug-in: every slot says NOT LICENSED');
  await page.keyboard.press('Escape');

  // 7. Move by dragging a loaded slot: the new insert starts, takes the settings, then the old one goes. Unload onto the browser.
  n = await mark();
  await dragTo(slot(0), slot(2));
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-insert' && s[0].input === 2 && s[0].name === 'UAD Precision De-Esser', `drag input 1's insert to input 3: plug-insert input 2 (${JSON.stringify(s)})`);
  n = await mark();
  await host(state([changed, DEESSER({ input: 2 })]));
  s = await plugSent(n);
  check(s.length === 2 && s[0].cmd === 'plug-param' && s[0].input === 2 && s[0].index === 0 && s[0].value === 3277 && s[1].cmd === 'plug-remove' && s[1].input === 0,
    `it runs: the moved settings follow (Thresh), then input 1 goes back to the plain strip (${JSON.stringify(s)})`);
  await host(state([DEESSER({ input: 2, params: changed.params })]));
  n = await mark();
  await dragTo(slot(2), plb.locator('.plb-list'));
  await page.locator('#dlg').getByRole('button', { name: 'Remove' }).click();
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-remove' && s[0].input === 2, 'dragged back onto the browser: asks, then plug-remove');
  await host(state([DEESSER({ input: 4 })]));
  n = await mark();
  await page.locator('#inputs .ins-x[aria-label^="Remove Precision De-Esser"]').click();
  await page.locator('#dlg').getByRole('button', { name: 'Remove' }).click();
  s = await plugSent(n);
  check(s.length === 1 && s[0].cmd === 'plug-remove' && s[0].input === 4, 'the slot\'s x: asks, then plug-remove');
  await host(state([]));
  check(await drawer.isHidden(), 'the drawer closes when its insert is gone');

  // 8. Refusals: inline in the browser, the slot marked; host blocks shown on every slot while placing.
  await host({ type: 'plugin-result', unit: 0, op: 'insert', input: 0, ok: false, code: 'licence', detail: 'not licensed on this x8p (a demo not started): new licences come through UA\'s own software' });
  check(/Input 1: not inserted\. not licensed on this x8p/.test(await notes()) && await slot(0).getAttribute('data-refused') === '1', 'not licensed (host): a note, the slot marked');
  await host({ type: 'plugin-result', unit: 0, op: 'insert', input: 2, ok: false, code: 'room', detail: 'no room on DSP 1 for this plug-in next to the strips and the other inserts' });
  check(/Input 3: not inserted\. no room on DSP 1/.test(await notes()), 'no room (host): a note');
  await host(state([], { strips: { warm: true, live: false }, dsp: { owned: false } }));
  check(/Needs an x8p power cycle/.test(await notes()), 'warm start: "Needs an x8p power cycle"');
  await item('UAD Mono Only').focus();
  await page.keyboard.press('Enter');
  check((await drops()).filter(x => x.startsWith('0:')).every(x => /=no:POWER CYCLE/.test(x)), 'placing after a warm start: every slot says POWER CYCLE');
  await page.keyboard.press('Escape');
  // [Warm] the driver owns DSP 1 after a warm adoption: nothing blocks; a refused one shows the driver's reason
  await host(state([], { strips: { warm: true, live: true }, dsp: { owned: true, warm: true, proven: false, ownWhy: '' } }));
  check(!/power cycle|power-cycle/i.test(await notes()), 'warm start, strips adopted (the driver owns DSP 1): no power-cycle note');
  await host(state([], { strips: { warm: true, live: true }, dsp: { owned: false, ownWhy: 'plug-ins after a warm start are off for this unit (StripWarmPlugins = 0): power-cycle the x8p (off, 10 s, on): its saved inserts come back after it' } }));
  check(/StripWarmPlugins = 0\): power-cycle the x8p/.test(await notes()), 'warm start refused by the driver: its reason and the power cycle');
  await host(state([], { dsp: { quarantined: true } }));
  check(/DSP 1 is quarantined/.test(await notes()), 'quarantine: power-cycle note');
  await host(state([], { hosting: { on: false, caller: false } }));
  check(/Plug-in hosting is off/.test(await notes()) && await plb.locator('.plb-chip').textContent() === 'HOSTING OFF', 'hosting off: note and chip');
  await host(state([], { hosting: { on: true, caller: false } }));
  check(/another program/.test(await notes()) && await plb.locator('.plb-chip').textContent() === 'OTHER PROGRAM', 'the driver names another program: note and chip');

  // 9. Saved inserts coming back: waiting on a warm start (Try again), then running after the power cycle.
  const saved = DEESSER({ input: 2, state: 'waiting', code: 'warm', why: 'needs an x8p power cycle: after a warm start no DSP is ours to place on' });
  await host(state([saved], { strips: { warm: true, live: false }, dsp: { owned: false } }));
  await slot(2).click();
  check(await slot(2).getAttribute('data-state') === 'waiting' && /Needs a power cycle: needs an x8p power cycle/.test(await drawer.locator('.ctl-note').first().textContent()) &&
    await drawer.getByRole('button', { name: 'Try again' }).isVisible(), 'a saved insert after a warm start: waiting, "Needs a power cycle", Try again');
  await host(state([{ ...saved, state: 'running', code: '', why: '' }]));
  check(await slot(2).getAttribute('data-state') === 'running' && (await slot(2).textContent()) === 'Precision De-Esser', 'restored after the power cycle: running');
  await page.keyboard.press('Escape');

  // 10. Stereo on a linked pair; a mono-only plug-in there says why.
  await host({ type: 'readback', unit: 0, valid: true, seq: 2, words: rb(0x2) });
  await host(state([]));
  await item('UAD Mono Only').focus();
  await page.keyboard.press('Enter');
  check(/=no:MONO ONLY/.test((await drops()).find(x => x.startsWith('0:3=')) || ''), 'inputs 3-4 linked: a mono-only plug-in there says MONO ONLY');
  await page.keyboard.press('Escape');
  n = await mark();
  await dragTo(item('UAD UA 1176LN Legacy'), slot(3));
  s = await plugSent(n);
  check(s.length === 1 && s[0].input === 2 && s[0].stereo === true, `dropped on input 4 of the linked pair: one stereo insert from input 3 (${JSON.stringify(s)})`);
  await host(state([DEESSER({ input: 2, stereo: true, name: 'UAD UA 1176LN Legacy', label: 'UA 1176LN Legacy' })]));
  check((await slot(2).textContent()) === 'UA 1176LN Legacy L' && (await slot(3).textContent()) === 'UA 1176LN Legacy R', 'the pair\'s slots: L and R');
  await page.screenshot({ path: path.join(out, 'ui-plugins-control.png') });

  // 11. A second unit: supported false (an apollo.sys that hosts on the x8p only) keeps its slots shut; supported, they open,
  //     checked against its own list; sends carry the unit.
  await host(state([], { unit: { supported: false } }, 1));
  check(await slot(0, 1).getAttribute('data-state') === 'na' && /x8p only/.test(await slot(0, 1).getAttribute('title')), 'Twin X supported:false: not on this driver build');
  await host(state([], {}, 1));
  await host({ ...LIST, unit: 1, items: ITEMS.map(i => ({ ...i, licensed: i.name !== 'UAD UA 1176LN Legacy' })) });
  check(await slot(0, 1).getAttribute('data-state') === 'empty' && /Twin X input 1 insert: empty/.test(await slot(0, 1).getAttribute('aria-label')), 'Twin X hosting: its slots open');
  n = await mark();
  await dragTo(item('UAD Precision De-Esser'), slot(1, 1));
  s = await plugSent(n);
  check(s.length === 1 && s[0].unit === 1 && s[0].input === 1, `a drop on the Twin X's input 2: plug-insert unit 1 (${JSON.stringify(s)})`);
  await slot(0, 1).click();
  check(/TWIN X/.test(await plb.locator('.plb-unit').textContent()) && await item('UAD UA 1176LN Legacy').getAttribute('data-lic') === '0',
    'picking for a Twin X slot: the browser lists the Twin X\'s own licences');
  await page.keyboard.press('Escape');

  // 12. Settings: plug-in hosting; leaving the Control view stops the watch.
  await host(state([], { hosting: { on: false, caller: false } }));
  await page.evaluate(() => showView('settings'));
  const set = page.locator('#plSettings');
  check(await set.isVisible() && /off/.test(await set.textContent()), 'Settings: a Plug-in hosting panel, off');
  n = await mark();
  await set.getByRole('button', { name: 'Turn on' }).click();
  s = await plugSent(n);
  check(s.some(x => x.cmd === 'plug-hosting' && x.on === true) && s.some(x => x.cmd === 'plug-unwatch') === false, 'Turn on sends plug-hosting on');
  n = await mark();
  await page.evaluate(() => { showView('control'); showView('status'); });
  check((await plugSent(n)).some(x => x.cmd === 'plug-unwatch'), 'leaving the Control view: plug-unwatch');
  await ctx.close();

  /* ---------------- B. The demo: invented plug-ins, nothing sent ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=plugins');
  check(await demo.evaluate(() => S.view) === 'control' && await demo.evaluate(() => document.activeElement.getAttribute('aria-label')) === 'Search plug-ins',
    'an old "plugins" view opens Control with the plug-in search focused');
  await demo.locator('#plb .plb-item[data-name="Demo Plate Reverb"]').focus();
  await demo.keyboard.press('Enter');
  await demo.keyboard.press('Enter');
  await demo.waitForTimeout(150);
  check((await demo.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="0"]').textContent()) === 'Demo Plate Reverb' &&
    /1,390 w free/.test(await demo.locator('#plb .plb-room').textContent()), 'demo: inserted by keyboard, DSP 1 room 1,390 words left');
  await demo.locator('#plb .plb-item[data-name="Demo EQ"]').focus();
  await demo.keyboard.press('Enter');
  check(/=no:NO ROOM/.test(await demo.evaluate(() => { const b = document.querySelector('#inputs .ins-slot[data-unit="0"][data-ch="1"][data-n="0"]'); return `=${b.dataset.drop}:${b.querySelector('.ins-why').textContent}`; })),
    'demo: no room for a second large plug-in');
  await demo.keyboard.press('Escape');
  await demo.screenshot({ path: path.join(out, 'ui-plugins-demo.png') });
  await demo.close();

  /* ---------------- C. 980 px ---------------- */
  const narrow = await browser.newPage({ viewport: { width: 980, height: 760 } });
  watch(narrow);
  await narrow.goto(ui + '?view=control');
  await narrow.locator('#plb .plb-item[data-name="Demo Compressor"]').focus();
  await narrow.keyboard.press('Enter');
  await narrow.keyboard.press('Enter');
  await narrow.waitForTimeout(100);
  await narrow.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="0"]').click();
  const over = await narrow.evaluate(() => {
    const bad = [];
    if (document.documentElement.scrollWidth > document.documentElement.clientWidth) bad.push('document');
    const v = document.getElementById('view-control').getBoundingClientRect();
    for (const n of document.querySelectorAll('#plb, #plDrawer, #plDrawer .pl-param, #plDrawer .pl-actions > *')) {
      const r = n.getBoundingClientRect();
      if (r.width && r.right > v.right + 1) bad.push(n.className || n.id);
    }
    return bad;
  });
  check(!over.length, `980 px: the browser and the drawer fit (${over.join(', ') || 'nothing past the edge'})`);
  await narrow.screenshot({ path: path.join(out, 'ui-plugins-980.png') });
  await narrow.close();

  /* ---------------- D. [Ins] chains, REC / MON, Unison, the aux buses (docs 1.9) through a stub host ---------------- */
  const cx = await browser.newContext({ viewport: { width: 1536, height: 900 } });
  await cx.addInitScript(stub);
  const pg = await cx.newPage();
  watch(pg);
  await pg.goto(ui);
  const h4 = m => pg.evaluate(x => onHostMessage(x), m);
  const at = () => pg.evaluate(() => window.__sent.length);
  const plugsSince = k => pg.evaluate(j => window.__sent.slice(j).filter(x => x.startsWith('{"cmd":"plug-')).map(x => JSON.parse(x)), k);
  const monSince = k => pg.evaluate(j => window.__sent.slice(j).filter(x => /^monitor\t/.test(x)), k);
  const row = (ch, k, unit = 0) => pg.locator(`#inputs .ins-slot[data-unit="${unit}"][data-ch="${ch}"][data-n="${k}"]`);
  const SL = (slot, name, over = {}) => ({ slot, name, label: name.replace(/^UAD /, ''), bypass: false, state: 'running', code: '', why: '', latency: 0, codeWords: 1000, pages: 1,
    params: [{ name: 'Gain', max: 32768, value: 100, text: '0 dB', steps: null }], ...over });
  const chain = (input, slots, over = {}) => ({ input, aux: null, stereo: false, mon: false, state: 'running', code: '', why: '', busy: '', latency: 0, recordDelay: 200,
    overloaded: false, running: true, load: { before: 1989, after: 4781 }, slots, unison: null, analog: false, ...over });
  const v19 = (chains, over = {}) => ({ ...state([], over), slots: 4, recmon: true, auxBuses: 2, unison: true, auxInserts: false, chains });
  let m0 = await at();
  await h4({ type: 'hello', app: 'test', config: DOCKED, demo: false, view: 'control' });
  await h4({ type: 'status', present: true, info: INFO });
  await h4({ type: 'units', units: [X8P] });
  check((await monSince(m0)).filter(x => /^monitor\taux(ret|mute):/.test(x)).join('|') ===
    'monitor\tauxmute:1\t0|monitor\tauxmute:2\t0|monitor\tauxret:1\t0|monitor\tauxret:2\t0', 'on connect: each aux\'s mute and return once (nothing reads them back)');
  await h4({ ...LIST });
  await h4(v19([chain(0, [SL(0, 'UAD Precision De-Esser'), SL(2, 'UAD UA 1176LN Legacy', { bypass: true, state: 'bypassed' })], { latency: 64, recordDelay: 136, overloaded: false, mon: false })]));
  check(await pg.locator('#inputs .useg[data-unit="0"] .strip[data-ch="0"] .ins-slots > .ins-row:not(.ins-uni-row)').count() === 4 &&
    /Precision De-Esser/.test(await row(0, 0).textContent()) && await row(0, 2).getAttribute('data-state') === 'bypassed', 'slots:4: four rows, each slot its own state');
  check(/Unison: no plug-in has a Unison map/.test(await pg.locator('#inputs .ins-uni[data-unit="0"][data-ch="0"]').getAttribute('title')) &&
    await pg.locator('#inputs .ins-uni[data-unit="0"][data-ch="0"]').getAttribute('aria-disabled') === 'true', 'the Unison slot: drawn disabled with its reason');
  const rm = pg.locator('#inputs .ins-recmon[data-unit="0"][data-ch="0"]');
  check(/Record delay 136 smp/.test(await rm.getAttribute('title')), 'REC / MON: the record delay in the tooltip');
  m0 = await at();
  await rm.getByRole('button', { name: 'MON' }).click();
  let ps = await plugsSince(m0);
  check(ps.length === 1 && ps[0].cmd === 'plug-recmon' && ps[0].input === 0 && ps[0].mon === true, `MON: plug-recmon input 0 mon true (${JSON.stringify(ps)})`);
  /* a pick into an empty row: plug-insert with its slot */
  m0 = await at();
  await row(0, 1).click();
  await plb.locator('.plb-item[data-name="UAD Mono Only"]').waitFor().catch(() => {});
  await pg.locator('#plb .plb-item[data-name="UAD Mono Only"]').dblclick();
  ps = await plugsSince(m0);
  check(ps.length === 1 && ps[0].cmd === 'plug-insert' && ps[0].input === 0 && ps[0].slot === 1 && ps[0].name === 'UAD Mono Only', `row 2 then a pick: plug-insert slot 1 (${JSON.stringify(ps)})`);
  /* keyboard: B bypasses slot 0, M + Enter on row 4 moves within the chain */
  m0 = await at();
  await row(0, 0).focus();
  await pg.keyboard.press('b');
  ps = await plugsSince(m0);
  check(ps.length === 1 && ps[0].cmd === 'plug-bypass' && ps[0].slot === 0 && ps[0].on === true, `B: plug-bypass slot 0 (${JSON.stringify(ps)})`);
  m0 = await at();
  await row(0, 0).focus();
  await pg.keyboard.press('m');
  await row(0, 3).focus();
  await pg.keyboard.press('Enter');
  ps = await plugsSince(m0);
  check(ps.length === 1 && ps[0].cmd === 'plug-move' && ps[0].input === 0 && ps[0].from === 0 && ps[0].to === 3, `a move between rows of one strip: plug-move 0 -> 3 (${JSON.stringify(ps)})`);
  m0 = await at();
  await row(0, 2).focus();
  await pg.keyboard.press('Delete');
  await pg.getByRole('button', { name: 'Remove', exact: true }).click();
  ps = await plugsSince(m0);
  check(ps.length === 1 && ps[0].cmd === 'plug-remove' && ps[0].slot === 2, `Delete: plug-remove slot 2 (${JSON.stringify(ps)})`);
  /* a refused build: its words inline under the strip */
  await h4({ type: 'plugin-result', unit: 0, op: 'insert', input: 0, aux: null, slot: 1, ok: false, code: 'cycles', detail: 'DSP 1 at 85 % load' });
  check(/DSP 1 at 85 % load/.test(await pg.locator('#inputs .strip[data-ch="0"] .ins-refusal').textContent()), 'cycles: the refusal inline under the strip');
  /* the aux buses: inserts off with the chain's why, a send from the strip, PRE / POST for every input, return and mute */
  await pg.evaluate(() => setAuxOpen(true));
  check(await pg.locator('#inputs .ins-slot[data-unit="0"][data-ch="100"][data-n="0"]').getAttribute('data-state') === 'off' &&
    /Aux inserts are off/.test(await pg.locator('#inputs .ins-slot[data-unit="0"][data-ch="100"][data-n="0"]').getAttribute('title')), 'aux inserts off: the aux slots disabled with the reason');
  m0 = await at();
  await pg.getByRole('slider', { name: 'Input 3 send to AUX 1' }).focus();
  await pg.keyboard.press('End');
  await pg.waitForTimeout(80);
  check((await monSince(m0)).includes('monitor\tauxsend:1:2\t24'), 'a strip\'s AUX 1 knob: auxsend:1:2');
  m0 = await at();
  await pg.getByRole('group', { name: 'AUX 2 sends: pre or post fader' }).getByRole('button', { name: 'POST' }).click();
  const post = await monSince(m0);
  check(post.length === 9 && post.every(x => /^monitor\tauxpost:2:\d+\t1$/.test(x)) && post.includes('monitor\tauxpost:2:10\t1'), `POST: auxpost for every input and the talkback (${post.length})`);
  m0 = await at();
  await pg.getByRole('switch', { name: 'AUX 1 return mute' }).click();
  await pg.getByRole('slider', { name: 'AUX 1 return level' }).focus();
  await pg.keyboard.press('Home');
  await pg.waitForTimeout(80);
  const ret = await monSince(m0);
  check(ret.includes('monitor\tauxmute:1\t1') && ret.includes('monitor\tauxret:1\t-121'), `the aux strip: auxmute, auxret (${JSON.stringify(ret)})`);
  check(JSON.stringify((await pg.evaluate(() => cfg.mixes.x8p.aux))[1]) === '{"post":true}' && (await pg.evaluate(() => cfg.mixes.x8p.aux[0].ret)) === -121,
    'kept in cfg.mixes (nothing reads back)');
  await cx.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
