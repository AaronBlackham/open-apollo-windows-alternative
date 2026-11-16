# SPDX-License-Identifier: GPL-2.0-only
"""Python port of open-apollo's `oa-assets dsp` (console-rs/crates/oa-assets/src/dsp.rs), for Windows.

Port by Avium, 2026-11-14. Why a port and not the Rust crate: building oa-assets needs ~60 crates
from crates.io and this PC may not fetch them (no network beyond the pinned panel specs). The
logic is a line-by-line port of dsp.rs and the helpers it calls (firmware.rs all_bill_records,
sha256_hex, rel, file_label, utc_now; pe.rs file_version_of / string_file_version and
plugins/pe.rs resources; detect.rs tools_dir, uad_version, walk, is_ua_name; lib.rs ci_path).
Output is the same `open-apollo-plugin-dsp/1` manifest (keys sorted and 2-space indented, as
serde_json writes it) and the same content-addressed store, `modules/<sha256>.bill`.

  python -I oa_assets_dsp.py dsp --from ROOT --out DIR --pins FIRMWARE-PINS.JSON [--dry-run] [--jobs N]
  python -I oa_assets_dsp.py strips --store DIR --pins FIRMWARE-PINS.JSON --out DIR

`dsp` reads every Powered plug-in DLL (and UAD2SDK.dll, UAD2DriverClient.dll) under ROOT as bytes,
never loading or running any of them, and writes DIR/modules/<sha256>.bill (each distinct Bill
record once, unmodified) and DIR/manifest.json. A rerun rewrites nothing already right and removes
store files the new manifest no longer lists. ROOT is only read.

`strips` (Windows port addition): writes the Console strip modules the native carrier needs
(native_carrier.DSPDIR, `<id>.bill`) from the store, choosing only records whose SHA-256 the
firmware pins list, as `oa-assets firmware` without --accept-unknown writes only pinned versions.

Windows note (port of firmware::inside): with ROOT a drive root (the live system, not a mounted
partition), "inside the install" means inside the folders this reads, not the whole drive.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import struct
import sys
import threading
import time
from pathlib import Path

MANIFEST = 'manifest.json'
MANIFEST_FORMAT = 'open-apollo-plugin-dsp/1'
STORE = 'modules'
SYSTEM_DLLS = ('UAD2SDK.dll', 'UAD2DriverClient.dll')
GENERATOR = 'oa-assets 0.1.0 (Python port for Windows)'
BILL_MAGIC, BILL_HDR = b'Bill', 0x14
EXIT_NOTHING = 1


# ---- lib.rs / detect.rs ----

def ci_path(root, parts):
    cur = Path(root)
    for part in parts:
        exact = cur / part
        if exact.exists():
            cur = exact
            continue
        try:
            entry = next((e for e in os.scandir(cur) if e.name.lower() == part.lower()), None)
        except OSError:
            return None
        if entry is None:
            return None
        cur = Path(entry.path)
    return cur


def rust_order(p):
    """PathBuf ordering: component-wise, byte-wise (case-sensitive), not Windows' case folding."""
    return [s.encode('utf-8', 'surrogatepass') for s in Path(p).parts]


def walk(directory, depth, want):
    out = []
    try:
        entries = [Path(e.path) for e in os.scandir(directory)]
    except OSError:
        entries = []
    for p in entries:
        if want(p):
            out.append(p)
        elif depth > 0 and p.is_dir():
            out.extend(walk(p, depth - 1, want))
    return sorted(out, key=rust_order)


def has_ext(p, ext):
    return Path(p).name.lower().endswith('.' + ext)


def is_ua_name(rel):
    return any(c.lower().startswith('uad') or 'universal audio' in c.lower() for c in Path(rel).parts)


def tools_dir(root):
    for pf in ('Program Files (x86)', 'Program Files'):
        x = ci_path(root, [pf, 'Universal Audio', 'Powered Plugins', 'UA.xml'])
        if x is not None:
            return x.parent
    return None


def read_text(path):
    """std::fs::read_to_string: UTF-8 or nothing."""
    try:
        return Path(path).read_bytes().decode('utf-8')
    except (OSError, UnicodeDecodeError):
        return None


def xml_tag(path, tag):
    text = read_text(path)
    if text is None:
        return None
    start = text.find(f'<{tag}>')
    if start < 0:
        return None
    start += len(tag) + 2
    end = text[start:].find(f'</{tag}>')
    return None if end < 0 else text[start:start + end].strip()


