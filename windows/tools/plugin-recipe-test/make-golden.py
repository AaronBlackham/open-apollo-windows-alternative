# SPDX-License-Identifier: GPL-2.0-only
"""Writes golden.h: p1_recipe.py's recipes (open-apollo's own carrier code) for the synthetic bundles
plugin-recipe-test makes, so its selftest compares the C++ port without Python or UA data.

  <plugins>\\.venv\\Scripts\\python.exe -I -X utf8 make-golden.py --plugins <apollo-win\\plugins> --work <empty scratch dir>

Needs plugin-recipe-test.exe built (it writes the synthetic bundles and modules: no UA bytes) and,
read from the local install's store, the 5 Console strip modules upstream's carrier hashes
(store\\dsp\\*.bill, copied into the scratch dir only; the golden text holds their pinned SHA-256s,
already in include\\apollo_strip_pins.h). golden.h itself carries no UA data.
"""
import argparse
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plugins', required=True)
    ap.add_argument('--work', required=True)
    a = ap.parse_args()
    plugins, work = Path(a.plugins).resolve(), Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(HERE / 'plugin-recipe-test.exe'), 'synth', str(work)], check=True)
    (work / 'store' / 'dsp').mkdir(parents=True, exist_ok=True)
    for f in (plugins / 'store' / 'dsp').glob('*.bill'):
        shutil.copyfile(f, work / 'store' / 'dsp' / f.name)
    # a plugins folder whose tools\oa_offline.py is the real one (p1_recipe.py imports it from --plugins)
    (work / 'tools').mkdir(exist_ok=True)
    real = plugins / 'tools' / 'oa_offline.py'
    (work / 'tools' / 'oa_offline.py').write_text(
        'import importlib.util as _u\n'
        f'_s = _u.spec_from_file_location("oa_offline_real", {str(real)!r})\n'
        '_m = _u.module_from_spec(_s)\n_s.loader.exec_module(_m)\n'
        'globals().update({k: v for k, v in vars(_m).items() if not k.startswith("__")})\n', encoding='utf-8')
    cases = [l.split('|') for l in (work / 'cases.txt').read_text(encoding='utf-8').splitlines() if l]
    import json
    (work / 'store' / 'manifest.json').write_text(json.dumps(dict(plugins=[])), encoding='utf-8')
    (work / 'bundles' / 'index.json').write_text(json.dumps(dict(
        bundles=[dict(name=n, layout=lay, file=f) for n, lay, f, _ in cases], uad_sdk=None)), encoding='utf-8')
    spec = importlib.util.spec_from_file_location('p1_recipe', HERE.parent / 'plugin-host' / 'p1_recipe.py')
    p1 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(p1)
    texts = []
    for name, layout, f, delay in cases:
        out = work / (f + '.recipe')
        p1.main([name, '--layout', layout, '--input', '0', '--plugins', str(work), '--out', str(out), '--record-delay', delay])
        texts.append(out.read_text(encoding='utf-8'))
    lines = ["/* golden.h - written by make-golden.py: tools/plugin-host/p1_recipe.py (open-apollo's carrier code) on the",
             '   synthetic bundles of plugin-recipe-test.cpp (no UA data; the strip module hashes are the pins). One line',
             '   per string (MSVC caps a string literal); plugin-recipe-test joins them. */',
             '#pragma once', '']
    for k, t in enumerate(texts):
        lines.append(f'static const char *const kGolden{k}[] = {{')
        for l in t.splitlines():
            lines.append('    "' + l.replace(chr(92), chr(92) * 2).replace('"', chr(92) + '"') + '",')
        lines.append('    nullptr,')
        lines.append('};')
    lines.append('static const char *const *const kGoldenLines[] = { ' + ', '.join(f'kGolden{k}' for k in range(len(texts))) + ' };')
    (HERE / 'golden.h').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'golden.h: {len(texts)} recipes')


if __name__ == '__main__':
    sys.exit(main())
