# SPDX-License-Identifier: GPL-2.0-only
"""The C++ recipe builder (app/src/plugin_host.h) against p1_recipe.py on this PC's real bundles.

  <plugins>\\.venv\\Scripts\\python.exe -I -X utf8 compare.py --plugins <apollo-win\\plugins> --work <scratch dir>
         [--only NAME ...] [--delays 200,carrier]

Every mapped bundle (mono and stereo layouts, input 1, each record delay asked for): p1_recipe.py
writes its recipe into --work, plugin-recipe-test.exe compare builds the same recipe in C++ and
compares the text line by line. A bundle p1_recipe.py refuses must be refused by the C++ side too.
Offline: reads the bundles, the module store and the specs; the recipes it writes are UA-derived
and stay in --work (a scratch folder), never in the repo.
"""
import argparse
import importlib.util
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plugins', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--only', action='append')
    ap.add_argument('--delays', default='200,carrier')
    a = ap.parse_args()
    plugins, work = Path(a.plugins).resolve(), Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('p1_recipe', HERE.parent / 'plugin-host' / 'p1_recipe.py')
    p1 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(p1)
    exe = str(HERE / 'plugin-recipe-test.exe')
    index = json.loads((plugins / 'bundles' / 'index.json').read_text(encoding='utf-8'))
    counts = dict(same=0, differ=0, both_refused=0, one_refused=0)
    t0 = time.time()
    for e in index['bundles']:
        if a.only and e['name'] not in a.only:
            continue
        for delay in a.delays.split(','):
            out = work / f"{Path(e['file']).stem}-{delay}.recipe"
            py_ok, why = True, ''
            try:
                p1.main([e['name'], '--layout', e['layout'], '--input', '0', '--plugins', str(plugins), '--out', str(out),
                         '--record-delay', delay])
            except SystemExit as x:
                py_ok, why = (x.code in (0, None)), str(x.code)
            except Exception as x:                  # noqa: BLE001
                py_ok, why = False, f'{type(x).__name__}: {x}'
            args = [exe, 'compare', str(out), str(plugins / 'bundles' / e['file']), '--store', str(plugins / 'store' / 'modules'),
                    '--layout', e['layout'], '--input', '0', '--record-delay', delay, '--specs', str(plugins / 'specs'),
                    '--owned', str(plugins / 'p1' / 'dsp-owned.txt')]
            if not py_ok:
                out.write_text('', encoding='utf-8')
            r = subprocess.run(args, capture_output=True, text=True)
            line = (r.stdout.strip() or r.stderr.strip()).splitlines()
            if py_ok and r.returncode == 0:
                counts['same'] += 1
                print(line[0] if line else 'PASS', flush=True)
            elif not py_ok and r.returncode != 0 and 'FAIL  ' + e['file'] + ':' in r.stdout:
                counts['both_refused'] += 1
                print(f"BOTH REFUSE  {e['file']} ({delay}): python: {why[:120]} | c++: {r.stdout.strip()[:160]}", flush=True)
            elif py_ok:
                counts['differ'] += 1
                print('\n'.join(line), flush=True)
            else:
                counts['one_refused'] += 1
                print(f"ONLY PYTHON REFUSES  {e['file']} ({delay}): {why[:160]}", flush=True)
    print(f"\n{counts['same']} identical, {counts['differ']} different, {counts['both_refused']} refused by both, "
          f"{counts['one_refused']} refused by Python only ({time.time() - t0:.0f} s)")
    return 1 if counts['differ'] or counts['one_refused'] else 0


if __name__ == '__main__':
    sys.exit(main())
