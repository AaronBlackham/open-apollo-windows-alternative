# SPDX-License-Identifier: GPL-2.0-only
"""[Ins] The C++ chain composer (app/src/plugin_host.h BuildChain) against upstream's own compose() on this PC's real
bundles.

  <plugins>\\.venv\\Scripts\\python.exe -X utf8 chain-compare.py --plugins <apollo-win\\plugins> --work <scratch dir>
         [--chain "Name A+Name B[+...]"]... [--layout mono|stereo] [--mon] [--delays 200,0,256]

Each chain (default: a few mixed ones from the mapped, known-passing bundles): upstream's compose() over its providers
writes the expected text into --work, `plugin-recipe-test chain ... --expect` builds the same chain in C++ and compares
line by line. Offline; the texts are UA-derived and stay in --work (a scratch folder), never in the repo.
"""
import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_CHAINS = [
    ('UAD Precision De-Esser', 'UAD UA 1176LN Legacy'),
    ('UAD Ampeg SVTVR Classic', 'UAD Precision De-Esser', 'UAD Teletronix LA-2A Legacy'),
    ('UAD UA 1176LN Legacy', 'UAD Teletronix LA-2A Legacy', 'UAD Precision De-Esser', 'UAD Cambridge EQ'),
    ('UAD Lexicon 480L', 'UAD Precision De-Esser'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plugins', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--chain', action='append')
    ap.add_argument('--layout', action='append')
    ap.add_argument('--mon', action='store_true')
    ap.add_argument('--delays', default='200,0,256')
    a = ap.parse_args()
    plugins, work = Path(a.plugins).resolve(), Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('chain_upstream', HERE / 'chain_upstream.py')
    cu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cu)
    oa = cu.load_oa(plugins)
    chains = [tuple(c.split('+')) for c in a.chain] if a.chain else DEFAULT_CHAINS
    layouts = a.layout or ['mono', 'stereo']
    delays = tuple(int(x) for x in a.delays.split(','))
    exe = str(HERE / 'plugin-recipe-test.exe')
    counts = dict(same=0, differ=0, skipped=0)
    for names in chains:
        for layout in layouts:
            for mon in ([False, True] if not a.mon else [True]):
                files = [cu.index_files(plugins, n, layout) for n in names]
                tag = ' + '.join(n.removeprefix('UAD ') for n in names) + f' ({layout}{", MON" if mon else ""})'
                if None in files:
                    counts['skipped'] += 1
                    print(f'SKIP  {tag}: not mapped in this layout', flush=True)
                    continue
                try:
                    text = cu.compose_text(oa, plugins / 'store', plugins / 'bundles', files, layout, mon, delays)
                except Exception as x:              # noqa: BLE001
                    counts['skipped'] += 1
                    print(f'SKIP  {tag}: upstream refuses it: {type(x).__name__}: {x}', flush=True)
                    continue
                out = work / ('chain-' + '-'.join(Path(f).stem[:12] for f in files) + f'-{layout}{"-mon" if mon else ""}.txt')
                out.write_text(text, encoding='utf-8')
                args = [exe, 'chain', '--store', str(plugins / 'store' / 'modules'), '--layout', layout,
                        '--delays', *map(str, delays), '--expect', str(out)] + (['--mon'] if mon else [])
                args += [str(plugins / 'bundles' / f) for f in files]
                r = subprocess.run(args, capture_output=True, text=True)
                ok = r.returncode == 0
                counts['same' if ok else 'differ'] += 1
                print(('PASS  ' if ok else 'FAIL  ') + tag + ': ' + (r.stdout.strip() or r.stderr.strip()).replace('\n', '\n      '), flush=True)
    print(f"\n{counts['same']} identical, {counts['differ']} different, {counts['skipped']} skipped")
    return 1 if counts['differ'] else 0


if __name__ == '__main__':
    sys.exit(main())
