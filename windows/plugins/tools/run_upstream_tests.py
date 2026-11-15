# SPDX-License-Identifier: GPL-2.0-only
"""Run upstream's synthetic mapper/carrier tests on Windows without pytest (none may be downloaded here).

  python -X utf8 run_upstream_tests.py

Provides the two pytest fixtures these tests use (tmp_path, monkeypatch) and runs the test
functions listed below from the unmodified copies in tools/open-apollo/mixer-engine/tests, against
the vendored (Windows-patched) mapper and the unmodified carrier code. Synthetic data only.
Skipped on purpose: test_tables_are_learned_once_per_bundle_and_refusals_recorded relies on
fork carrying a monkeypatched function into the pool workers; Windows only has spawn.
The daemon-level tests in test_native_bundles.py need ua_mixer_daemon, which is not vendored.
"""
import importlib.util
import inspect
import os
import shutil
import sys
import tempfile
import traceback
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE / 'open-apollo/mixer-engine/tests'
RUN = {
    'test_plugin_mapper_build.py': ['test_a_map_is_current_until_one_of_its_inputs_changes',
                                    'test_a_failure_is_not_retried_with_the_same_inputs', 'test_v1_index_is_mapped_again',
                                    'test_merge_replaces_one_plugin_only', 'test_progress_lines_count_eta_and_markers',
                                    'test_eta_unknown_before_the_first_map_and_human_text',
                                    'test_workers_default_to_cores_minus_one', 'test_authorized_file_and_all',
                                    'test_stage_copies_what_mapping_reads'],
    'test_native_bundles.py': ['test_carrier_recipe', 'test_write_masks_gaps',
                               'test_registry_offers_each_verified_layout_from_its_own_bundle',
                               'test_offering_reads_no_bundle_and_the_first_use_builds_once',
                               'test_known_good_list_holds_no_dsp_data'],
}


class MonkeyPatch:
    _missing = object()

    def __init__(self):
        self.undo = []

    def setattr(self, target, name, value, raising=True):
        old = getattr(target, name, self._missing)
        if old is self._missing and raising:
            raise AttributeError(name)
        self.undo.append(lambda: delattr(target, name) if old is self._missing else setattr(target, name, old))
        setattr(target, name, value)

    def setenv(self, key, value):
        old = os.environ.get(key)
        self.undo.append(lambda: os.environ.pop(key, None) if old is None else os.environ.__setitem__(key, old))
        os.environ[key] = str(value)

    def delenv(self, key, raising=True):
        if key not in os.environ:
            if raising:
                raise KeyError(key)
            return
        old = os.environ.pop(key)
        self.undo.append(lambda: os.environ.__setitem__(key, old))

    def close(self):
        for f in reversed(self.undo):
            f()


def main():
    sys.path[:0] = [str(HERE / 'open-apollo/mixer-engine')]
    import oa_offline
    oa_offline._inert_fcntl()                       # plugin_driver's Linux ioctl import; never called
    os.environ.setdefault('PYTHONUTF8', '1')
    passed = failed = 0
    for file, names in RUN.items():
        spec = importlib.util.spec_from_file_location(file[:-3], TESTS / file)
        mod = importlib.util.module_from_spec(spec)
        sys.modules[file[:-3]] = mod
        spec.loader.exec_module(mod)
        for name in names:
            fn = getattr(mod, name)
            tmp = Path(tempfile.mkdtemp(prefix='oa-test-'))
            mp = MonkeyPatch()
            kwargs = {'tmp_path': tmp, 'monkeypatch': mp}
            try:
                fn(**{k: kwargs[k] for k in inspect.signature(fn).parameters})
                passed += 1
                print(f'PASS  {file}::{name}')
            except Exception:
                failed += 1
                print(f'FAIL  {file}::{name}\n' + traceback.format_exc())
            finally:
                mp.close()
                shutil.rmtree(tmp, ignore_errors=True)
    print(f'{passed} passed, {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
