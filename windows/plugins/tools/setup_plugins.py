# SPDX-License-Identifier: GPL-2.0-only
"""[PlugSetup] Helpers for setup-plugins.ps1 (Set up UAD plug-ins on a release install).

Run by the setup script with the private runtime it installs, in isolated mode:

  python -I -X utf8 setup_plugins.py names   --manifest STORE\\manifest.json --ids 825,853 --out FILE
  python -I -X utf8 setup_plugins.py modules --bundles DIR --store DIR --pins firmware-pins.json --out REQUEST.json
  python -I -X utf8 setup_plugins.py summary --bundles DIR [--names FILE] [--only NAME ...]
  python -I -X utf8 setup_plugins.py selftest

names    The plug-in names whose render id a unit's licence answer admits (Apollo Control hands the admitted render
         ids over; the table itself never leaves it). A plug-in counts when its own render id or a UA.xml entry the
         module store matched to it (a collection: API 560 -> "API 500 EQ Collection") is admitted. The mapper's
         --authorized FILE: one name per line.
modules  The request apollo-control.exe --plugin-admin takes (plugin_engine.h AdminMain): every mapped bundle's
         architecture 0 and 2 modules by SHA-256, less the five Mic strip modules (include/apollo_strip_pins.h: they
         live in ApolloStrips), each checked to be in the store. Same list as Settings > Install modules builds.
summary  What the maps folder holds now, for the setup's last line: plug-ins and bundles mapped, failures (of the
         selection, when one is given) and plug-ins waiting for a licence.

Reads JSON written on this PC (the module store manifest, the mapper's index and bundles); writes one small file.
Nothing here opens a device, a socket or a UA program.
"""
import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

