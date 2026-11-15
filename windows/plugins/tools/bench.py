# SPDX-License-Identifier: GPL-2.0-only
"""What gate P1 needs from one mapped bundle, printed from upstream's own recipe code.

  python -X utf8 bench.py "UAD API Preamp" [--layout mono|stereo] [--input 0] [--bundles DIR] [--store DIR]
                          [--json FILE] [--words]

Prints: the modules the CREATE uploads (SHA-256, id, kind, architecture, pool, words, bytes);
the render id and its licence-table index; the chain head and every private row (pool, dwords,
link, flags) with the head's words; the parameter list (name, maximum, default, steps, the
plug-in's own texts at 0 / middle / maximum); latency and the record delay the recipe writes;
command size against the one-page limit. --words also dumps the CREATE command words.
Built by ProfiledCarrierProvider (tools/open-apollo/mixer-engine), as the daemon builds it;
nothing opens a device, a socket or a UA program.
"""
import argparse
import json
import sys
from pathlib import Path

import oa_offline as oa

POOL_OF_KIND = {0: 0, 1: 1, 2: 2, 3: 3, 4: 1}
HEAD_WORDS = {0: 'header 0x40000000', 1: 'in channels', 2: 'out channels', 4: 'flags (0x22)', 5: 'members N',
              6: 'link: Mic Input Strip m0', 7: 'Mic carrier id 0x846', 8: 'link: effect root row', 9: 'effect render id'}
ROW_NAMES = ['chain head', 'Mic m0', 'Mic m1', 'Mic m2', 'Mic m3', 'Mic m4', 'Mic c0', 'Mic c1', 'Mic c2', 'Mic c3']


