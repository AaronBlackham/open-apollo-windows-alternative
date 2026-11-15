// Scans UAD DLLs for "Bill" DSP module records (docs/spec-dsp-mixer.md 1.4) and compares them
// with the strip modules open-apollo verified (pins for UAD 12.0.0.602).
// usage: node find-strip-modules.js <dir-with-dlls>
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PINS = {
  0x0EB: { len: 716, w4: 162, role: 'Mic strip', sha: '2b879e0824970650f4ae0de110bb4bdb22897d0833d85156dd36f51986c15ea7' },
  0x0DA: { len: 3044, w4: 744, role: 'Mic strip', sha: '37cd9c2df53ec742d61353437ad7f7212633e27801f1cf1f57e3818cbe47dd06' },
  0x0A5: { len: 1940, w4: 456, role: 'Mic + Input strip', sha: '1fe90428f88b7f8c0a35b01ea0cfca3937d4b365b7c6ef427d24cfee65ee6c81' },
  0x0C2: { len: 184, w4: 33, role: 'Mic + Input strip', sha: 'ad0946b371df47c9a7d459c7b5b288d58aad9690b6a9a39a6c5895ac36720a2f' },
  0x12B: { len: 452, w4: 96, role: 'Mic strip', sha: 'ff60952444ae492aa1a8b85dae686ad8445b9971260174202ad1ce13c73ba02d' },
  0x0DB: { len: 728, w4: 165, role: 'Input strip (reserved)', sha: '5937b45b7f08f974f1d8599b3cfaec8130ac7815f3a3c3410612ba041a7cee0a' },
  0x0C9: { len: 352, w4: 75, role: 'Talkback (reserved)', sha: '8222981a81d20c417113b02340e1df8995d4aa11b07942017583f2151a31a24a' },
};

const dir = process.argv[2];
const found = new Map();   // id -> [{file, off, len, w4, sha}]
for (const name of fs.readdirSync(dir).filter(n => n.toLowerCase().endsWith('.dll'))) {
  const b = fs.readFileSync(path.join(dir, name));
  for (let off = 0; off + 20 <= b.length; off += 4) {
    if (b[off] !== 0x42 || b[off + 1] !== 0x69 || b[off + 2] !== 0x6c || b[off + 3] !== 0x6c) continue;  // "Bill"
    const id = b.readUInt32LE(off + 4);
    if (id >>> 24 !== 0) continue;
    if (b.readUInt32LE(off + 8) !== 0x02000000) continue;
    const payload = b.readUInt32LE(off + 12);
    const len = 0x14 + payload;
    if (off + len > b.length) continue;
    const w4 = b.readUInt32LE(off + 16);
    const sha = crypto.createHash('sha256').update(b.subarray(off, off + len)).digest('hex');
    if (!found.has(id)) found.set(id, []);
    found.get(id).push({ file: name, off, len, w4, sha });
  }
}

console.log(`Bill records found: ${[...found.values()].reduce((n, a) => n + a.length, 0)} (${found.size} distinct ids)\n`);
for (const [id, pin] of Object.entries(PINS)) {
  const hits = found.get(Number(id)) || [];
  const tag = `0x${Number(id).toString(16).toUpperCase().padStart(3, '0')} ${pin.role}`;
  if (!hits.length) { console.log(`${tag}: NOT FOUND`); continue; }
  const shas = [...new Set(hits.map(h => h.sha))];
  for (const sha of shas) {
    const h = hits.find(x => x.sha === sha);
    const same = sha === pin.sha ? 'IDENTICAL to the 12.0.0.602 pin'
      : (h.len === pin.len && h.w4 === pin.w4 ? 'same length and w4, different bytes' : `DIFFERENT shape (len ${h.len} vs ${pin.len}, w4 ${h.w4} vs ${pin.w4})`);
    console.log(`${tag}: len ${h.len} w4 ${h.w4} in ${hits.filter(x => x.sha === sha).map(x => x.file).join(', ')} -> ${same}`);
  }
}
