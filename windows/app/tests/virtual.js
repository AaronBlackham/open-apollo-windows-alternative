// [Virtual] [AuxStrips] The VIRTUAL inputs and the aux returns' strips in Apollo Control (docs/apollo-control-units.md 1.13,
// docs/STRIPS.md 13-14): VIRTUAL strips only where the live strips' virtualMask has the bit (model range, after the Mic
// strips, pairs 1-2, 3-4 with a host-side LINK), their messages on the existing names with decimal buses 24-31 (mix, pan,
// M, S, cue sends, aux sends, PRE / POST), refusals named, kept through a restart report, replayed when they appear; the
// aux note where no return strip runs and the mono pan locked; Settings' Virtual inputs up to the host's params-set
// (pending, cancelled, stored and restarted, stored and not restarted); sessions and undo; the demo; every look.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: 'SN20000000000001', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '20000000000001', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 2, playChannels: 46,
  recChannels: 34, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };
const words = f => { const w = new Array(40).fill(0); f(w); w[38] = 0x00010203; return w.map(x => x >>> 0); };
const x8pRb = () => words(w => { w[2] = 140; w[6] = 0xC0000020; });
const twRb = () => words(w => { w[2] = 150 | (140 << 8); w[6] = 0xC0000011; });
/* a "live" as the host sends it: an older driver's strips report (version 2/3: no extra fields), or with "over" the
   version 4/5 fields (virtualMask, virtualAsked, virtualWhy, auxMask, auxAsked, auxWhy, auxLive) */
const live = (unit, over = {}) => ({ type: 'live', unit, engine: null, engineErr: 21, endpoints: null, endpointsErr: 21,
  strips: { version: 3, state: 4, flags: 0x79, enable: 1, mask: unit ? 3 : 0xFF, stepsLimit: 9, stepsDone: 9, phantomState: 1, phantomMask: 0, why: '',
    perInput: true, taken: [2, 2, 2, 2, 2, 2, 2, 2], started: [1, 1, 1, 1, 1, 1, 1, 1], ...over } });
const V5 = { version: 5, virtualMask: 0, virtualAsked: 0, virtualWhy: 0, auxMask: 5, auxAsked: 5, auxWhy: 0, auxLive: 3 };
const PARAMS = (id, stored, pending = false, value = stored) => ({ type: 'params', unit: id, present: true, key: id, resolved: true, entries: [
  { name: 'StripsEnable', value: 1, default: 1, stored: null, src: 'D', hex: false, pending: false },
  { name: 'StripVirtualMask', value, default: 0, stored, src: stored === null ? 'D' : 'U', hex: true, pending }] });
