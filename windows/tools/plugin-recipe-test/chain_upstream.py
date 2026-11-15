# SPDX-License-Identifier: GPL-2.0-only
"""[Ins] Upstream's own chain composer (open-apollo 2.1.0 mixer-engine/native_chain_composer.py compose(), vendored
unmodified in plugins\\tools\\open-apollo) on bundles, printed in plugin-recipe-test's ChainText form.

Used by make-golden-chain.py (synthetic bundles, golden_chain.h) and chain-compare.py (this PC's real bundles).
Offline: no device, no socket, no UA program; real bundles and what this prints are UA-derived and stay local.
"""
import importlib.util
import json
from pathlib import Path


def load_oa(plugins):
    """oa_offline from <plugins>\\tools (the vendored upstream code)."""
    spec = importlib.util.spec_from_file_location('oa_offline', Path(plugins) / 'tools' / 'oa_offline.py')
    oa = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oa)
    return oa


def compose_text(oa, store, bundles_dir, files, layout, mon, delays, input_index=0):
    """compose() over the providers of `files` (in slot order), its arguments and observations as text."""
    nc, prov, _ = oa.load(Path(store), Path(bundles_dir))
    import native_chain_composer as ncc     # importable once oa.load put mixer-engine on sys.path
    providers = []
    for f in files:
        kind = prov.StereoProfiledCarrierProvider if layout == 'stereo' else prov.ProfiledCarrierProvider
        providers.append(kind(Path(bundles_dir) / f, input_index, module_store=Path(store) / 'modules'))
    channels = (input_index, input_index + 1) if layout == 'stereo' else (input_index,)
    record, monitor, capacity = delays
    kw = dict(record_pre_effects=True) if mon else {}
    c = ncc.compose(providers, channels=channels, record_delay_samples=record, monitor_delay_samples=monitor,
                    active_capacity=capacity, **kw)
    a = c.arguments
    out = ['apollo-plug-chain 1', f'carrier 0x{nc.MIC_CARRIER:x}', 'synch %d %d' % tuple(a['input_pair']),
           f'delays {record} {monitor} {capacity}', 'render_ids' + ''.join(f' 0x{r:x}' for r in c.render_ids),
           'scratch %d %d %d' % tuple(a['scratch'])]
    out += ['resource ' + (r.hex() if isinstance(r, (bytes, bytearray)) else str(r)) for r in a['resources']]
    out += ['row %d %d 0x%08x %d' % tuple(r) for r in a['private_rows']]
    cmd = list(a['commands'])
    for i in range(0, len(cmd), 12):
        out.append('cmd' + ''.join(f' 0x{w:08x}' for w in cmd[i:i + 12]))
    for row, word, values in c.observations:
        out.append(f'observe {row} {word} {len(values)}' + ''.join(f' 0x{v:08x}' for v in values))
    return '\n'.join(out) + '\n'


def index_files(plugins, name, layout):
    index = json.loads((Path(plugins) / 'bundles' / 'index.json').read_text(encoding='utf-8'))
    for e in index['bundles']:
        if e['name'] == name and e['layout'] == layout:
            return e['file']
    return None
