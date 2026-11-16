// Feeds the UI host-style messages (as apollo-control.exe would) and checks the non-demo path.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const url = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href + '?view=control';
const check = (c, l) => console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`);

(async () => {
  const browser = await chromium.launch();
  const page = await (await browser.newContext({ viewport: { width: 1240, height: 800 } })).newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(url);
  await page.waitForTimeout(500);
  await page.evaluate(() => exitDemo());

  const base = { version: 2, failedStage: 0, lastStatus: 0, fpgaRev: 0xA2160A11, extCaps: 0x81600611, fwRelease: 0x0B040000,
    subsystemId: 0x14, deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192,
    sampleRate: 48000, servicePasses: 10, interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: 'X8P2017ABCDEFGHI' };

  // 1. Starting up.
  await page.evaluate(b => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 2, flags: 0x9 } }), base);
  let banner = await page.locator('#bannerText').textContent();
  check(/starting up \(DSP rings ready\)/.test(banner), `starting banner: "${banner}"`);
  check(await page.evaluate(() => document.body.dataset.live) === '0', 'controls dimmed while starting');

  // 2. Failed.
  await page.evaluate(b => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 0x100, failedStage: 4, lastStatus: 0xC00000B5, flags: 0x9 } }), base);
  banner = await page.locator('#bannerText').textContent();
  check(/stopped at audio handshake done \(status 0xC00000B5\)/.test(banner), `failed banner: "${banner}"`);

  // 3. Connected, mixer ready; readback: input 1 48V+PAD, gain 42 (GainC 33); input 6 LINE + gain 20;
  //    link 3-4; monitor raw 140 (-26 dB), HP1 180, mute on, dim on (bit 31 => unsigned word).
  await page.evaluate(b => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 5, flags: 0x40 | 0x80 | 0x200 | 0x400 } }), base);
  const w = new Array(40).fill(0);
  w[0] = (1 << 3) | (1 << 1) | (0x104 << 12) | (1 << 25);   // in1 48V+PAD, link 3-4, Hi-Z jack on input 2
  w[3] = 33;                                                  // in1 GainC 33 -> 42 dB
  w[5] = 1 << 6;                                              // in6 (ch 5): line (bit 6*(5&3)+0 = 6)
  w[7] = 11 << 6;                                             // in6 GainC 11 -> 20 dB (shift (5&1)*6 + (5&2)*8 = 6)
  w[2] = (140 | (180 << 8) | (1 << 16) | (1 << 31)) >>> 0;
  await page.evaluate(words => onHostMessage({ type: 'readback', valid: true, seq: 7, words }), w.map(x => x >>> 0));
  const s = await page.evaluate(() => ({ inputs: S.inputs, links: S.links, mon: S.mon, live: document.body.dataset.live }));
  check(s.inputs[0].p48 && s.inputs[0].pad && s.inputs[0].gain === 42, `input 1: 48V ${s.inputs[0].p48}, PAD ${s.inputs[0].pad}, gain ${s.inputs[0].gain}`);
  check(s.inputs[5].line && s.inputs[5].gain === 20, `input 6: line ${s.inputs[5].line}, gain ${s.inputs[5].gain}`);
  check(s.links[1] && !s.links[0], `links ${JSON.stringify(s.links)}`);
  check(s.inputs[1].hiz && !s.inputs[0].hiz, 'Hi-Z jack on input 2 only');
  check(s.mon.level === 140 && s.mon.hp1 === 180 && s.mon.mute && s.mon.dim && !s.mon.mono, `monitor ${JSON.stringify(s.mon)}`);
  check(s.live === '1', 'controls live when connected');
  check(await page.locator('#connText').textContent() === 'X8P2017ABCDEFGHI · 48 kHz · clock stopped', 'status line');

  // 4. Send a change, then a readback that disagrees after the grace period -> warning toast.
  await page.evaluate(() => setInput(2, 'pad', true));
  const sent = await page.evaluate(() => [...S.pending.keys()]);
  check(sent.includes('in:2:pad'), 'PAD on input 3 is pending until readback agrees');
  await page.waitForTimeout(1700);
  await page.evaluate(words => onHostMessage({ type: 'readback', valid: true, seq: 8, words }), w.map(x => x >>> 0));
  const toastText = (await page.locator('.toast').allTextContents()).join(' | ');
  check(/Input 3 PAD: sent on, the unit reports off/.test(toastText) && /Input 4 PAD/.test(toastText),
    `disagreement toasts (3-4 linked, so both): "${toastText}"`);

  // 4b. [polish Nov 15] x8p pairs: on linked 3-4 a change on input 4 goes to input 3 first (the input the unit
  //     applies to both), then to input 4; on unlinked 1-2 input 2 gets only its own write.
  const sentPair = await page.evaluate(() => {
    const keep = window.send, out = [];
    window.send = (...p) => out.push(p.map(String).join(' '));
    setInput(3, 'phase', true);
    setInput(1, 'phase', true);
    window.send = keep;
    return out.filter(s => s.startsWith('input '));
  });
  check(sentPair.join() === 'input 2 phase 1,input 3 phase 1,input 1 phase 1',
    `x8p linked 3-4: input 4's change to input 3 first, then 4; unlinked 1-2: input 2 alone (${JSON.stringify(sentPair)})`);
  const shown = await page.evaluate(() => [S.inputs[2].phase, S.inputs[3].phase, S.inputs[0].phase, S.inputs[1].phase]);
  check(shown.join() === 'true,true,false,true', `x8p: both linked inputs show the change, input 1 untouched (${shown.join()})`);

  // 5. Driver error for a monitor change.
  await page.evaluate(() => onHostMessage({ type: 'error', op: 'monitor', key: 'level', index: 1, code: 21, detail: 'The device is not ready' }));
  const t2 = await page.locator('.toast').last().textContent();
  check(/Monitor level was not changed: the mixer is not ready yet/.test(t2), `error toast: "${t2}"`);

  // 6. Levels arrive: meters lose the no-clock hatch.
  await page.evaluate(() => onHostMessage({ type: 'levels', valid: true, peaks: [4194304, 0, 838861] }));
  await page.waitForTimeout(300);
  check(!(await page.locator('.strip[data-ch="0"] .meter').getAttribute('class')).includes('noclock'), 'meters live once levels arrive');
  check(/-6\.0/.test(await page.locator('.strip[data-ch="0"] .strip-peak').textContent()), 'input 1 peak reads -6.0 dBFS');

  // 6b. [G2] Headphones on the x8p: no HP 1 level knob (analog), nothing sent for it.
  //     [Monitor] Its HP 1 / HP 2 sources (CUE 1 / CUE 2, the kext's S12 fields) are shown and sent.
  await page.evaluate(() => { window.__sent = []; window.send = (...p) => window.__sent.push(p.map(String)); });
  check(await page.getByRole('slider', { name: 'Headphone 1 level' }).isHidden(), 'x8p: HP 1 level knob hidden');
  check(await page.getByRole('group', { name: 'Headphone 1 source' }).isVisible() &&
        await page.getByRole('group', { name: 'Headphone 2 source' }).isVisible() &&
        (await page.getByRole('group', { name: 'Headphone 2 source' }).getByRole('button').allTextContents()).join() === 'CUE 1,CUE 2',
        'x8p: HP 1 and HP 2 source selectors, CUE 1 / CUE 2 [Monitor]');
  check(/knobs on the unit/.test(await page.locator('.mon-hp .hp-note').textContent()), 'x8p: headphone note shown');
  await page.evaluate(() => { setMon('hp1', 100); setMon('hp1src', 1); setMon('hp2src', 0); setMon('level', 150); });
  await page.waitForTimeout(100);
  let hp = await page.evaluate(() => window.__sent);
  check(!hp.some(p => p[0] === 'monitor' && p[1] === 'hp1') && hp.some(p => p[0] === 'monitor' && p[1] === 'hp1src' && p[2] === '1') &&
        hp.some(p => p[0] === 'monitor' && p[1] === 'hp2src' && p[2] === '0'), `x8p: no hp1 level sent; hp1src/hp2src sent [Monitor] (${JSON.stringify(hp)})`);
  check(hp.some(p => p[0] === 'monitor' && p[1] === 'level' && p[2] === '150'), 'x8p: the monitor level still goes out');

  // 6c. [G2] A unit-tagged status for unit 0 is the controls' unit; a Twin X (unit 1, observe) is not.
  await page.evaluate(b => onHostMessage({ type: 'status', unit: 0, present: true, info: { ...b, version: 6, mode: 2, observe: false, stage: 5, flags: 0x40 | 0x80 | 0x200 | 0x400 } }), base);
  const twx = { ...base, version: 6, stage: 1, flags: 0x3, deviceType: 0x99, publicType: 0x19, subsystemId: 0x19, numDsps: 4,
    numPreamps: 0, serial: 'TWX2024ABCDEF', mode: 0, observe: true, modelName: 'Apollo Twin X (Gen 1)', unitFlags: 0x15,
    owner: { rateInfo: 0x201, clockSrcs: 0x14A1007E, descInVersion: 1, descInCount: 24, descOutVersion: 1, descOutCount: 10, stale: true } };
  await page.evaluate(t => onHostMessage({ type: 'status', unit: 1, present: true, info: t }), twx);
  const after = await page.evaluate(() => ({ dt: S.host.info.deviceType, serial: S.host.info.serial, live: document.body.dataset.live, u1: S.units[1] && S.units[1].info.deviceType }));
  check(after.dt === 0x22 && after.serial === 'X8P2017ABCDEFGHI' && after.live === '1', `Twin X status leaves the x8p controls alone (${JSON.stringify(after)})`);
  check(after.u1 === 0x99, 'Twin X status stored as unit 1');
  const mon0 = await page.evaluate(() => ({ ...S.mon }));
  await page.evaluate(() => onHostMessage({ type: 'readback', unit: 1, valid: true, seq: 99, words: new Array(40).fill(0) }));
  await page.evaluate(() => onHostMessage({ type: 'levels', unit: 1, valid: false, peaks: [] }));
  const mon1 = await page.evaluate(() => ({ ...S.mon, lv: S.levelsValid }));
  check(mon1.level === mon0.level && mon1.mute === mon0.mute && mon1.lv === true, 'readback and levels of unit 1 are ignored');

  // 6d. [G2] units list and the combined-device reply.
  await page.evaluate(() => onHostMessage({ type: 'units', units: [
    { id: '0123ABCD89EF4567', index: 0, model: 'Apollo x8p', deviceType: 34, serial: 'X8P2017ABCDEFGHI', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 },
    { id: 'FEDC000011112222', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: 'TWX2024ABCDEF', fw: '1.2.3', mode: 0, present: true, active: false, observe: true, stage: 1 }] }));
  check(await page.evaluate(() => S.unitList.length === 2 && S.unitList[1].observe === true), 'units list stored');
  // The host's reply until the driver has AGG_* (main.cpp HandleUiJson): error code 1, no elevation.
  await page.evaluate(() => onHostMessage({ type: 'agg', config: null, pending: false, error: { code: 1, detail: 'not available in this build' }, needsAdmin: false }));
  check(await page.evaluate(() => S.agg && S.agg.error.code === 1 && S.agg.needsAdmin === false && S.host.info.deviceType === 0x22), 'agg reply stored, nothing else touched');

  // 6e. [G2] The per-model path: a model with a digital headphone level (Twin X caps) shows the knob and sends hp1.
  await page.evaluate(b => onHostMessage({ type: 'status', unit: 0, present: true, info: { ...b, deviceType: 0x99, stage: 5, flags: 0x40 | 0x80 | 0x200 | 0x400 } }), base);
  check(await page.getByRole('slider', { name: 'Headphone 1 level' }).isVisible(), 'hpLevel model: HP 1 knob shown');
  check(await page.getByRole('group', { name: 'Headphone 1 source' }).isVisible() &&
        await page.getByRole('group', { name: 'Headphone 2 source' }).isHidden(), 'hpLevel model (Twin X caps): one HP source selector, no HP 2 [Monitor]');
  await page.evaluate(() => { window.__sent = []; setMon('hp1', 101); });
  hp = await page.evaluate(() => window.__sent);
  check(hp.some(p => p[0] === 'monitor' && p[1] === 'hp1' && p[2] === '101'), 'hpLevel model: hp1 level goes out');
  await page.evaluate(b => onHostMessage({ type: 'status', unit: 0, present: true, info: { ...b, stage: 5, flags: 0x40 | 0x80 | 0x200 | 0x400 } }), base);

  // 7. Device removed.
  await page.evaluate(() => onHostMessage({ type: 'status', present: false, error: 2, detail: 'apollo.sys is not loaded on any device' }));
  check(/not running on the x8p yet/.test(await page.locator('#bannerText').textContent()), 'absent banner after removal');

  await page.screenshot({ path: path.join(out, 'ui-hostpath.png') });
  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
