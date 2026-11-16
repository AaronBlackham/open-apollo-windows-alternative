# SPDX-License-Identifier: GPL-2.0-only
"""Shared loader for offline_check.py and bench.py: upstream's own native carrier code
(tools/open-apollo/mixer-engine, unmodified) pointed at this PC's store, strips and bundles.

Nothing here opens a device, a socket or a UA program. plugin_native imports plugin_driver,
which imports fcntl for its Linux ioctls; the offline checks never construct a driver, so an
inert fcntl stand-in satisfies the import on Windows (it raises if anything calls it).
"""
import json
import os
import re
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
VENDOR = HERE / 'open-apollo'
MIXER = VENDOR / 'mixer-engine'
EXECUTOR = VENDOR / 'tools/plugin-mapper/plugin_mapper/executor'
KNOWN_GOOD = MIXER / 'device_maps/native_known_good.json'
PINS = VENDOR / 'console-rs/crates/oa-assets/data/firmware-pins.json'
PLUGINS = HERE.parent                      # apollo-win/plugins
STORE = PLUGINS / 'store'
BUNDLES = PLUGINS / 'bundles'
SPECS = PLUGINS / 'specs'


def _inert_fcntl():
    if 'fcntl' in sys.modules:
        return
    mod = types.ModuleType('fcntl')

    def refuse(*_a, **_k):
        raise OSError('offline: no device ioctl on this PC')
    mod.ioctl = mod.flock = mod.fcntl = refuse
    sys.modules['fcntl'] = mod


def load(store=STORE, bundles=BUNDLES):
    """(native_carrier, provider module) with DSPDIR on store/dsp and the emulator's env set."""
    _inert_fcntl()
    index = json.loads((Path(bundles) / 'index.json').read_text(encoding='utf-8'))
    if index.get('uad_sdk'):
        os.environ.setdefault('OA_UAD_SDK', index['uad_sdk'])     # read (as bytes) by the emulator only
    os.environ.setdefault('OA_MODULE_STORE', str(Path(store) / 'modules'))
    for p in (str(MIXER), str(EXECUTOR)):
        if p not in sys.path:
            sys.path.insert(0, p)
    import native_carrier
    native_carrier.DSPDIR = str(Path(store) / 'dsp')   # as upstream's tests point it (test_native_bundles.py)
    import native_profiled_carrier_provider as provider
    return native_carrier, provider, index


def provider_for(provider, bundle_path, store=STORE, input_index=0):
    data = json.loads(Path(bundle_path).read_bytes())
    kind = provider.StereoProfiledCarrierProvider if data.get('layout') == 'stereo' else provider.ProfiledCarrierProvider
    return kind(bundle_path, input_index, module_store=Path(store) / 'modules'), data


def known_good(path=KNOWN_GOOD):
    return {e['name']: e for e in json.loads(Path(path).read_text(encoding='utf-8'))['plugins']}


def store_manifest(store=STORE):
    return json.loads((Path(store) / 'manifest.json').read_text(encoding='utf-8'))


def slug(name):
    return re.sub(r'[^a-z0-9]+', '-', name.lower().removeprefix('uad ')).strip('-')