def link(v):
    return 'none' if v == 0xffffffff else f'row {v >> 24} word {v & 0xffffff}'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('name')
    ap.add_argument('--layout', choices=('mono', 'stereo'), default='mono')
    ap.add_argument('--input', type=int, default=0, help='tree input (0 = input 1)')
    ap.add_argument('--bundles', type=Path, default=oa.BUNDLES)
    ap.add_argument('--store', type=Path, default=oa.STORE)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--words', action='store_true')
    args = ap.parse_args(argv)
    nc, prov, index = oa.load(args.store, args.bundles)
    entry = next((e for e in index['bundles'] if e['name'] == args.name and e['layout'] == args.layout), None)
    if entry is None:
        why = index.get('failures', {}).get(args.name, {}).get('reasons')
        sys.exit(f'no {args.layout} bundle for {args.name}' + (f' (not mapped: {why})' if why else ''))
    path = args.bundles / entry['file']
    p, data = oa.provider_for(prov, path, args.store, args.input)
    d = data['descriptor']
    try:
        a, one_page = p.create_arguments(), True
    except ValueError:
        a, one_page = p.composition_arguments(), False
    kg = oa.known_good().get(args.name, {})
    out = dict(name=args.name, layout=args.layout, bundle=str(path), dll=data['dll'], input_pair=list(a['input_pair']))

    print(f"{args.name} ({args.layout}), bundle {path.name}")
    print(f"  DLL sha256 {data['dll']['sha256']}")
    rid = d['render_id']
    print(f'\nRender id: {rid} (0x{rid:x}); licence table index {rid - 0x800} (render id - 0x800)')
    out['render_id'] = rid

    print('\nModules (CREATE resources, in upload order):')
    mods = []
    strips = {bytes.fromhex(x['sha256']): x for x in oa.store_manifest(args.store)['modules'] if x.get('verified')}
    for h in a['resources'][:5]:
        x = strips.get(h)
        size = (args.store / 'modules' / (h.hex() + '.bill')).stat().st_size
        mods.append(dict(sha256=h.hex(), id=x['id'] if x else '?', kind=x['kind'] if x else None, arch=x['arch'] if x else None,
                         bytes=size, role='Mic Input Strip (pinned)'))
    for r in p.recipe.resources:
        mods.append(dict(sha256=r['sha256'], id=f"{r['id']:03x}", kind=r['kind'], arch=r['architecture'],
                         pool=r['pool'], words=r['words'], bytes=r['bytes'], role='plug-in'))
    for m in mods:
        pool = m.get('pool', POOL_OF_KIND.get(m['kind']))
        words = f"{m['words']:>6} words" if 'words' in m else ' ' * 12
        print(f"  {m['sha256']}  id {m['id']}  kind {m['kind']}  arch {m['arch']}  pool {pool}  {words}  {m['bytes']:>7} bytes  {m['role']}")
    code = sum(m['words'] for m in mods if m['role'] == 'plug-in' and m['kind'] == 0)
    print(f"  plug-in code in pool 0: {code} words; {len(mods)} modules (limit 64)")
    out['modules'] = mods

    print('\nChain head and private rows (pool, dwords, link, flags):')
    rows = a['private_rows']
    for i, (pool, size, lk, flags) in enumerate(rows):
        label = ROW_NAMES[i] if i < 10 else f'effect row {i - 10}' + (' (root)' if i == 10 else '')
        print(f'  row {i:3d}  pool {pool}  {size:6d} dwords  link {link(lk):22s} flags {flags}  {label}')
    head = {0: 0x40000000, 1: 1, 2: 1, 4: 0x22, 5: 2, 7: nc.MIC_CARRIER, 9: rid}
    if args.layout == 'stereo':
        head.update({1: 2, 2: 2})
    print('  head words: ' + ', '.join(f'[{w}] {HEAD_WORDS[w]} = 0x{v:x}' for w, v in sorted(head.items()))
          + '; [6] and [8] are links (memspec), [3] unused')
    print(f"  input pair {tuple(a['input_pair'])} (in2 0x20 = mono); dsp {a['dsp']}; scratch {a['scratch']}")
    out['rows'] = [list(r) for r in rows]
    out['head_words'] = {str(k): v for k, v in head.items()}

    print('\nParameters:')
    params = []
    meta = {}
    for c in data['parameter_metadata']['controls']:
        i, field = int(c['path'].split('/parameters/')[1].split('/')[0]), c['path'].rsplit('/', 1)[1]
        meta.setdefault(i, {})[field] = c
    for s in data['control_sweep']:
        i, m = s['index'], meta.get(s['index'], {})
        texts = [x['formatted'].get('console_trimmed', x['formatted']) if isinstance(x['formatted'], dict) else x['formatted']
                 for x in s['samples']]
        steps = (m.get('StepValue') or {}).get('values')
        default = (m.get('NormalizedValue') or {}).get('value')
        name = (m.get('Name') or {}).get('value')
        params.append(dict(index=i, name=name, maximum=s['maximum'], default=default, steps=steps,
                           sweep=[[x['integer'], t] for x, t in zip(s['samples'], texts)]))
        sweep = ', '.join(f'{x["integer"]}: {t!r}' for x, t in zip(s['samples'], texts))
        print(f"  {i:3d}  {name!s:28s} max {s['maximum']:>6}  default {default}  "
              f"{'steps ' + str(len(steps)) if steps else 'continuous'}  [{sweep}]")
    out['parameters'] = params

    lat = d['latency_samples']
    print(f"\nLatency: {lat} samples at 48 kHz (source {data['latency_source']}; known-good {kg.get('latency_samples')}); "
          f'the standalone recipe writes record delay {p.recipe.record_delay} at Mic m0 word 16 '
          f'(budget {200 if lat <= 200 else 1000} minus latency, floor 0; a chain composer sets it from the compensation plan)')
    print(f"Commands: {len(a['commands'])} words ({'fits one page' if one_page else 'NEEDS STAGING'}, limit {nc.MAX_COMMAND_WORDS}); "
          f"{len(p.observations())} readback runs, {sum(len(v) for _, _, v in p.observations())} words observed after CREATE")
    if kg.get('dynamic_refs'):
        print(f"DSP-owned words (not verified): {kg['dynamic_refs']}")
    out.update(latency_samples=lat, record_delay=p.recipe.record_delay, command_words=len(a['commands']), one_page=one_page)
    if args.words:
        print('\nCREATE commands:')
        cmds = list(a['commands'])
        for i in range(0, len(cmds), 8):
            print('  ' + ' '.join(f'{w:08x}' for w in cmds[i:i + 8]))
        out['commands'] = cmds
    if args.json:
        args.json.write_text(json.dumps(out, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
