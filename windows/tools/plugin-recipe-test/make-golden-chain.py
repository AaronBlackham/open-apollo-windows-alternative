# SPDX-License-Identifier: GPL-2.0-only
"""[Ins] Writes golden_chain.h: upstream's own compose() (open-apollo 2.1.0 native_chain_composer.py, vendored in
plugins\\tools\\open-apollo) on the synthetic bundles plugin-recipe-test makes, so its selftest compares the C++
chain composer (app/src/plugin_host.h BuildChain) without Python or UA data.

  <plugins>\\.venv\\Scripts\\python.exe -I -X utf8 make-golden-chain.py --plugins <apollo-win\\plugins> --work <empty scratch dir>

Needs plugin-recipe-test.exe built (it writes the synthetic bundles and modules and chains.txt) and, read from the
local install's store, the 5 Console strip modules upstream's carrier hashes (store\\dsp\\*.bill, copied into the
scratch dir only; the golden text holds their pinned SHA-256s, already in include\\apollo_strip_pins.h).
golden_chain.h itself carries no UA data.
"""
import argparse
import importlib.util
import json
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
    (work / 'store' / 'manifest.json').write_text(json.dumps(dict(plugins=[])), encoding='utf-8')
    cases = [l.split('|') for l in (work / 'cases.txt').read_text(encoding='utf-8').splitlines() if l]
    (work / 'bundles' / 'index.json').write_text(json.dumps(dict(
        bundles=[dict(name=n, layout=lay, file=f) for n, lay, f, _ in cases], uad_sdk=None)), encoding='utf-8')
    spec = importlib.util.spec_from_file_location('chain_upstream', HERE / 'chain_upstream.py')
    cu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cu)
    oa = cu.load_oa(plugins)
    texts = []
    for line in (work / 'chains.txt').read_text(encoding='utf-8').splitlines():
        if not line:
            continue
        files, layout, mon, rec, monitor, cap = line.split('|')
        texts.append(cu.compose_text(oa, work / 'store', work / 'bundles', files.split(','), layout, mon == '1',
                                     (int(rec), int(monitor), int(cap))))
    lines = ['/* golden_chain.h - written by make-golden-chain.py: open-apollo\'s own native_chain_composer.compose() on the',
             '   synthetic bundles of plugin-recipe-test.cpp (kChainCases; no UA data; the strip module hashes are the pins).',
             '   One line per string (MSVC caps a string literal); plugin-recipe-test joins them. */',
             '#pragma once', '']
    for k, t in enumerate(texts):
        lines.append(f'static const char *const kChainGolden{k}[] = {{')
        for l in t.splitlines():
            lines.append('    "' + l.replace(chr(92), chr(92) * 2).replace('"', chr(92) + '"') + '",')
        lines.append('    nullptr,')
        lines.append('};')
    lines.append('static const char *const *const kChainGoldenLines[] = { ' +
                 ', '.join(f'kChainGolden{k}' for k in range(len(texts))) + ' };')
    (HERE / 'golden_chain.h').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'golden_chain.h: {len(texts)} chains')


if __name__ == '__main__':
    sys.exit(main())
