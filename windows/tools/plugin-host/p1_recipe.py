# SPDX-License-Identifier: GPL-2.0-only
"""Gate P1 recipe: one mapped bundle -> the CREATE requests apollo-plug sends (docs/PLUGINS.md 4.1).

  python -I -X utf8 p1_recipe.py "UAD Oxford SuprEsser DS" [--layout mono|stereo] [--input 0]
         [--unit x8p|twinx] [--plugins DIR] [--out FILE] [--authorized FILE] [--renderids FILE]
         [--effect auto] [--record-delay 200|carrier]

Built by open-apollo's own carrier code (ProfiledCarrierProvider.create_arguments(), as the
daemon builds a CREATE), through apollo-win\\plugins\\tools\\oa_offline.py: the chain recipe
(Mic carrier + the plug-in, START + CHAIN, input `--input`, mono), the plug-in's own modules and
descriptor rows for the unrouted CREATE, and the readback runs (provider.observations()).

A startup over one 1016-word page (vst2_bx, Lexicon 480L): the whole chain recipe as the carrier
composes it (composition_arguments(), what its stage_bundle() splits), with upstream's own page cut
as "chunk <word offset> <words>" lines (native_carrier.chunks(blocks()), cross-checked against the
carrier's startup_chunks). apollo-plug sends page 0 with CREATE STAGE+CHAIN, the rest as SERVICE
writes, reads the startup back, then routes it with START (plugin_native_chain.py:323-371).

--effect (default auto): how step-p1 proves the plug-in processes input 1. tail: a reverb or echo
(the decay after claps or other short sounds grows); delay: the record lag shifts by the latency;
level: the level or spectrum of input 1 against the same source on input 2 changes (amps, EQs,
dynamics at latency 0). auto: tail for reverbs and echoes by name, else delay when the plug-in
has latency, else level.

Run it from a NORMAL (not elevated) prompt: it reads the bundle and the module store and writes
one text file. Nothing here opens a device, a socket or a UA program. The recipe is derived from
UA's files: it stays on this PC (plugins\\p1\\ by default), never in the repo or the installer.

--authorized FILE: one plug-in name per line (apollo-plug licence --authorized-out writes it from
the x8p's own answer); the recipe is refused for a plug-in not in it. --renderids FILE: writes
"0x<render id> <name>" for every plug-in in the store manifest (apollo-plug licence --names).

--unit twinx (P7): a recipe for the Twin X Gen 1: inputs 1-2 (--input 0 or 1; its Hi-Z jack is
input 1), stereo on the pair 1/2 only, written as <plug-in>-twinx-<layout>-in<n>.recipe. The words are the
x8p's for the same input (both units: analog input n = DSP input n-1 on the strip DSP, DSP 1); only the inputs
it may name differ. --authorized must then be the Twin X's own answer (apollo-plug --unit twinx licence).

--layout stereo: the stereo bundle on a linked pair (--input 0 = inputs 1/2, 2 = inputs 3/4), as
upstream's StereoProfiledCarrierProvider builds it (channel counts 2 in rows 0, 1 and 10; in2 = in1 + 1).
apollo.sys starts a stereo chain only with FEAT_STEREO.

dsp_owned lines: words the running DSP itself writes (meters, envelopes), which a
readback must not compare: "dsp_owned <chain row> <word> <source>", source bundle (the bundle's
dynamic_observation_refs, from upstream's known-good list; provider.observations() already leaves
them out), meter (a meter word of the plug-in's panel spec, meterSources vendorSpecificFloat, inside
the plug-in's rows) or hardware (plugins\\p1\\dsp-owned.txt: words a hardware run saw move, one
"<plug-in name> | <row> <word> | <note>" per line). apollo-plug and Apollo Control skip them.

--record-delay N (default 200): the chain's record delay (Mic m0 word 16). Upstream's carrier
writes 200 - latency (22 for the SuprEsser), which compensates the plug-in exactly against the
plain strip's 200 (strip_plan.h m0 word 16 = 0xc8): the record path then does not move and the
plug-in's latency cannot be seen on the record slots. With 200 the record lag of the input shifts
by exactly the plug-in's latency while the chain runs, which proves it processes audio
(docs/PLUGINS.md 4.1 "Pass"). --record-delay carrier keeps upstream's value.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGE_WORDS = 1016                                   # one SERVICE / START page (apollo_plugin.h, native_carrier.py:15)
# reverbs, echoes and delays: their effect is a tail (names as UA.xml spells them)
TAIL_NAMES = re.compile(r'480L|RMX16|Plate|Verb|Reverb|Room|Hall|Chamber|Spring|Echo|Delay|DMX|Ocean Way|Capitol|Cooper Time',
                        re.IGNORECASE)


def effect_mode(name, latency, asked):
    if asked != 'auto':
        return asked
    if TAIL_NAMES.search(name):
        return 'tail'
    return 'delay' if latency > 0 else 'level'


def dsp_owned(plugins, name, data):
    """(row, word, source) the running DSP writes itself: the bundle's dynamic refs, the panel
    spec's meter words inside the plug-in's rows, and the hardware list plugins\\p1\\dsp-owned.txt."""
    rows = data['descriptor']['rows']
    found = {}
    for row, word in data.get('dynamic_observation_refs', ()):
        found.setdefault((int(row), int(word)), 'bundle')
    meta = ((data.get('profile') or {}).get('captured_metadata') or {}).get('path') or ''
    stem = re.split(r'[\\/]', meta)[-1]
    if stem.lower().endswith('.json'):
        stem = stem[:-5]
    spec = plugins / 'specs' / f'PluginSpec_{stem}.json'
    if stem and spec.is_file():
        try:
            meters = json.loads(spec.read_text(encoding='utf-8')).get('meterSources') or {}
        except ValueError:
            meters = {}
        for m in meters.values():
            ref = m.get('vendorSpecificFloat') if isinstance(m, dict) else None
            if type(ref) is int and 0 <= ref <= 0xffffffff:
                r, w = ref >> 24, ref & 0xffffff
                if r < len(rows) and w < rows[r][1]:
                    found.setdefault((10 + r, w), 'meter')
    hw = plugins / 'p1' / 'dsp-owned.txt'
    if hw.is_file():
        for line in hw.read_text(encoding='utf-8').splitlines():
            parts = [x.strip() for x in line.split('|')]
            if len(parts) >= 2 and parts[0] == name and not line.lstrip().startswith('#'):
                rw = parts[1].split()
                if len(rw) == 2 and rw[0].isdigit() and rw[1].isdigit():
                    found.setdefault((int(rw[0]), int(rw[1])), 'hardware')
    return sorted((r, w, src) for (r, w), src in found.items())


def find_plugins(arg):
    if arg:
        return Path(arg).resolve()
    root = HERE.parent.parent                       # apollo-win, or builds\<work>
    for cand in (root / 'plugins', root.parent.parent / 'plugins'):
        if (cand / 'tools' / 'oa_offline.py').is_file():
            return cand
    sys.exit('apollo-win\\plugins not found: pass --plugins DIR')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('name')
    ap.add_argument('--layout', choices=('mono', 'stereo'), default='mono',
                    help='stereo: the stereo bundle on the pair starting at --input (0 or 2)')
    ap.add_argument('--input', type=int, default=0, help='analog input index (0 = input 1)')
    ap.add_argument('--unit', choices=('x8p', 'twinx'), default='x8p',
                    help='twinx: the Twin X Gen 1 (inputs 0-1, the pair 0/1)')
    ap.add_argument('--plugins', help='apollo-win\\plugins (store, bundles, tools)')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--authorized', type=Path)
    ap.add_argument('--renderids', type=Path)
    ap.add_argument('--record-delay', default='200', help="0..2000, or 'carrier' (upstream's 200 - latency)")
    ap.add_argument('--effect', choices=('auto', 'tail', 'delay', 'level'), default='auto',
                    help='how step-p1 measures the effect on input 1')
    args = ap.parse_args(argv)
    if sys.flags.utf8_mode != 1:
        sys.exit('run with python -X utf8 (the bundles and specs are UTF-8)')
    if not 0 <= args.input <= 3:
        sys.exit('--input: 0..3 (the carrier maps tree inputs 0-3 to DSP inputs 0-3; x8p inputs 1-4)')
    if args.layout == 'stereo' and args.input not in (0, 2):
        sys.exit("--layout stereo: --input 0 (inputs 1/2) or 2 (inputs 3/4), upstream's STEREO_FIRST")
    unit = 'Twin X' if args.unit == 'twinx' else 'x8p'
    if args.unit == 'twinx' and (args.input > 1 or (args.layout == 'stereo' and args.input != 0)):
        sys.exit('--unit twinx: --input 0 or 1 (inputs 1-2; stereo: 0, the pair 1/2): the Twin X has 2 strip inputs')

    plugins = find_plugins(args.plugins)
    sys.path.insert(0, str(plugins / 'tools'))
    import oa_offline as oa                         # noqa: E402  (vendored upstream carrier, offline)

    store, bundles = plugins / 'store', plugins / 'bundles'
    nc, prov, index = oa.load(store, bundles)
    manifest = oa.store_manifest(store)

    if args.renderids:
        lines = sorted({(p['render_id'], p['name']) for p in manifest['plugins'] if p.get('render_id') and p.get('name')})
        args.renderids.parent.mkdir(parents=True, exist_ok=True)
        args.renderids.write_text(''.join(f'0x{rid:03x} {name}\n' for rid, name in lines), encoding='utf-8')
        print(f'{len(lines)} render ids -> {args.renderids}')

    if args.authorized:
        names = {l.strip() for l in args.authorized.read_text(encoding='utf-8').splitlines() if l.strip()}
        if args.name not in names:
            sys.exit(f'{args.name} is not in {args.authorized} (the {unit} does not authorize it): no recipe')

    entry = next((e for e in index['bundles'] if e['name'] == args.name and e['layout'] == args.layout), None)
    if entry is None:
        why = index.get('failures', {}).get(args.name, {}).get('reasons')
        sys.exit(f'no {args.layout} bundle for {args.name}' + (f' (not mapped: {why})' if why else ''))
    path = bundles / entry['file']
    p, data = oa.provider_for(prov, path, store, args.input)
    try:
        a, staged = p.create_arguments(), False
    except ValueError:
        # over one page: the carrier's complete recipe, staged as its stage_bundle() stages it
        a, staged = p.composition_arguments(), True
    d = data['descriptor']
    in1, in2 = a['input_pair']
    if (args.layout == 'mono') != (in2 == 32) or (args.layout == 'stereo' and in2 != in1 + 1):
        sys.exit(f'input pair {in1}/{in2} does not match the {args.layout} layout')
    cmds = list(a['commands'])
    delay = p.recipe.record_delay
    if args.record_delay != 'carrier':
        delay = int(args.record_delay, 0)
        if not 0 <= delay <= 2000:
            sys.exit('--record-delay: 0..2000 (the chain Mic strip holds 2048 samples)')
        # the carrier's own block: {0x00140004, row 1 << 24 | 16, mask 0, delay} (native_carrier.py:398)
        at = [i for i in range(len(cmds) - 3) if cmds[i] == 0x00140004 and cmds[i + 1] == (1 << 24) | 16 and cmds[i + 2] == 0
              and cmds[i + 3] == p.recipe.record_delay]
        if len(at) != 1:
            sys.exit('the record-delay WRITE was not found exactly once in the carrier recipe')
        cmds[at[0] + 3] = delay
    plain_delay = 200                               # strip_plan.h g_StripM0A: m0 word 16 = 0xc8
    shift = delay + d['latency_samples'] - plain_delay
    # the pages, cut by upstream's own code (the record-delay patch changes one value, not a length)
    pages = nc.chunks(nc.blocks(cmds))
    if sum(pages, ()) != tuple(cmds) or any(len(c) > PAGE_WORDS for c in pages):
        sys.exit('upstream\'s page cut does not reproduce the startup: no recipe')
    carrier_pages = getattr(p.recipe, 'startup_chunks', None)
    # (the stereo wrapper appends its channel-count WRITEs after the mono startup the carrier cut)
    if carrier_pages is not None and args.layout == 'mono' and [len(c) for c in carrier_pages] != [len(c) for c in pages]:
        sys.exit('the carrier\'s own startup_chunks differ from the page cut: no recipe')
    if args.layout == 'stereo':
        # upstream's stereo create_arguments() checks the mono startup's size, then appends its channel-count WRITEs
        # (Manley VoxBox: 1011 + 15 words): the page cut decides, as the driver takes at most 1016 words per page
        staged = len(pages) > 1
    if staged != (len(pages) > 1):
        sys.exit(f'staging ({staged}) and {len(pages)} page(s) disagree: no recipe')
    effect = effect_mode(args.name, d['latency_samples'], args.effect)
    owned = dsp_owned(plugins, args.name, data)
    if owned:
        print('DSP-owned words skipped by the readback: ' + ', '.join(f'row {r} word {w} ({src})' for r, w, src in owned))

    stem = f'{oa.slug(args.name)}-twinx' if args.unit == 'twinx' else oa.slug(args.name)
    out = args.out or (plugins / 'p1' / f'{stem}-{args.layout}-in{args.input + 1}.recipe')
    out.parent.mkdir(parents=True, exist_ok=True)
    L = ['apollo-plug-recipe 1',
         '# Derived from UA files on this PC: keep local (never the repo, the installer or a log).',
         f'name {args.name}',
         f'layout {args.layout}',
         f'bundle {path.name}',
         f'dll_sha256 {data["dll"]["sha256"]}',
         f'render_id 0x{d["render_id"]:x}',
         f'latency {d["latency_samples"]}',
         f'record_delay {delay}',
         f'# record path: chain {delay} + {d["latency_samples"]} against the plain strip\'s {plain_delay}',
         f'expect_shift {shift}',
         f'effect {effect}',
         f'synch {in1} {in2}',
         'scratch {} {} {}'.format(*a['scratch'])]
    if staged:
        L.append(f'# startup {len(cmds)} words in {len(pages)} pages: page 0 with CREATE STAGE+CHAIN, the rest by SERVICE, '
                 'then START (upstream chunks(blocks()))')
    at = 0
    for c in pages:
        L.append(f'chunk {at} {len(c)}')
        at += len(c)
    L.append('# chain: Mic Input Strip carrier (rows 0-9, the 5 pinned strip modules) + the plug-in')
    for h in a['resources']:
        L.append(f'resource {h.hex()}')
    for pool, size, link, flags in a['private_rows']:
        L.append(f'row {pool} {size} 0x{link:08x} {flags}')
    for i in range(0, len(cmds), 12):
        L.append('cmd ' + ' '.join(f'0x{w:08x}' for w in cmds[i:i + 12]))
    L.append('# unrouted: the plug-in alone (its own modules, architectures 0 and 2, and its descriptor rows)')
    for r in p.recipe.resources:
        L.append(f'plain_resource {r["sha256"]}')
    for pool, size, link, flags in p.recipe.local_rows:
        L.append(f'plain_row {pool} {size} 0x{link:08x} {flags}')
    for r in p.recipe.resources:
        L.append(f'module {r["sha256"]} 0x{r["id"]:03x} kind {r["kind"]} arch {r["architecture"]} pool {r["pool"]} '
                 f'words {r["words"]} bytes {r["bytes"]}')
    obs = p.observations()
    for row, word, values in obs:
        L.append(f'observe {row} {word} {len(values)} ' + ' '.join(f'0x{v:08x}' for v in values))
    for row, word, src in owned:
        L.append(f'dsp_owned {row} {word} {src}')
    out.write_text('\n'.join(L) + '\n', encoding='utf-8')

    code = sum(r['words'] for r in p.recipe.resources if r['kind'] == 0)
    where = f'inputs {args.input + 1}/{args.input + 2}' if args.layout == 'stereo' else f'input {args.input + 1}'
    print(f'{args.name} ({args.layout}, {unit} {where}): render id 0x{d["render_id"]:x}, '
          f'latency {d["latency_samples"]}, {len(a["resources"])} chain modules ({len(p.recipe.resources)} the plug-in\'s, '
          f'{code} code words), {len(a["private_rows"])} rows, {len(cmds)} command words '
          f'({"staged: " + str(len(pages)) + " pages" if staged else "one page"}), {len(obs)} readback runs')
    print(f'record delay {delay} (carrier {p.recipe.record_delay}): the record lag of input {args.input + 1} should shift '
          f'by {shift} samples while the chain runs; effect measured as: {effect}')
    print(f'recipe -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