def uad_version(root):
    t = tools_dir(root)
    return xml_tag(t / 'UA.xml', 'swReleaseName') if t else None


# ---- firmware.rs ----

def le32(b, at):
    return struct.unpack_from('<I', b, at)[0]


def all_bill_records(data):
    """Every on-disk Bill record, as UAD2SDK's own validator bounds them (firmware::all_bill_records):
    4-byte aligned, id arch byte 0, kind <= 4, no flags, architecture <= 2, a payload that fits,
    a whole number of words and w4 at most the record's word count minus 13."""
    out, n, i = [], len(data), data.find(BILL_MAGIC)
    while i >= 0:
        if i % 4 == 0 and i + BILL_HDR <= n:
            ident, w2, size, w4 = struct.unpack_from('<IIII', data, i + 4)
            length = BILL_HDR + size
            if (ident >> 24 == 0 and w2 & 0xff <= 4 and (w2 >> 8) & 0xffff == 0 and w2 >> 24 <= 2
                    and i + length <= n and length % 4 == 0 and w4 + 13 <= length // 4):
                out.append(dict(offset=i, id=ident, len=length, w2=w2, w4=w4))
        i = data.find(BILL_MAGIC, i + 1)
    return out


def sha256_hex(b):
    return hashlib.sha256(b).hexdigest()


def file_label(p):
    return Path(p).name


def rel(root, p):
    try:
        parts = Path(p).relative_to(root).parts
    except ValueError:
        parts = Path(p).parts
    return '/'.join(parts)


def utc_now():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def write_atomic(directory, relpath, data):
    dest = Path(directory) / relpath
    dest.parent.mkdir(parents=True, exist_ok=True)   # Windows port: no Unix modes on NTFS (ACLs inherit)
    tmp = dest.parent / f'.{dest.name}.tmp-{os.getpid()}'
    try:
        tmp.unlink()
    except FileNotFoundError:
        pass
    try:
        with open(tmp, 'xb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dest)
    except OSError as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise OSError(f"can't write {dest}: {e}") from e


def inside(out, root):
    """firmware::inside, plus the Windows port's drive-root rule (see the module docstring)."""
    try:
        root = Path(root).resolve(strict=True)
    except OSError:
        return False
    out = Path(os.path.abspath(out))
    base, tail = out, []
    while not base.exists() and base.parent != base:
        tail.append(base.name)
        base = base.parent
    p = base.resolve()
    for c in reversed(tail):
        p = p.parent if c == '..' else p / c
    if root.parent == root:   # a drive root: the live Windows system
        read = [ci_path(root, [pf, 'Universal Audio']) for pf in ('Program Files', 'Program Files (x86)')]
        read += [ci_path(root, ['Windows']), ci_path(root, ['Program Files', 'Common Files', 'VST3'])]
        return any(d is not None and (p == d.resolve() or d.resolve() in p.parents) for d in read)
    return p == root or root in p.parents


# ---- pe.rs ----

def file_version_of(b):
    at = b.rfind(struct.pack('<I', 0xFEEF04BD))
    if at < 0 or at + 16 > len(b):
        return None
    ms, ls = le32(b, at + 8), le32(b, at + 12)
    return f'{ms >> 16}.{ms & 0xffff}.{ls >> 16}.{ls & 0xffff}'


def _u16(b, o):
    return struct.unpack_from('<H', b, o)[0] if 0 <= o and o + 2 <= len(b) else None


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0] if 0 <= o and o + 4 <= len(b) else None


class _Missing(Exception):
    pass


def _need(v):
    if v is None:
        raise _Missing
    return v


def resources(b):
    """plugins/pe.rs resources(): every TYPE/NAME/LANG leaf (ty, name, lang, offset, size)."""
    try:
        if b[:2] != b'MZ':
            return None
        pe = _need(_u32(b, 0x3c))
        if b[pe:pe + 4] != b'PE\0\0':
            return None
        coff = pe + 4
        nsec, opt_size = _need(_u16(b, coff + 2)), _need(_u16(b, coff + 16))
        opt = coff + 20
        magic = _need(_u16(b, opt))
        dirs = {0x20b: opt + 112, 0x10b: opt + 96}.get(magic)
        if dirs is None:
            return None
        res_rva = _need(_u32(b, dirs + 16))
        secs = []
        for i in range(nsec):
            s = opt + opt_size + i * 40
            vsize, va, raw_size, raw = (_need(_u32(b, s + k)) for k in (8, 12, 16, 20))
            secs.append((va, max(vsize, raw_size), raw))

        def file_offset(rva):
            for va, sz, raw in secs:
                if va <= rva < (va + sz) & 0xffffffff:
                    return rva - va + raw
            return None
        if res_rva == 0:
            return []
        root = _need(file_offset(res_rva))

        def name_of(ident):
            if ident & 0x80000000 == 0:
                return str(ident)
            at = root + (ident & 0x7fffffff)
            n = _need(_u16(b, at))
            units = [_need(_u16(b, at + 2 + 2 * k)) for k in range(n)]
            return struct.pack(f'<{n}H', *units).decode('utf-16-le', 'replace')

        def entries(d):
            n = _need(_u16(b, d + 12)) + _need(_u16(b, d + 14))
            return [(_need(_u32(b, d + 16 + 8 * k)), _need(_u32(b, d + 20 + 8 * k))) for k in range(min(n, 65536))]
        out = []
        for tid, toff in entries(root):
            if toff & 0x80000000 == 0:
                continue
            ty = name_of(tid)
            for nid, noff in entries(root + (toff & 0x7fffffff)):
                if noff & 0x80000000 == 0:
                    continue
                name = name_of(nid)
                for lid, loff in entries(root + (noff & 0x7fffffff)):
                    if loff & 0x80000000:
                        continue
                    de = root + loff
                    rva, size = _need(_u32(b, de)), _need(_u32(b, de + 4))
                    offset = file_offset(rva)
                    if offset is None:
                        continue
                    out.append(dict(ty=ty, name=name, lang=lid & 0x7fffffff, offset=offset, size=size))
        return out
    except _Missing:
        return None


def string_value(block, key):
    pat = key.encode('utf-16-le') + b'\0\0'
    start_from = 0
    while True:
        hit = block.find(pat, start_from)
        if hit < 0:
            return None
        start_from = hit + 2
        start = hit - 6
        if start < 0 or start % 4:
            continue
        chars = struct.unpack_from('<H', block, start + 2)[0]
        value = start + (6 + len(pat) + 3) // 4 * 4
        units = []
        for k in range(chars):
            if value + 2 * k + 2 > len(block):
                break
            u = struct.unpack_from('<H', block, value + 2 * k)[0]
            if u == 0:
                break
            units.append(u)
        s = struct.pack(f'<{len(units)}H', *units).decode('utf-16-le', 'replace').strip()
        return s or None


def string_file_version(b):
    leaves = resources(b)
    if not leaves:
        return None
    leaf = next((l for l in leaves if l['ty'] == '16'), None)
    if leaf is None or leaf['offset'] + leaf['size'] > len(b):
        return None
    return string_value(b[leaf['offset']:leaf['offset'] + leaf['size']], 'FileVersion')


# ---- dsp.rs ----

def plugin_dlls(root):
    out = []
    for pf in ('Program Files', 'Program Files (x86)'):
        d = ci_path(root, [pf, 'Universal Audio', 'Powered Plugins', 'UAD-2 Powered Plugins'])
        if d is not None:
            out += walk(d, 0, lambda p: has_ext(p, 'dll') and p.is_file())
    dedup = []
    for p in out:   # Vec::dedup: consecutive duplicates only
        if not dedup or dedup[-1] != p:
            dedup.append(p)
    return dedup


def system_dlls(root):
    return [p for n in SYSTEM_DLLS if (p := ci_path(root, ['Windows', 'System32', n])) is not None and p.is_file()]


def xml_unescape(s):
    return s.replace('&lt;', '<').replace('&gt;', '>').replace('&quot;', '"').replace('&apos;', "'").replace('&amp;', '&')


def xml_attrs(tag):
    out, rest = [], tag
    while (eq := rest.find('=')) >= 0:
        words = rest[:eq].split()
        name = words[-1] if words else ''
        after = rest[eq + 1:].lstrip()
        if not after or after[0] not in '"\'':
            break
        q = after[0]
        end = after[1:].find(q)
        if end < 0:
            break
        out.append((name, xml_unescape(after[1:1 + end])))
        rest = after[end + 2:]
    return out


def _u32_parse(s):
    try:
        v = int(s.strip(), 10)
    except (ValueError, AttributeError):
        return None
    return v if 0 <= v <= 0xffffffff and s.strip().isdigit() else None


def split_ids(s):
    return [v.strip() for v in s.split(',') if v.strip()]


def parse_ua_xml(text):
    start = text.find('<plugins>')
    if start < 0:
        return []
    body = text[start:]
    end = body.find('</plugins>')
    body = body[:end if end >= 0 else len(body)]
    out = []
    for tag in body.split('<plugin ')[1:]:
        gt = tag.find('>')
        tag = tag[:gt if gt >= 0 else len(tag)]
        attrs = dict(reversed(xml_attrs(tag.rstrip('/'))))   # find(): the first occurrence wins
        ident = _u32_parse(attrs['id']) if 'id' in attrs else None
        if ident is None:
            continue
        out.append(dict(id=ident, title=attrs.get('title', ''),
                        render_id=_u32_parse(attrs['renderid']) if 'renderid' in attrs else None,
                        vst_ids=split_ids(attrs.get('vstids', ''))))
    return out


def ua_xml_json(e):
    return {'id': e['id'], 'title': e['title'], 'renderid': e['render_id'], 'vstids': e['vst_ids']}


def parse_plugin_manifest(v):
    def text(k):
        s = v.get(k)
        return s.strip() if isinstance(s, str) and s.strip() else None

    def number(k):
        x = v.get(k)
        if isinstance(x, int) and not isinstance(x, bool) and x >= 0:
            return x & 0xffffffff
        return _u32_parse(x) if isinstance(x, str) else None
    vst_ids = split_ids(v.get('vstID') if isinstance(v.get('vstID'), str) else '')
    for i in split_ids(v.get('vstids') if isinstance(v.get('vstids'), str) else ''):
        if i not in vst_ids:
            vst_ids.append(i)
    rid = number('renderID')
    return dict(name=text('name'), plugin_id=text('plugin_id'), render_id=rid if rid is not None else number('renderid'),
                vst_ids=vst_ids)


def read_json(path):
    try:
        return json.loads(Path(path).read_bytes())
    except (OSError, ValueError):
        return None


def vst3_index(root):
    out = {}
    d = ci_path(root, ['Program Files', 'Common Files', 'VST3'])
    if d is None:
        return out
    for b in walk(d, 3, lambda p: has_ext(p, 'vst3') and is_ua_name(p.relative_to(d))):
        def read(name):
            p = ci_path(b, ['Contents', 'Resources', name])
            return read_json(p) if p is not None else None
        m = read('manifest.json')
        if not isinstance(m, dict) or not isinstance(m.get('plugin_id'), str) or not m['plugin_id']:
            continue
        mi = read('moduleinfo.json')
        cid = None
        if isinstance(mi, dict) and isinstance(mi.get('Classes'), list):
            c = next((c for c in mi['Classes'] if isinstance(c, dict) and c.get('Category') == 'Audio Module Class'), None)
            cid = c['CID'] if c is not None and isinstance(c.get('CID'), str) else None
        out.setdefault(m['plugin_id'], []).append(dict(
            bundle=rel(d, b), plugin_id=m['plugin_id'],
            bundle_id=m['bundle_id'] if isinstance(m.get('bundle_id'), str) else None, cid=cid))
    return out


def scan_dll(path, store, lock):
    data = Path(path).read_bytes()   # bytes only: nothing is loaded or run
    records = []
    for rec in all_bill_records(data):
        b = data[rec['offset']:rec['offset'] + rec['len']]
        sha = sha256_hex(b)
        with lock:
            store.setdefault(sha, b)
        records.append((rec, sha))
    return dict(path=Path(path), size=len(data), sha256=sha256_hex(data), version=string_file_version(data),
                file_version=file_version_of(data), records=records)


def module_ref(rec, sha):
    return {'id': f"{rec['id']:03x}", 'kind': rec['w2'] & 0xff, 'arch': rec['w2'] >> 24, 'w2': rec['w2'], 'w4': rec['w4'],
            'length': rec['len'], 'sha256': sha, 'offset': rec['offset']}


def store_name(sha):
    return f'{STORE}/{sha}.bill'


def is_store_file_name(name):
    h = name[:-5] if name.endswith('.bill') else None
    return h is not None and len(h) == 64 and all(c in '0123456789abcdef' for c in h)


def holds(path, length, sha):
    try:
        st = os.lstat(path)
        if not os.path.isfile(path) or os.path.islink(path) or st.st_size != length:
            return False
        return sha256_hex(Path(path).read_bytes()) == sha
    except OSError:
        return False


def dumps(v):
    return json.dumps(v, indent=2, sort_keys=True, ensure_ascii=False) + '\n'


def pinned_names(pins):
    return {k['sha256']: f"dsp/{int(m['id'], 16):03x}.bill" for m in pins['dsp_modules'] for k in m['known']}


def collect(root, out, pins, dry_run=False, jobs=0, log=print):
    root = Path(root)
    log('── Reading your UAD install\'s plug-in DSP modules ──')
    log(f'[INFO]  From {root} (read-only)')
    version = uad_version(root)
    if version:
        log(f'[INFO]  UAD software {version}')
    if not dry_run:
        log(f'[INFO]  To {out}')
    plugins, system = plugin_dlls(root), system_dlls(root)
    if not plugins:
        log('[FAIL]  No Powered plug-in DLLs in this install (UAD-2 Powered Plugins folder)')
        return None, EXIT_NOTHING
    jobs = jobs or min(os.cpu_count() or 2, 4)
    log(f'[INFO]  Scanning {len(plugins)} plug-in DLLs and {len(system)} UAD system DLLs')
    store, lock = {}, threading.Lock()
    paths = plugins + system
    scans = []
    with concurrent.futures.ThreadPoolExecutor(max(1, min(jobs, len(paths)))) as pool:
        futures = [pool.submit(scan_dll, p, store, lock) for p in paths]
        for p, f in zip(paths, futures):
            try:
                scans.append(f.result())
            except OSError as e:
                log(f"[WARN]  can't read {p}: {e}")
    plugin_set = set(plugins)
    is_plugin = lambda s: s['path'] in plugin_set
    if not any(is_plugin(s) for s in scans):
        log('[FAIL]  No Powered plug-in DLLs in this install (UAD-2 Powered Plugins folder)')
        return None, EXIT_NOTHING

    tools = tools_dir(root)
    ua_path = ci_path(tools, ['UA.xml']) if tools else None
    ua_xml = parse_ua_xml(read_text(ua_path) or '') if ua_path else []
    vst3 = vst3_index(root)
    pinned = pinned_names(pins)

    users, first = {}, {}
    for s in scans:
        for rec, sha in s['records']:
            users.setdefault(sha, set()).add(s['path'])
            first.setdefault(sha, rec)
    modules = sorted(first.items(), key=lambda kv: (kv[1]['id'], kv[1]['w2'], kv[0]))
    records = sum(len(s['records']) for s in scans if is_plugin(s))
    total_bytes = sum(r['len'] for _, r in modules)
    module_json = []
    for sha, r in modules:
        m = {'sha256': sha, 'file': store_name(sha), 'id': f"{r['id']:03x}", 'kind': r['w2'] & 0xff, 'arch': r['w2'] >> 24,
             'w2': r['w2'], 'w4': r['w4'], 'length': r['len'], 'verified': sha in pinned, 'used_by': len(users[sha])}
        if sha in pinned:
            m['pinned_as'] = pinned[sha]
        module_json.append(m)

    def dll_json(s):
        return {'dll': file_label(s['path']), 'path': rel(root, s['path']), 'size': s['size'], 'sha256': s['sha256'],
                'version': s['version'], 'file_version': s['file_version'],
                'modules': [module_ref(r, sha) for r, sha in s['records']]}
    plugin_json, unmatched = [], 0
    for s in (s for s in scans if is_plugin(s)):
        stem = s['path'].stem
        mp = ci_path(tools, ['Manifest', stem, 'manifest.json']) if tools else None
        mv = read_json(mp) if mp is not None else None
        meta = parse_plugin_manifest(mv) if isinstance(mv, dict) else dict(name=None, plugin_id=None, render_id=None, vst_ids=[])
        registry = [ua_xml_json(e) for e in ua_xml
                    if (meta['render_id'] is not None and e['render_id'] == meta['render_id'])
                    or any(v in meta['vst_ids'] for v in e['vst_ids'])]
        bundles = [{'bundle': b['bundle'], 'plugin_id': b['plugin_id'], 'bundle_id': b['bundle_id'], 'cid': b['cid']}
                   for b in vst3.get(meta['plugin_id'], [])] if meta['plugin_id'] else []
        unmatched += not registry
        v = dll_json(s)
        v.update(name=meta['name'], plugin_id=meta['plugin_id'], render_id=meta['render_id'], vst_ids=meta['vst_ids'],
                 ua_xml=registry, vst3=bundles)
        plugin_json.append(v)
    system_json = [dll_json(s) for s in scans if not is_plugin(s)]
    verified = sum(m['verified'] for m in module_json)
    manifest = {'format': MANIFEST_FORMAT, 'generator': GENERATOR, 'created': utc_now(),
                'from': os.path.abspath(root), 'uad_version': version, 'store': STORE,
                'totals': {'plugins': len(plugin_json), 'records': records, 'modules': len(modules),
                           'bytes': total_bytes, 'verified': verified},
                'modules': module_json, 'plugins': plugin_json, 'system': system_json}
    log(f'[INFO]  {len(plugin_json)} plug-ins carry {records} records: {len(modules)} distinct modules, '
        f'{total_bytes / 1e6:.1f} MB ({verified} are pinned strip modules)')
    if unmatched:
        log(f'[INFO]  {unmatched} plug-ins have no UA.xml entry (Console strip headers and internals)')
    report = dict(manifest=manifest, written=0, kept=0, removed=0)
    if dry_run:
        log(f'[ OK ]  Dry run: nothing written; {len(modules)} store files and {MANIFEST} would go to {out}')
        return report, 0

    log('── Module store ──')
    out = Path(out)
    for sha, r in modules:
        name = store_name(sha)
        if holds(out / name, r['len'], sha):
            report['kept'] += 1
        else:
            write_atomic(out, name, store[sha])
            report['written'] += 1
    write_manifest(out, manifest)
    keep = {f'{sha}.bill' for sha, _ in modules}
    try:
        stale = [e for e in os.scandir(out / STORE) if is_store_file_name(e.name) and e.name not in keep]
    except OSError:
        stale = []
    for e in stale:
        os.remove(e.path)
        report['removed'] += 1
    log(f"[ OK ]  {len(modules)} modules in {out / STORE}: {report['written']} written, {report['kept']} already there, "
        f"{report['removed']} stale removed")
    return report, 0


def write_manifest(out, manifest):
    strip = lambda v: {k: x for k, x in v.items() if k != 'created'} if isinstance(v, dict) else v
    old = read_json(Path(out) / MANIFEST)
    if isinstance(old, dict) and old.get('format') is not None and strip(old) == strip(manifest):
        return
    write_atomic(out, MANIFEST, dumps(manifest).encode('utf-8'))


def strips(store, pins, out, log=print):
    """dsp/<id>.bill for every pinned strip module the store holds (pinned SHA-256 only)."""
    exit_code = 0
    for m in pins['dsp_modules']:
        ident = int(m['id'], 16)
        hit = next((k for k in m['known'] if (Path(store) / STORE / f"{k['sha256']}.bill").is_file()), None)
        if hit is None:
            log(f"{ident:03x}.bill  missing  (no pinned version in the store){'  REQUIRED' if m.get('required', True) else ''}")
            exit_code = exit_code or (1 if m.get('required', True) else 0)
            continue
        data = (Path(store) / STORE / f"{hit['sha256']}.bill").read_bytes()
        if sha256_hex(data) != hit['sha256'] or len(data) != hit['length'] or (hit.get('w4') is not None and le32(data, 16) != hit['w4']):
            log(f'{ident:03x}.bill  failed  store file differs from its pin')
            exit_code = 2
            continue
        write_atomic(out, f'{ident:03x}.bill', data)
        log(f"{ident:03x}.bill  ok  sha256 {hit['sha256']}  (pinned UAD {hit.get('uad')})")
    return exit_code


def main(argv=None):
    ap = argparse.ArgumentParser(prog='oa_assets_dsp.py', description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('dsp')
    d.add_argument('--from', dest='root', type=Path, required=True)
    d.add_argument('--out', type=Path, required=True)
    d.add_argument('--pins', type=Path, required=True, help='console-rs/crates/oa-assets/data/firmware-pins.json')
    d.add_argument('--dry-run', action='store_true')
    d.add_argument('--json', action='store_true')
    d.add_argument('--jobs', type=int, default=0)
    s = sub.add_parser('strips')
    s.add_argument('--store', type=Path, required=True)
    s.add_argument('--pins', type=Path, required=True)
    s.add_argument('--out', type=Path, required=True)
    args = ap.parse_args(argv)
    pins = json.loads(args.pins.read_text(encoding='utf-8'))
    if pins.get('format') != 'open-apollo-firmware-pins/1':
        ap.error('pins: format is not open-apollo-firmware-pins/1')
    if args.cmd == 'strips':
        return strips(args.store, pins, args.out)
    if inside(args.out, args.root):
        ap.error(f'--out {args.out} is inside the UAD install; pick a folder outside it')
    report, code = collect(args.root, args.out, pins, args.dry_run, args.jobs,
                           log=(lambda *_: None) if args.json else print)
    if args.json and report:
        sys.stdout.write(dumps(report['manifest']))
    return code


if __name__ == '__main__':
    sys.exit(main())