const wait = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const watch = p => {
    p.on('pageerror', e => errors.push(e.message));
    p.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  };
  const stub = () => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  };
  const open = async (viewport = { width: 1500, height: 900 }) => {
    const ctx = await browser.newContext({ viewport });
    await ctx.addInitScript(stub);
    const page = await ctx.newPage();
    watch(page);
    await page.goto(ui);
    return { ctx, page };
  };

  /* ---------------- A. The host path: strips per mask, messages, LINK, a restart, refusals ---------------- */
  let { ctx, page } = await open();
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  const mon = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^monitor\t/.test(s)), n);
  const sw = name => page.getByRole('switch', { name, exact: true });
  const checked = async loc => (await loc.getAttribute('aria-checked')) === 'true';
  const x8 = page.locator('.useg[data-unit="0"]'), tw = page.locator('.useg[data-unit="1"]');
  const editKnob = async (name, text) => {
    await page.getByRole('slider', { name, exact: true }).focus();
    await page.keyboard.press('Enter');
    await page.keyboard.type(text);
    await page.keyboard.press('Enter');
    await page.waitForTimeout(80);
  };
  const notes = () => page.locator('#ctlBar .ctl-note').allTextContents().then(t => t.join(' | '));

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: x8pRb() });
  await host({ type: 'readback', unit: 1, valid: true, seq: 1, words: twRb() });
  await page.evaluate(() => setAuxOpen(true));

  // 1. An older driver (no version 4/5 fields): no VIRTUAL strip; each aux says it carries no audio; the pans stay free.
  await host(live(0));
  await host(live(1));
  check(await page.locator('#inputs .vstrip').count() === 0, 'an older driver: no VIRTUAL strip anywhere');
  const x8Notes = x8.locator('.aux-note:visible');
  check(await x8Notes.count() === 2 && await tw.locator('.aux-note:visible').count() === 2 && /No audio/.test(await x8Notes.first().textContent()) &&
    /AUX 1 carries no audio on the x8p: the installed apollo\.sys builds no strips on the aux returns \(an older build\)/.test(await x8Notes.first().getAttribute('title')),
    `an older driver: a note on every aux, the reason in its tooltip ("${await x8Notes.first().getAttribute('title')}")`);
  check(await x8.locator('.aux-strip[data-aux="0"]').getAttribute('aria-describedby') === 'auxNote0_0' && await x8.locator('.aux-tag').first().textContent() === 'ST' &&
    await page.locator('[aria-label="Input 1 AUX 1 pan"]').getAttribute('aria-disabled') === null, 'the note describes the aux strip; tag ST; the send pans free');

  // 2. Version 5 on the x8p: VIRTUAL 1-2 built, both aux returns running.
  await host(live(0, { ...V5, virtualMask: 0x3, virtualAsked: 0x3 }));
  const vs = x8.locator('.vstrip');
  check(await vs.count() === 2 && (await vs.locator('.strip-name').allTextContents()).join() === 'VIRTUAL 1,VIRTUAL 2' &&
    (await vs.locator('.strip-num').allTextContents()).join() === 'V1,V2', 'virtualMask 0x3: VIRTUAL 1 and VIRTUAL 2 strips on the x8p');
  const order = await page.evaluate(() => {
    const b = document.querySelector('.useg[data-unit="0"] .useg-body'), kids = [...b.children];
    const vp = b.querySelector('.vpair'), pairs = kids.filter(k => k.classList.contains('pair') && !k.classList.contains('vpair'));
    return { afterMic: kids.indexOf(vp) > kids.indexOf(pairs[pairs.length - 1]), beforeTb: kids.indexOf(vp) < kids.indexOf(b.querySelector('.tb-strip')),
      head: vp.querySelector('.pair-name').textContent, link: !!vp.querySelector('.link-btn') };
  });
  check(order.afterMic && order.beforeTb && order.head === 'V1 - V2' && order.link, `after the Mic strips, before the talkback, as the pair V1 - V2 with LINK (${JSON.stringify(order)})`);
  check(await vs.first().locator('.st-pre, .st-ins').count() === 0 && await vs.first().getAttribute('data-ch') === null &&
    await page.getByRole('slider', { name: 'VIRTUAL 1 mix level', exact: true }).getAttribute('aria-valuetext') === '0.0 dB',
    'no preamp, no insert, no data-ch; an untouched VIRTUAL shows UA\'s default 0.0 dB');
  check(await x8.locator('.aux-note:visible').count() === 0 && (await x8.locator('.aux-tag').allTextContents()).join() === 'MONO,MONO' &&
    await x8.locator('.aux-strip[data-aux="0"]').getAttribute('data-audio') === 'live', 'auxLive 0x3: no note on the x8p\'s auxes, both tagged MONO');
  const pan1 = page.locator('[aria-label="Input 1 AUX 1 pan"]');
  check(await pan1.getAttribute('aria-disabled') === 'true' && /AUX 1 is mono on the x8p: a send's pan does nothing there/.test(await pan1.getAttribute('title')) &&
    await page.locator('[aria-label="VIRTUAL 2 AUX 2 pan"]').getAttribute('aria-disabled') === 'true', 'a mono aux: every send pan locked, the tooltip says why');
  let n = await mark();
  await page.evaluate(() => { const k = document.querySelector('[aria-label="Input 1 AUX 1 pan"]'); k.dispatchEvent(new KeyboardEvent('keydown', { key: 'PageUp', bubbles: true })); });
  await page.waitForTimeout(80);
  check(!(await mon(n)).some(s => /auxpan/.test(s)), 'a locked pan sends nothing');
  check(await tw.locator('.vstrip').count() === 0 && await tw.locator('.aux-note:visible').count() === 2, 'the Twin X (older report) keeps no VIRTUAL and its notes');
  check(/live, pass complete · stage 9 of 9 · VIRTUAL 1, 2 · AUX 1, 2 returns/.test(await page.evaluate(() => { showView('status'); const t = document.querySelector('.st-line').textContent; showView('control'); return t; })),
    'Status: the strip line names the VIRTUAL inputs and aux returns running');

  // 3. Their messages: mix level, pan, M, S, aux send, cue send on the strip (Classic), POST, all with decimal bus 24 / 25.
  n = await mark();
  await editKnob('VIRTUAL 1 mix level', '-6');
  await page.getByRole('slider', { name: 'VIRTUAL 1 mix pan', exact: true }).focus();
  await page.keyboard.press('PageDown');
  await page.waitForTimeout(100);
  await sw('VIRTUAL 2 mute in the monitor mix').click();
  await sw('VIRTUAL 1 solo in the monitor mix').click();
  let s = await mon(n);
  check(s.includes('monitor\tmix:24\t-12') && s.includes('monitor\tmixpan:24\t-25') && s.includes('monitor\tmixmute:25\t1') && s.includes('monitor\tmixsolo:24\t1'),
    `VIRTUAL 1 level -6 dB and pan L25, VIRTUAL 2 M, VIRTUAL 1 S: mix:24, mixpan:24, mixmute:25, mixsolo:24 (${JSON.stringify(s)})`);
  const silent = await x8.locator('.strip, .tb-strip').evaluateAll(r => r.map(x => (x.classList.contains('silent') ? 1 : 0)).join(''));
  check(silent === '11111111011', `VIRTUAL 1 soloed: every other channel of the x8p silenced, VIRTUAL 1 not (${silent})`);
  check(JSON.stringify(await page.evaluate(() => [cfg.mixes.x8p.main[24], cfg.mixes.x8p.main[25]])) === '[{"level":-12,"pan":-25,"solo":true},{"mute":true}]',
    'kept in cfg.mixes.x8p.main under 24 and 25');
  await page.waitForTimeout(300);                       /* Classic adds the strips' cue sends on its quarter-second tick */
  n = await mark();
  await sw('VIRTUAL 1 solo in the monitor mix').click();
  await editKnob('VIRTUAL 1 send to AUX 1', '-10');
  await editKnob('VIRTUAL 2 send to CUE 1', '-3');
  await x8.getByRole('group', { name: 'AUX 1 sends: pre or post fader' }).getByRole('button', { name: 'POST' }).click();
  s = await mon(n);
  const post = s.filter(x => /^monitor\tauxpost:1:/.test(x));
  check(s.includes('monitor\tmixsolo:24\t0') && s.includes('monitor\tauxsend:1:24\t-20') && s.includes('monitor\tsend:1:25\t-6'),
    `S off, VIRTUAL 1 into AUX 1, VIRTUAL 2 into CUE 1: auxsend:1:24, send:1:25 (${JSON.stringify(s.filter(x => !/auxpost/.test(x)))})`);
  check(post.length === 11 && post.includes('monitor\tauxpost:1:24\t1') && post.includes('monitor\tauxpost:1:25\t1') && post.includes('monitor\tauxpost:1:10\t1'),
    `POST: every input, the built VIRTUAL inputs and the talkback (${post.length})`);
  n = await mark();
  await page.getByRole('switch', { name: 'Mute every input and the talkback in the monitor mix', exact: true }).click();
  check((await mon(n)).join() === 'monitor\tmixmuteall\t1' && await checked(sw('VIRTUAL 1 mute in the monitor mix')), 'MUTE ALL: one mixmuteall, the VIRTUAL rows follow (the driver mutes them too)');
  await page.getByRole('switch', { name: 'Mute every input and the talkback in the monitor mix', exact: true }).click();

  // 4. The Cues page: VIRTUAL 1 and 2 between input 8 and the talkback, send and pan on bus 24 / 25.
  await page.evaluate(() => showView('cues'));
  const cueNums = await page.locator('#cues .cue-strip .strip-num').allTextContents();
  check(cueNums.join() === '1,2,3,4,5,6,7,8,V1,V2,TB' && /VIRTUAL 1/.test(await page.locator('#cues .cue-strip[data-bus="24"] .strip-name').textContent()),
    `Cues: V1, V2 after input 8, before TB (${cueNums.join()})`);
  n = await mark();
  await page.getByRole('slider', { name: 'VIRTUAL 2 pan', exact: true }).focus();
  await page.keyboard.press('PageUp');
  await page.waitForTimeout(100);
  check((await mon(n)).includes('monitor\tpan:1:25\t25'), 'Cues: VIRTUAL 2 pan into CUE 1: pan:1:25');
  await page.evaluate(() => showView('control'));

  // 5. LINK V1 - V2 (here only): pans hard left / right, VIRTUAL 1's level and mute on both, V2 a follower; unlink back.
  n = await mark();
  await x8.locator('.vpair').getByRole('button', { name: 'Link VIRTUAL 1 and 2' }).click();
  await page.waitForTimeout(100);
  s = await mon(n);
  check(s.includes('monitor\tmixpan:24\t-100') && s.includes('monitor\tmixpan:25\t100') && s.includes('monitor\tmix:25\t-12') && s.includes('monitor\tmixsolo:25\t0'),
    `LINK: pans L100 / R100, VIRTUAL 1's -6 dB and solo state on VIRTUAL 2 (${JSON.stringify(s)})`);
  check((await x8.locator('.vpair').getAttribute('class')).includes('linked') && (await vs.nth(1).getAttribute('class')).includes('follower') &&
    await page.getByRole('slider', { name: 'VIRTUAL 2 mix level', exact: true }).count() === 0 &&
    JSON.stringify(await page.evaluate(() => cfg.mixes.x8p.vlinks)) === '[true,false,false,false]', 'linked: VIRTUAL 1 drives the pair, kept as vlinks');
  n = await mark();
  await editKnob('VIRTUAL 1 mix level', '-3');
  s = await mon(n);
  check(s.includes('monitor\tmix:24\t-6') && s.includes('monitor\tmix:25\t-6'), 'the pair\'s fader moves both buses');
  n = await mark();
  await x8.locator('.vpair').getByRole('button', { name: 'Link VIRTUAL 1 and 2' }).click();
  s = await mon(n);
  check(s.includes('monitor\tmixpan:24\t0') && s.includes('monitor\tmixpan:25\t0') && await page.evaluate(() => cfg.mixes.x8p.vlinks === undefined),
    'unlink: both pans back to the centre, nothing kept');

  // 6. The Twin X: only its own range (VIRTUAL 1-4), tagged with its slot; one aux running.
  await host(live(1, { ...V5, mask: 3, virtualMask: 0x31, virtualAsked: 0x31, auxMask: 1, auxAsked: 1, auxLive: 1 }));
  check(await tw.locator('.vstrip').count() === 1 && await tw.locator('.vpair.single').count() === 1 && await tw.locator('.vpair .link-btn').count() === 0,
    'Twin X virtualMask 0x31: VIRTUAL 1 alone (bits past VIRTUAL 4 are not the Twin X\'s), no LINK');
  n = await mark();
  await editKnob('Twin X VIRTUAL 1 mix level', '-12');
  check((await mon(n)).includes('monitor\tmix:24\t-24\t1'), 'Twin X VIRTUAL 1: mix:24 tagged with its slot');
  const twNote = tw.locator('.aux-note:visible');
  check(await twNote.count() === 1 && /AUX 2 carries no audio on the Twin X: its StripAuxMask leaves the AUX 2 return without a strip/.test(await twNote.getAttribute('title')) &&
    (await tw.locator('.aux-tag').allTextContents()).join() === 'MONO,ST', `Twin X AUX 2 not asked: its note says why; AUX 1 MONO ("${await twNote.getAttribute('title')}")`);

  // 7. A restart: while the strips report nothing the strips stay; the new mask rebuilds the segment.
  await host({ type: 'live', unit: 0, engine: null, engineErr: 21, strips: null, stripsErr: 21, endpoints: null, endpointsErr: 21 });
  check(await vs.count() === 2, 'no strips report (the unit restarting): the VIRTUAL strips stay');
  await host(live(0, { ...V5, virtualMask: 0x1, virtualAsked: 0x1 }));
  check(await vs.count() === 1 && await x8.locator('.vpair.single').count() === 1 && await x8.locator('.vpair .link-btn').count() === 0, 'virtualMask 0x1: VIRTUAL 1 alone, no LINK');
  n = await mark();
  await page.evaluate(() => { setMix(25, 'level', -20, 0); setSend(0, 25, 'level', -20, 0); });
  check((await mon(n)).length === 0, 'VIRTUAL 2 not built: nothing goes to its bus');
  check(await page.evaluate(() => cfg.mixes.x8p.main[25].level === -6), 'what was set for it is kept');

  // 8. A refusal names the VIRTUAL input and puts the control back.
  await host({ type: 'error', op: 'monitor', key: 'mixmute:24', index: 0x4218, code: 87, detail: 'The parameter is incorrect' });
  check(/VIRTUAL 1 monitor mix mute was not changed: the driver rejected the value\./.test(await notes()), `refusal: "${await notes()}"`);
  await page.screenshot({ path: path.join(out, 'ui-virtual-host.png') });
  await ctx.close();

  /* ---------------- B. The saved mix: replayed when the VIRTUAL inputs appear, never before ---------------- */
  ({ ctx, page } = await open({ width: 1240, height: 860 }));
  const hostB = m => page.evaluate(x => onHostMessage(x), m);
  const monB = () => page.evaluate(() => window.__sent.filter(s => /^monitor\t/.test(s)));
  const cfgB = { version: 1, mixes: { x8p: { main: { 0: { mute: true }, 24: { level: -10 }, 25: { mute: true } }, cues: [{ sends: { 24: { level: -20, pan: 0 } } }, {}, {}, {}],
    aux: [{ sends: { 24: { level: -8, pan: 0 } } }, {}], vlinks: [true] } } };
  await hostB({ type: 'hello', app: 'test', config: JSON.stringify(cfgB), demo: false, view: 'control' });
  await hostB({ type: 'status', present: true, info: INFO });
  let sb = await monB();
  check(sb.includes('monitor\tmixmute:0\t1') && !sb.some(x => /:(24|25)\t/.test(x)), 'connect before any strips report: the saved mix goes out, nothing for VIRTUAL');
  await hostB(live(0, { ...V5, virtualMask: 0x3, virtualAsked: 0x3 }));
  sb = (await monB()).slice(sb.length);
  check(sb.join('|') === 'monitor\tmixmute:25\t1|monitor\tmix:24\t-10|monitor\tauxsend:1:24\t-8|monitor\tsend:1:24\t-20',
    `VIRTUAL 1-2 appear: what is kept for them goes out, mutes first (${JSON.stringify(sb)})`);
  check(await page.evaluate(() => vlinkOf(0, 0) && (segs.get(0).vpairs[0].node.classList.contains('linked'))), 'the kept link shows (vlinks from config.json)');
  await ctx.close();

  /* ---------------- C. Settings, Virtual inputs: the draft, Apply, the host's answers ---------------- */
  ({ ctx, page } = await open({ width: 1400, height: 900 }));
  const hostC = m => page.evaluate(x => onHostMessage(x), m);
  const json = (n0, cmd) => page.evaluate(([k, c]) => window.__sent.slice(k).filter(s => s[0] === '{').map(s => JSON.parse(s)).filter(x => x.cmd === c), [n0, cmd]);
  const markC = () => page.evaluate(() => window.__sent.length);
  const panel = page.locator('#viPanel');
  const blk = id => panel.locator(`.vi-unitblock[data-unit="${id}"]`);
  const vbox = (id, n0) => blk(id).locator(`.vi-row[data-virt="${n0}"] input`);
  const status = id => blk(id).locator('.ep-status').textContent().catch(() => '');
  await hostC({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await hostC({ type: 'status', present: true, info: INFO });
  await hostC({ type: 'units', units: [X8P, TWX] });
  await hostC({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await hostC(live(0, { ...V5, virtualMask: 0x3, virtualAsked: 0x3 }));
  await hostC(live(1, { ...V5, mask: 3 }));
  let nc = await markC();
  await page.evaluate(() => showView('settings'));
  check((await json(nc, 'params-get')).length === 1, 'Settings asks every unit\'s parameters (params-get)');
  await hostC(PARAMS(X8P.id, 3));
  await hostC(PARAMS(TWX.id, null));
  check(await blk(X8P.id).locator('.vi-row').count() === 8 && await blk(TWX.id).locator('.vi-row').count() === 4, 'a box per VIRTUAL input: 8 on the x8p, 4 on the Twin X');
  check(await vbox(X8P.id, 1).isChecked() && await vbox(X8P.id, 2).isChecked() && !(await vbox(X8P.id, 3).isChecked()) &&
    /Running/.test(await blk(X8P.id).locator('.vi-row[data-virt="1"]').textContent()) && /VIRTUAL 1 and 2 running/.test(await blk(X8P.id).locator('.ep-head').textContent()),
    'checked from what the next start reads; VIRTUAL 1 and 2 running');
  const applyX = blk(X8P.id).getByRole('button', { name: 'Apply' });
  check(await applyX.isDisabled(), 'Apply waits for a change');
  await vbox(X8P.id, 3).check();
  check(!(await applyX.isDisabled()) && /Built at the restart/.test(await blk(X8P.id).locator('.vi-row[data-virt="3"]').textContent()), 'a draft: VIRTUAL 3 "built at the restart", Apply on');
  nc = await markC();
  await applyX.click();
  await page.locator('#dlg[open]').waitFor();
  const dlg = await page.locator('#dlg[open]').textContent();
  check(/Restart the x8p/.test(dlg) && /administrator rights/.test(dlg) && /audio on the x8p stops for a few seconds/.test(dlg) &&
    /Nothing plays through a VIRTUAL input until a DAW sends audio to it over ASIO: Windows has no playback device for them/.test(dlg),
    'Apply asks first: the restart, audio stopping for a few seconds, nothing plays until a DAW sends over ASIO');
  await page.keyboard.press('Escape');
  await page.waitForTimeout(100);
  check((await json(nc, 'params-set')).length === 0 && await vbox(X8P.id, 3).isChecked(), 'declined: nothing sent, the draft kept');
  await applyX.click();
  await page.locator('#dlg[open]').getByRole('button', { name: 'Restart the x8p', exact: true }).click();
  let set = await json(nc, 'params-set');
  check(set.length === 1 && JSON.stringify(set[0]) === JSON.stringify({ cmd: 'params-set', unit: X8P.id, name: 'StripVirtualMask', value: 7 }),
    `params-set with the unit's id, the name and the whole mask (${JSON.stringify(set)})`);
  await hostC({ type: 'params-result', unit: X8P.id, name: 'StripVirtualMask', value: 7, pending: true, code: '', detail: 'asking for administrator rights to store StripVirtualMask and restart the unit' });
  check(await vbox(X8P.id, 1).isDisabled() && /asking for administrator rights/.test(await status(X8P.id)), 'pending: boxes locked, the host\'s words');
  await hostC({ type: 'params-result', unit: X8P.id, name: 'StripVirtualMask', value: 7, pending: false, ok: false, stored: false, restarted: false, code: 'cancelled', detail: 'cancelled' });
  check(/administrator prompt was declined: nothing changed/.test(await status(X8P.id)) && await vbox(X8P.id, 3).isChecked() && !(await vbox(X8P.id, 3).isDisabled()),
    'cancelled: said so, the draft kept');
  nc = await markC();
  await applyX.click();
  await page.locator('#dlg[open]').getByRole('button', { name: 'Restart the x8p', exact: true }).click();
  await hostC({ type: 'params-result', unit: X8P.id, name: 'StripVirtualMask', value: 7, pending: false, ok: true, stored: true, restarted: true, code: '', detail: 'stored; the unit restarted' });
  check(/Stored\. The x8p restarts now/.test(await status(X8P.id)) && (await json(nc, 'params-get')).length === 1, 'stored and restarted: said so, parameters asked again');
  await hostC(PARAMS(X8P.id, 7));
  await hostC(live(0, { ...V5, virtualMask: 0x7, virtualAsked: 0x7 }));
  check(/Running/.test(await blk(X8P.id).locator('.vi-row[data-virt="3"]').textContent()) && await applyX.isDisabled() &&
    await page.evaluate(() => segs.get(0).virt.length) === 3, 'the unit back with VIRTUAL 3: running here, its strip on the Control view');
  // stored, but the restart was put off (something holds the unit open): the value waits for the next start
  await vbox(X8P.id, 3).uncheck();
  await applyX.click();
  await page.locator('#dlg[open]').getByRole('button', { name: 'Restart the x8p', exact: true }).click();
  await hostC({ type: 'params-result', unit: X8P.id, name: 'StripVirtualMask', value: 3, pending: false, ok: false, stored: true, restarted: false, code: 'reboot',
    detail: 'stored, but Windows could not restart the unit now: something holds it open (an ASIO host or an audio app). Close it and apply again, or power-cycle the unit' });
  check(/Stored, not running yet: stored, but Windows could not restart the unit now/.test(await status(X8P.id)), 'stored, not restarted: the host\'s reason');
  await hostC(PARAMS(X8P.id, 3, true, 7));
  await page.evaluate(() => { Virt.state.notes = {}; Virt.render(); });
  const again = blk(X8P.id).getByRole('button', { name: 'Restart the x8p' });
  check(/Stored for the x8p's next start: VIRTUAL 1 and 2\. Restart it to build them now\./.test(await status(X8P.id)) && await again.isVisible() &&
    !(await vbox(X8P.id, 3).isChecked()), 'the stored value differs from what runs: said so, with a Restart button');
  nc = await markC();
  await again.click();
  await page.locator('#dlg[open]').getByRole('button', { name: 'Restart the x8p', exact: true }).click();
  set = await json(nc, 'params-set');
  check(set.length === 1 && set[0].value === 3, 'Restart: the stored value again');
  await hostC({ type: 'params-result', unit: X8P.id, name: 'StripVirtualMask', value: 3, pending: false, ok: true, stored: true, restarted: true, code: '', detail: '' });
  // the Twin X: its own four, its own id
  nc = await markC();
  await vbox(TWX.id, 4).check();
  await blk(TWX.id).getByRole('button', { name: 'Apply' }).click();
  await page.locator('#dlg[open]').getByRole('button', { name: 'Restart the Twin X', exact: true }).click();
  set = await json(nc, 'params-set');
  check(set.length === 1 && set[0].unit === TWX.id && set[0].value === 8, `Twin X VIRTUAL 4: value 8 for its id (${JSON.stringify(set)})`);
  await hostC({ type: 'params-result', unit: TWX.id, name: 'StripVirtualMask', value: 8, pending: false, ok: false, stored: false, restarted: false, code: 'value', detail: 'refused' });
  check(/The host refused these inputs for this model\./.test(await status(TWX.id)), 'a refusal: said in the unit\'s block');
  // an older driver, and a read-only unit
  await hostC(live(1, { mask: 3 }));
  await page.evaluate(() => { Virt.state.draft = {}; Virt.render(); });
  check(/an older build/.test(await blk(TWX.id).textContent()) && await vbox(TWX.id, 1).isDisabled(), 'an older driver: said so, the boxes locked');
  await hostC({ type: 'units', units: [X8P, { ...TWX, mode: 0, observe: true, active: false }] });
  await hostC({ type: 'status', unit: 1, present: true, info: { ...TW_INFO, mode: 0, observe: true } });
  await page.evaluate(() => renderSettings());
  check(/observe mode: read-only here/.test(await blk(TWX.id).textContent()) && await blk(TWX.id).locator('input').count() === 0, 'a read-only unit: no boxes');
  await page.locator('#viPanel').screenshot({ path: path.join(out, 'ui-virtual-settings.png') });
  await ctx.close();

  /* ---------------- D. Sessions and undo: the VIRTUAL buses and links kept like a Mic input's ---------------- */
  ({ ctx, page } = await open({ width: 1240, height: 860 }));
  await page.evaluate(() => {
    const reply = m => setTimeout(() => onHostMessage(m), 5);
    window.__files = {};
    const post = window.chrome.webview.postMessage;
    window.chrome.webview.postMessage = s => {
      post(s);
      if (typeof s !== 'string' || s[0] !== '{') return;
      const m = JSON.parse(s);
      if (m.cmd === 'session-plugins') reply({ type: 'plugin-session', op: 'get', doc: { version: 2, mapsDir: '', chains: [] } });
      if (m.cmd === 'session-save') { window.__files[m.name] = m.session; reply({ type: 'session-result', op: 'save', ok: true, name: m.name, file: `${m.name}.json`, why: '' }); }
      if (m.cmd === 'session-load') {
        const x = m.last ? null : window.__files[m.name];
        reply(x ? { type: 'session', op: 'load', name: m.name, file: m.file, session: JSON.parse(JSON.stringify(x)) }
          : { type: 'session-result', op: m.last ? 'last' : 'load', ok: false, name: m.name || '', file: '', why: 'none' });
      }
    };
  });
  const hostD = m => page.evaluate(x => onHostMessage(x), m);
  const markD = () => page.evaluate(() => window.__sent.length);
  const monD = n0 => page.evaluate(k => window.__sent.slice(k).filter(s => /^monitor\t/.test(s)), n0);
  await hostD({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await hostD({ type: 'status', present: true, info: INFO });
  await hostD({ type: 'units', units: [X8P] });
  await hostD({ type: 'readback', unit: 0, valid: true, seq: 1, words: x8pRb() });
  await hostD(live(0, { ...V5, virtualMask: 0x3, virtualAsked: 0x3 }));
  await page.evaluate(() => { setMix(24, 'level', -8, 0); setVLink(0, 0, true); setSend(1, 25, 'level', -14, 0); Aux.setSend(0, 1, 24, 'level', -30); });
  await wait(150);
  check(await page.evaluate(() => Sess.save('Virtual')), 'a session with VIRTUAL settings saves');
  const part = await page.evaluate(() => window.__files.Virtual.units[0]);
  check(part.mix.main[24].level === -8 && part.mix.main[24].pan === -100 && part.mix.main[25].pan === 100 && JSON.stringify(part.mix.vlinks) === '[true,false,false,false]' &&
    part.mix.cues[1].sends[25].level === -14 && part.mix.aux[1].sends[24].level === -30 && (await page.evaluate(() => window.__files.Virtual.version)) === 1,
    'the file (still version 1): VIRTUAL 1-2 under main 24 / 25, their link, a cue send and an aux send, keyed by bus');
  const cleaned = await page.evaluate(() => { const c = Sess.clean(JSON.parse(JSON.stringify(window.__files.Virtual))); return c.units[0].mix; });
  check(cleaned.main[24].level === -8 && JSON.stringify(cleaned.vlinks) === '[true,false,false,false]', 'read back by this build: the VIRTUAL values and the link');
  const old = await page.evaluate(() => { const f = JSON.parse(JSON.stringify(window.__files.Virtual)); delete f.units[0].mix.vlinks; for (const b of [24, 25]) delete f.units[0].mix.main[b];
    const c = Sess.clean(f); return c.units[0].mix; });
  check(old.vlinks === undefined && old.main[24] === undefined && !old.error, 'a file from an older build (no VIRTUAL keys) reads as before');
  await page.evaluate(() => { setMix(24, 'level', -40, 0); setVLink(0, 0, false); });
  await wait(150);
  let nd = await markD();
  await page.evaluate(() => Sess.load({ name: 'Virtual', file: 'Virtual.json' }));
  await wait(600);
  let sd = await monD(nd);
  check(sd.includes('monitor\tmix:24\t-8') && sd.includes('monitor\tmixpan:24\t-100') && sd.includes('monitor\tmixpan:25\t100') && sd.includes('monitor\tsend:2:25\t-14') &&
    sd.includes('monitor\tauxsend:2:24\t-30') && await page.evaluate(() => vlinkOf(0, 0)), `the session loads them back: level, pans, link, sends (${JSON.stringify(sd.filter(x => /:2[45]/.test(x)))})`);
  // undo
  await wait(1300);
  await page.evaluate(() => setMix(24, 'mute', true, 0));
  await wait(900);
  nd = await markD();
  await page.locator('body').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press('Control+z');
  await wait(250);
  sd = await monD(nd);
  check(sd.some(x => /^monitor\tmixmute:24\t0/.test(x)) && await page.evaluate(() => !mixOfBus(0, 24).mute), `Ctrl+Z: VIRTUAL 1's mute goes back off (${sd.filter(x => /:24/.test(x)).join(', ')})`);
  await ctx.close();

  /* ---------------- E. The demo: its model, Settings to the strips, the aux note; every look draws the strips ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1500, height: 900 } });
  watch(demo);
  await demo.goto(ui + '?view=control');
  await demo.waitForTimeout(900);
  const dx = demo.locator('.useg[data-unit="0"]');
  check(await demo.locator('#inputs .vstrip').count() === 0 && (await dx.locator('.aux-tag').allTextContents()).join() === 'MONO,MONO' &&
    await dx.locator('.aux-note:visible').count() === 0, 'demo: no VIRTUAL strip by default, both aux returns running (MONO)');
  await demo.evaluate(() => { setAuxOpen(true); Virt.demoState(0, { auxLive: 0x1 }); });
  check(await dx.locator('.aux-note:visible').count() === 1 && /the AUX 2 return strip is not running/.test(await dx.locator('.aux-note:visible').getAttribute('title')),
    'demo: AUX 2 return not running: its note');
  await demo.evaluate(() => Virt.demoState(0, { auxLive: 0x3 }));
  await demo.evaluate(() => showView('settings'));
  const dpanel = demo.locator('#viPanel .vi-unitblock[data-unit="SNDEMO2017SIM00001"]');
  await dpanel.locator('.vi-row[data-virt="1"] input').check();
  await dpanel.locator('.vi-row[data-virt="2"] input').check();
  await dpanel.getByRole('button', { name: 'Apply' }).click();
  await demo.locator('#dlg[open]').getByRole('button', { name: 'Restart the x8p', exact: true }).click();
  await demo.waitForTimeout(800);
  check(/Running/.test(await dpanel.locator('.vi-row[data-virt="2"]').textContent()), 'demo: Apply builds VIRTUAL 1 and 2 (a restart, simulated)');
  await demo.evaluate(() => showView('control'));
  await demo.waitForTimeout(700);
  check(await dx.locator('.vstrip').count() === 2 && await dx.locator('.vstrip[data-virt="1"] .strip-peak').textContent() !== '--',
    'demo: their strips on the Control view, VIRTUAL 1 playing (a DAW loop)');
  await demo.evaluate(() => Virt.demoState(1, { virtualMask: 0xF, virtualAsked: 0xF }));
  check(await demo.locator('.useg[data-unit="1"] .vstrip').count() === 4, 'demo: the Twin X\'s VIRTUAL 1-4');
  const looks = await demo.evaluate(() => LOOKS.map(l => l.id));
  for (const id of looks) {
    await demo.evaluate(l => setLook(l), id);
    await demo.waitForTimeout(250);
    const v1 = dx.locator('.vstrip[data-virt="1"]');
    if (id === 'day') await v1.locator('.strip-name').click();
    await v1.scrollIntoViewIfNeeded();
    const box = await v1.boundingBox(), fd = await demo.getByRole('slider', { name: 'VIRTUAL 1 mix level', exact: true }).boundingBox();
    const sends = await demo.getByRole('slider', { name: 'VIRTUAL 1 send to AUX 1', exact: true }).boundingBox();
    const clipped = await v1.evaluate(n => n.scrollWidth > n.clientWidth + 1);
    /* a strip stands (a fader up its length) or, in Mono's rows, lies (a value bar along it) */
    const long = b => Math.max(b.width, b.height), short = b => Math.min(b.width, b.height);
    check(!!box && long(box) >= 100 && short(box) >= 50 && !!fd && long(fd) >= 40 && !!sends && sends.width > 0 && !clipped,
      `${id}: VIRTUAL 1 drawn with its fader and aux sends (${box ? `${Math.round(box.width)}x${Math.round(box.height)}` : 'none'}, fader ${fd ? Math.round(fd.height) : 0})`);
    if (id === 'day') await v1.locator('.strip-name').click();
  }
  await demo.evaluate(() => setLook('classic'));
  await demo.waitForTimeout(250);
  await demo.screenshot({ path: path.join(out, 'ui-virtual-demo.png') });
  await demo.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
