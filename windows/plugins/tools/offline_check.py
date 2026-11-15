# SPDX-License-Identifier: GPL-2.0-only
"""Offline checks of the bundles the Windows mapper built, with upstream's own code and data.

  python -X utf8 offline_check.py [--bundles DIR] [--store DIR] [--specs DIR] --reference OPEN-APOLLO
                                  [--only NAME ...] [--defaults] [--json FILE]

Per bundle, upstream's code (unmodified, tools/open-apollo/mixer-engine):
  recipe     ProfiledCarrierProvider / StereoProfiledCarrierProvider builds the CREATE recipe
             (GeneratedOrdinaryCarrier: every resource read from the store by SHA-256 and checked
             against the Bill loader contract, rows, the driver's WRITE check, the startup event
             replay equal to the shadow words); create_arguments() must fit one command page
             (1016 words, no staging), as gate P1 needs.
  panel      plugin-qualify.py's panel_check against the pinned panel spec.
  defaults   (--defaults) the emulator starts at load and learns values and texts (upstream
             learn_defaults; writes <bundle>.defaults.json next to the bundle, as the daemon does).
Against upstream reference data (--reference, the open-apollo checkout):
  known-good layouts, latency and dynamic_refs; parameter counts and panel results from
  docs/status/plugins/<slug>.json (measured upstream on 2026-10-20); the DLL hashes upstream's
  art recipes list; render id against UA's own manifest/UA.xml in the module store manifest;
  strip modules against the firmware pins.
Upstream evidence for a DLL that is not the build upstream mapped (added Nov 15 2026; UA's 11.4 framework):
  steps      every stepped spec parameter: maximum = step count - 1, and the plug-in's own formatter
             texts at the values it holds after each set are the spec's step labels (the spec is the Console's own list,
             taken from a newer UAD)
  art        the DLL resources upstream's art recipe lists, by SHA-256: the same in this DLL means the
             plug-in's own release is the one upstream drew (its code was rebuilt on another framework)
  pinned     plug-in modules whose id has a firmware pin (UAD 12.0.0.602): same SHA-256
  genuine    modules (id, word 2, length, word 4) a genuine UA CREATE uploaded on upstream's x4, where
             upstream recorded them (driver/tests/test_plugin_upload.py): all among this recipe's modules,
             and that CREATE's private-row count (HARDWARE-SESSIONS.md) equal to this descriptor's
  dyn_refs   the known-good DSP-owned word positions (row, word) exist in this recipe's private rows
Nothing opens a device, a socket or a UA program; DLLs are only read (and emulated with --defaults).
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path

import oa_offline as oa

GATE_P1 = ['UAD API Preamp', 'UAD API 560', 'UAD API 550A', 'UAD Avalon VT-737sp', 'UAD Bermuda Triangle',
           'UAD Pultec HLF-3C', 'UAD dbx 160', 'UAD Precision De-Esser', 'UAD UA 610-B', 'UAD Neve 1073']


def qualify_module():
    spec = importlib.util.spec_from_file_location('plugin_qualify', oa.VENDOR / 'tools/plugin-qualify.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def upstream_dll_hashes(reference):
    out = {}
    for f in glob.glob(str(Path(reference) / 'console-rs/crates/oa-assets/data/plugin-recipes/*.json')):
        for v in json.loads(Path(f).read_text(encoding='utf-8')).get('inputs', {}).values():
            if v.get('kind') == 'dll':
                out.setdefault(v['file'], set()).update(v['sha256'] if isinstance(v['sha256'], list) else [v['sha256']])
    return out


def upstream_status(reference, name):
    p = Path(reference) / 'docs/status/plugins' / (oa.slug(name) + '.json')
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else None


def upstream_art(reference):
    """{dll file: {resource 'type/name/lang': sha256}} from upstream's art recipes (the newer build's resources)."""
    out = {}
    for f in glob.glob(str(Path(reference) / 'console-rs/crates/oa-assets/data/plugin-recipes/*.json')):
        r = json.loads(Path(f).read_text(encoding='utf-8'))
        dlls = {k: v['file'] for k, v in r.get('inputs', {}).items() if v.get('kind') == 'dll'}
        for b in r.get('blobs', {}).values():
            if b.get('in') in dlls and 'res' in b:
                out.setdefault(dlls[b['in']], {})[b['res']] = b['sha']
    return out


def dll_resources(path):
    """{'type/name/lang': sha256} of a DLL's resources (pefile; bytes only)."""
    import pefile
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    out = {}
    for t in (pe.DIRECTORY_ENTRY_RESOURCE.entries if hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE') else []):
        for n in t.directory.entries:
            for lang in n.directory.entries:
                d = lang.data.struct
                key = f"{t.id if t.id is not None else t.name}/{n.id if n.id is not None else n.name}/{lang.id}"
                out[key] = hashlib.sha256(pe.get_data(d.OffsetToData, d.Size)).hexdigest()
    pe.close()
    return out


def genuine_uploads(reference):
    """{plug-in: [(id, w2, length, w4)]}: modules genuine UA CREATEs uploaded on upstream's x4, as upstream's
    driver tests record them (test_titan_1176_replies: the UA 1176LN Legacy, 2026-10-11)."""
    p = Path(reference) / 'driver/tests/test_plugin_upload.py'
    if not p.is_file():
        return {}
    text = p.read_text(encoding='utf-8')
    body = text[text.find('def test_titan_1176_replies'):]
    body = body[:body.find('\ndef ', 1)]
    mods = [(int(a, 16), int(b, 16), int(c), int(d)) for a, b, c, d in
            re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (\d+), (\d+), \{', body)]
    if not mods or '1176LN Legacy' not in body:
        return {}
    # The same CREATE's private rows (HARDWARE-SESSIONS.md, "Native 1176 CREATE on DSP 3")
    s = Path(reference) / 'HARDWARE-SESSIONS.md'
    m = re.search(r'Native 1176 CREATE.{0,400}?(\d+) private rows', s.read_text(encoding='utf-8'), re.S) if s.is_file() else None
    return {'UAD UA 1176LN Legacy': dict(modules=mods, rows=int(m.group(1)) if m else None)}


def firmware_pins():
    pins = json.loads(oa.PINS.read_text(encoding='utf-8'))
    return {int(m['id'], 16): {k['sha256'] for k in m['known']} for m in pins['dsp_modules']}


def upstream_evidence(r, data, a, p, spec, args, kg_entry, art, genuine, pins, manifest):
    """The checks against upstream for a DLL upstream did not map (docstring: steps, art, pinned, genuine,
    dyn_refs). Adds to r['checks'] and r['evidence']."""
    ok = lambda key, good, detail: r['checks'].__setitem__(key, dict(result='pass' if good else 'fail', detail=detail))
    name = r['name']
    ev = r.setdefault('evidence', {})
    # steps: the plug-in's own domain and texts against the Console's step lists
    if spec is not None:
        bad, stepped, held = [], 0, set()
        for c in data['control_sweep']:
            sp = spec['parameters'][c['index']] if c['index'] < len(spec['parameters']) else None
            steps = (sp or {}).get('steps')
            if not steps:
                continue
            stepped += 1
            if c['maximum'] != len(steps) - 1:
                bad.append(f"{c['index']}: maximum {c['maximum']}, spec {len(steps)} steps")
                continue
            for s in c['samples']:
                # The text names the value the plug-in holds after the set: its own setter may keep another one
                # (a pad in Line mode, a linked channel's control in a mono insert), which the sweep reads back.
                value = s.get('readback', s['integer'])
                if value != s['integer']:
                    held.add(c['index'])
                text = (s.get('formatted') or {}).get('console_trimmed')
                if 0 <= value < len(steps) and text is not None and text.strip() != str(steps[value]).strip():
                    bad.append(f"{c['index']}@{value}: '{text}' vs spec '{steps[value]}'")
        continuous = len(data['control_sweep']) - stepped
        note = f'; {len(held)} held another value in this layout (ordinals {sorted(held)})' if held else ''
        ok('steps', not bad, f'{stepped} stepped parameters: step counts and own texts equal the spec labels; '
                             f'{continuous} continuous{note}' if not bad else '; '.join(bad[:6]))
        ev['stepped'], ev['continuous'], ev['held'] = stepped, continuous, sorted(held)
    # art: the newer build's resources, by hash
    want = art.get(f'{name}.dll')
    if want:
        mine = dll_resources(data['dll']['path'])
        same = sum(mine.get(k) == v for k, v in want.items())
        ok('art', same == len(want), f'{same}/{len(want)} DLL resources upstream\'s art recipe lists are byte-identical here')
        ev['art'] = [same, len(want)]
    # pinned: plug-in modules that are also pinned firmware modules
    hits = [(x['id'], x['sha256']) for x in data['descriptor']['resources'] if x['id'] in pins]
    if hits:
        same = [i for i, h in hits if h in pins[i]]
        ok('pinned', len(same) == len(hits), f'{len(same)}/{len(hits)} plug-in modules with a firmware pin '
                                             f"(ids {', '.join(f'{i:03x}' for i, _ in hits)}) equal UAD 12.0.0.602's")
        ev['pinned'] = [len(same), len(hits)]
    # genuine: (id, w2, length, w4) of a genuine CREATE's uploads, upstream's record
    if name in genuine:
        # the store manifest keeps (id, w2, length, w4) per DLL record
        plug = next((x for x in manifest['plugins'] if x['name'] == name), {})
        used = {x.hex() for x in a['resources']}
        mine = {(int(m['id'], 16), m['w2'], m['length'], m['w4']) for m in plug.get('modules', ()) if m['sha256'] in used}
        want, rows = genuine[name]['modules'], genuine[name]['rows']
        found = [g for g in want if g in mine]
        own_rows = len(data['descriptor']['rows'])
        good = len(found) == len(want) and rows in (None, own_rows)
        ok('genuine', good, f"{len(found)}/{len(want)} modules a genuine UA CREATE uploaded upstream (id, w2, length, w4) "
                            f"are this recipe's ({', '.join(f'{g[0]:03x}' for g in found)}); its private rows {rows}, "
                            f"this descriptor's {own_rows}")
        ev['genuine'] = [len(found), len(want), rows, own_rows]
    # dyn_refs: known-good DSP-owned positions inside this recipe's rows
    refs = kg_entry.get('dynamic_refs') or []
    if refs:
        rows = a['private_rows']          # (pool, dwords, link, flags); rows 10.. are the plug-in's
        out = [f'{rw}/{wd}' for rw, wd in refs if not 10 <= rw < len(rows) or wd >= rows[rw][1]]
        ok('dyn_refs', not out, f'{len(refs)} known-good DSP-owned positions inside the {len(rows)} rows' if not out
           else f'outside the rows: {out[:8]}')
        ev['dynamic_refs'] = len(refs)


def check_bundle(entry, args, nc, prov, manifest, kg, specs, q, up_dlls, reference):
    name, layout = entry['name'], entry['layout']
    path = Path(args.bundles) / entry['file']
    r = dict(name=name, layout=layout, file=entry['file'], checks={}, mismatches=[])
    ok = lambda key, good, detail: r['checks'].__setitem__(key, dict(result='pass' if good else 'fail', detail=detail))
    try:
        p, data = oa.provider_for(prov, path, args.store)
    except Exception as e:   # upstream's own recipe build refused it
        ok('recipe', False, f'{type(e).__name__}: {e}')
        return r
    d = data['descriptor']
    try:
        a = p.create_arguments()
        words = len(a['commands'])
        ok('recipe', True, f"{len(a['private_rows'])} rows, {len(a['resources'])} modules, {words} command words "
                           f'(one page: <= {nc.MAX_COMMAND_WORDS}), {nc.check(a["private_rows"], list(a["commands"]))} WRITEs')
    except ValueError as e:
        ok('recipe', False, f'create_arguments: {e} (needs staging; gate P1 wants one page)')
        a = p.composition_arguments()
    r['render_id'] = d['render_id']
    r['command_words'] = len(a['commands'])
    r['rows'] = len(a['private_rows'])
    r['modules'] = [dict(sha256=x['sha256'], id=x['id'], arch=x['architecture'], kind=x['kind'], words=x['words'],
                         bytes=x['bytes']) for x in p.recipe.resources]
    r['observations'] = sum(len(v) for _, _, v in p.observations())
    r['latency_samples'] = d['latency_samples']
    r['parameters'] = len(data['control_sweep'])
    # render id: licence table index 0..767, and UA's own registry for this DLL
    m = next((x for x in manifest['plugins'] if x['dll'] == f'{name}.dll'), None)
    ua_ids = sorted({x['renderid'] for x in (m or {}).get('ua_xml', ())})
    good = 0x800 <= d['render_id'] <= 0xaff and m is not None and m.get('render_id') == d['render_id']
    ok('render_id', good, f"{d['render_id']} (0x{d['render_id']:x}); UA manifest {m.get('render_id') if m else None}, UA.xml {ua_ids}")
    # modules: every recipe resource in the store and carried by this DLL (or UA's system DLLs)
    carried = {x['sha256'] for x in (m or {}).get('modules', ())} | {x['sha256'] for s in manifest['system'] for x in s['modules']}
    missing = [x['sha256'][:12] for x in r['modules'] if x['sha256'] not in carried or not (Path(args.store) / 'modules' / (x['sha256'] + '.bill')).is_file()]
    ok('modules', not missing, f"{len(r['modules'])} plug-in modules, all in the store and in this DLL" if not missing else f'not found: {missing}')
    strip = {x['pinned_as']: x['sha256'] for x in manifest['modules'] if x.get('verified')}
    hashes = [h.hex() for h in a['resources'][:5]]
    ok('strip_modules', all(h in strip.values() for h in hashes), 'the 5 Mic Input Strip modules match the firmware pins')
    # upstream references
    e = kg.get(name, {})
    ok('known_good', layout in e.get('layouts', ()), f"family {e.get('family')}, layouts {e.get('layouts')}")
    good = d['latency_samples'] == e.get('latency_samples')
    ok('latency', good, f"{d['latency_samples']} samples (known-good {e.get('latency_samples')}, source {data['latency_source']})")
    if e.get('dynamic_refs'):
        r['mismatches'].append(f"known-good lists {len(e['dynamic_refs'])} DSP-owned words (dynamic_refs)")
    st = upstream_status(reference, name)
    up_map = ((st or {}).get('layouts', {}).get(layout, {}).get('map') or {}).get('detail')
    mine = f"{r['parameters']} parameters"
    ok('parameters', up_map == mine, f'{mine} (upstream {up_map})')
    spec = specs.get(name)
    if spec is None:
        ok('panel', False, 'no panel spec')
    else:
        res, detail = q.panel_check(spec, r['parameters'])
        up_panel = ((st or {}).get('layouts', {}).get(layout, {}).get('panel') or {}).get('detail')
        ok('panel', res == 'pass' and (up_panel is None or up_panel == detail), f'{detail} (upstream {up_panel})')
    dll_sha = data['dll']['sha256']
    known = up_dlls.get(f'{name}.dll', set())
    r['dll'] = dict(sha256=dll_sha, version=(m or {}).get('version'), identical_to_upstream=dll_sha in known)
    if not r['dll']['identical_to_upstream']:
        r['mismatches'].append(f"DLL differs from the build upstream's recipes list ({(m or {}).get('version')})")
    layout_name = ((data.get('profile') or {}).get('framework_layout') or {}).get('name')
    if layout_name:
        r['framework_layout'] = layout_name
    upstream_evidence(r, data, a, p, spec, args, e, args.art, args.genuine, args.pins, manifest)
    if args.defaults:
        try:
            p.learn_defaults()
            values, texts = p.state.values, p._texts[1] if p._texts else None
            ok('defaults', len(values) == len(p.maxima), f'emulator started; {len(values)} values, texts: {list(texts or [])[:4]}...')
        except Exception as ex:
            ok('defaults', False, f'{type(ex).__name__}: {ex}')
    r['mismatches'] += [f"{k}: {v['detail']}" for k, v in r['checks'].items() if v['result'] != 'pass']
    return r


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bundles', type=Path, default=oa.BUNDLES)
    ap.add_argument('--store', type=Path, default=oa.STORE)
    ap.add_argument('--specs', type=Path, default=oa.SPECS)
    ap.add_argument('--reference', type=Path, required=True, help='the open-apollo checkout (read only)')
    ap.add_argument('--only', action='append')
    ap.add_argument('--gate', action='store_true', help='only the gate P1 candidates')
    ap.add_argument('--defaults', action='store_true', help='also start the emulator and learn load-time values/texts')
    ap.add_argument('--json', type=Path)
    args = ap.parse_args(argv)
    nc, prov, index = oa.load(args.store, args.bundles)
    manifest, kg, q = oa.store_manifest(args.store), oa.known_good(), qualify_module()
    specs = q.load_specs(str(args.specs))
    up_dlls = upstream_dll_hashes(args.reference)
    args.art, args.genuine, args.pins = upstream_art(args.reference), genuine_uploads(args.reference), firmware_pins()
    want = set(args.only or []) | (set(GATE_P1) if args.gate else set())
    results = []
    for entry in sorted(index['bundles'], key=lambda e: (e['name'], e['layout'])):
        if want and entry['name'] not in want:
            continue
        r = check_bundle(entry, args, nc, prov, manifest, kg, specs, q, up_dlls, args.reference)
        results.append(r)
        fails = [k for k, v in r['checks'].items() if v['result'] != 'pass']
        print(f"{r['name']} ({r['layout']}): {'PASS' if not fails else 'FAIL ' + ', '.join(fails)}")
        for k, v in r['checks'].items():
            print(f"   {k:14s} {v['result']:4s}  {v['detail']}")
        for mm in r['mismatches']:
            if not any(mm.startswith(k + ':') for k in r['checks']):
                print(f'   note           {mm}')
    failures = index.get('failures', {})
    for name in sorted(failures):
        if want and name not in want:
            continue
        print(f"{name}: not mapped: {'; '.join(f'{k}: {v}' for k, v in failures[name].get('reasons', {}).items())}")
    if args.json:
        args.json.write_text(json.dumps(dict(results=results, failures={k: v for k, v in failures.items()
                                                                     if not want or k in want}), indent=1), encoding='utf-8')
    print(f"{len(results)} bundles checked, {sum(not any(v['result'] != 'pass' for v in r['checks'].values()) for r in results)} pass every check")
    return 0


if __name__ == '__main__':
    sys.exit(main())