# The Mic Input Strip modules apollo.sys reads from ApolloStrips (include/apollo_strip_pins.h g_ApolloStripPins):
# their ids; the SHA-256s come from open-apollo's firmware-pins.json, which pins the same bytes.
STRIP_IDS = ('0eb', '0da', '0a5', '0c2', '12b')
HEX64 = re.compile(r'[0-9a-f]{64}')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write_text(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(text, encoding='utf-8', newline='\n')
    os.replace(tmp, path)


def parse_ids(text):
    """'825,0x853 2084' -> {0x825, 0x853, 0x2084?}: hex without or with 0x (Apollo Control writes plain hex)."""
    out = set()
    for tok in re.split(r'[\s,;]+', text or ''):
        if not tok:
            continue
        v = int(tok, 16)
        if not 0x800 <= v < 0xb00:
            raise ValueError(f'render id {tok} is outside the licence table (0x800-0xAFF)')
        out.add(v)
    return out


def authorized_names(manifest, ids):
    names = set()
    for p in manifest.get('plugins', []):
        name = p.get('name')
        if not name:
            continue
        rids = {p.get('render_id')} | {e.get('renderid') for e in p.get('ua_xml') or []}
        if rids & ids:
            names.add(name)
    return sorted(names)


def strip_pins(pins):
    out = set()
    for m in pins.get('dsp_modules', []):
        if str(m.get('id', '')).lower() in STRIP_IDS:
            out |= {k['sha256'] for k in m.get('known', []) if HEX64.fullmatch(k.get('sha256', ''))}
    return out


def module_list(bundles, store, pins):
    """(sorted SHA-256 list, missing in the store, bundles read)."""
    index = read_json(Path(bundles) / 'index.json')
    skip = strip_pins(pins)
    want, read = set(), 0
    for b in index.get('bundles', []):
        f = Path(bundles) / Path(b.get('file', '')).name
        if not f.is_file():
            continue
        data = read_json(f)
        read += 1
        for r in (data.get('descriptor') or {}).get('resources', []):
            sha = str(r.get('sha256', '')).lower()
            if r.get('architecture') in (0, 2) and HEX64.fullmatch(sha) and sha not in skip:
                want.add(sha)
    have = sorted(s for s in want if (Path(store) / 'modules' / f'{s}.bill').is_file())
    return have, sorted(want - set(have)), read


def summary(bundles, names=None, only=None):
    index_path = Path(bundles) / 'index.json'
    if not index_path.is_file():
        return dict(plugins=0, bundles=0, failed=[], waiting=0, selection=None)
    index = read_json(index_path)
    built = index.get('bundles', [])
    mapped = sorted({b['name'] for b in built if (Path(bundles) / b['file']).is_file()})
    selection = None
    if only:
        selection = set(only)
    elif names:
        selection = {n.strip() for n in Path(names).read_text(encoding='utf-8').splitlines() if n.strip()}
    failures = index.get('failures', {})
    failed = sorted(n for n in failures if (selection is None or n in selection) and n not in mapped)
    return dict(plugins=len(mapped), bundles=len(built), failed=[dict(name=n, why=_reason(failures[n])) for n in failed],
                waiting=len(index.get('waiting_for_licence', [])),
                selection=None if selection is None else len(selection),
                selected_mapped=None if selection is None else len(selection & set(mapped)))


def _reason(f):
    reasons = (f or {}).get('reasons') or {}
    return '; '.join(f'{k}: {v}' for k, v in reasons.items())[:300]


# ---------------------------------------------------------------- commands

def cmd_names(a):
    ids = parse_ids(a.ids)
    names = authorized_names(read_json(a.manifest), ids)
    write_text(a.out, ''.join(n + '\n' for n in names))
    print(json.dumps(dict(ids=len(ids), names=len(names))))
    return 0


def cmd_modules(a):
    have, missing, read = module_list(a.bundles, a.store, read_json(a.pins))
    req = {'hosting': '', 'user': '', 'from': str((Path(a.store) / 'modules').resolve()), 'modules': have}
    write_text(a.out, json.dumps(req, indent=1) + '\n')
    print(json.dumps(dict(modules=len(have), missing=len(missing), bundles=read)))
    return 0 if not missing else 1


def cmd_summary(a):
    print(json.dumps(summary(a.bundles, a.names, a.only)))
    return 0


def cmd_selftest(_a):
    """Synthetic store and maps (no UA data): names, modules and summary as the setup uses them."""
    failed = 0

    def check(ok, label):
        nonlocal failed
        failed += not ok
        print(('PASS  ' if ok else 'FAIL  ') + label)

    pins = {'format': 'open-apollo-firmware-pins/1', 'dsp_modules': [
        {'id': '0eb', 'known': [{'sha256': 'e' * 64}]}, {'id': '0db', 'known': [{'sha256': 'd' * 64}]}]}
    manifest = {'plugins': [
        {'name': 'UAD One', 'render_id': 0x825, 'ua_xml': [{'title': 'UAD One', 'renderid': 0x825}]},
        {'name': 'UAD Member', 'render_id': 0x854, 'ua_xml': [{'title': 'UAD Collection', 'renderid': 0x853}]},
        {'name': 'UAD Other', 'render_id': 0x900, 'ua_xml': []},
        {'name': None, 'render_id': 0x825}]}
    check(parse_ids('825, 0x853 ;900') == {0x825, 0x853, 0x900}, 'render ids: hex with or without 0x, any separator')
    try:
        parse_ids('7ff')
        check(False, 'a render id below 0x800 is refused')
    except ValueError:
        check(True, 'a render id below 0x800 is refused')
    check(authorized_names(manifest, {0x825, 0x853}) == ['UAD Member', 'UAD One'],
          'names: own render id or a matched UA.xml collection id admits the plug-in; nameless entries skipped')
    check(authorized_names(manifest, set()) == [], 'names: an empty answer admits nothing')
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        (t / 'bundles').mkdir()
        (t / 'store' / 'modules').mkdir(parents=True)
        a, b, c = 'a' * 64, 'b' * 64, 'c' * 64
        for s in (a, b, 'e' * 64, 'd' * 64):
            (t / 'store' / 'modules' / f'{s}.bill').write_bytes(b'Bill')
        res = [dict(sha256=a, architecture=2), dict(sha256=b, architecture=0), dict(sha256='f' * 64, architecture=1),
               dict(sha256='e' * 64, architecture=2), dict(sha256='d' * 64, architecture=2)]
        (t / 'bundles' / 'one-mono.json').write_text(json.dumps({'descriptor': {'resources': res}}), encoding='utf-8')
        (t / 'bundles' / 'two-mono.json').write_text(json.dumps({'descriptor': {'resources': [dict(sha256=c, architecture=2)]}}),
                                                     encoding='utf-8')
        index = {'bundles': [dict(name='UAD One', layout='mono', file='one-mono.json'),
                             dict(name='UAD Two', layout='mono', file='two-mono.json'),
                             dict(name='UAD Gone', layout='mono', file='gone-mono.json')],
                 'failures': {'UAD Bad': {'key': None, 'reasons': {'discovery': 'no profile'}},
                              'UAD Elsewhere': {'key': None, 'reasons': {'setup': 'no panel spec'}}},
                 'waiting_for_licence': ['UAD Later']}
        (t / 'bundles' / 'index.json').write_text(json.dumps(index), encoding='utf-8')
        have, missing, read = module_list(t / 'bundles', t / 'store', pins)
        check(have == sorted([a, b, 'd' * 64]) and missing == [c] and read == 2,
              'modules: architectures 0 and 2, the strip pin (0eb) left out, another pinned id (0db) kept, '
              'a module not in the store reported, a missing bundle skipped')
        names = t / 'names.txt'
        names.write_text('UAD One\nUAD Bad\n', encoding='utf-8')
        s = summary(t / 'bundles', names)
        check(s['plugins'] == 2 and s['bundles'] == 3 and [f['name'] for f in s['failed']] == ['UAD Bad'] and
              s['waiting'] == 1 and s['selection'] == 2 and s['selected_mapped'] == 1,
              'summary: mapped plug-ins, failures of the selection only, waiting for a licence')
        s = summary(t / 'nowhere')
        check(s['plugins'] == 0 and s['failed'] == [], 'summary: no maps yet')
        out = t / 'req.json'
        (t / 'pins.json').write_text(json.dumps(pins), encoding='utf-8')
        rc = cmd_modules(argparse.Namespace(bundles=t / 'bundles', store=t / 'store', pins=t / 'pins.json', out=out))
        req = read_json(out)
        check(rc == 1 and req['hosting'] == '' and req['from'].endswith('modules') and len(req['modules']) == 3,
              'modules request: hosting untouched, the store folder, the verified list (exit 1: one missing)')
    print('setup_plugins selftest: ' + ('FAILED' if failed else 'all passed'))
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='setup_plugins.py', description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    n = sub.add_parser('names')
    n.add_argument('--manifest', type=Path, required=True)
    n.add_argument('--ids', required=True)
    n.add_argument('--out', type=Path, required=True)
    m = sub.add_parser('modules')
    m.add_argument('--bundles', type=Path, required=True)
    m.add_argument('--store', type=Path, required=True)
    m.add_argument('--pins', type=Path, required=True)
    m.add_argument('--out', type=Path, required=True)
    s = sub.add_parser('summary')
    s.add_argument('--bundles', type=Path, required=True)
    s.add_argument('--names', type=Path)
    s.add_argument('--only', action='append')
    sub.add_parser('selftest')
    a = ap.parse_args(argv)
    return {'names': cmd_names, 'modules': cmd_modules, 'summary': cmd_summary, 'selftest': cmd_selftest}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
