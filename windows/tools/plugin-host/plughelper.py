# SPDX-License-Identifier: GPL-2.0-only
"""Apollo Control's parameter planner: a plug-in's own setter, run in open-apollo's offline emulator.

  <plugins>\\.venv\\Scripts\\python.exe -I -X utf8 plughelper.py --plugins <apollo-win\\plugins>

Apollo Control starts this only when a parameter of a running insert changes (never at start-up:
inserts are rebuilt from saved words in C++, app/src/plugin_host.h). It plans; it never opens a
device, a socket or a UA program. Every device operation stays in Apollo Control, which sends the
planned WRITEs through SERVICE, reads the planned words back and only then says "accept".

What plans a change is upstream's own provider (open-apollo 2.1.0
mixer-engine/native_profiled_carrier_provider.py, vendored unmodified in plugins\\tools\\open-apollo):
plan_control() runs the plug-in's setter in the emulator (or answers from <bundle>.tables when the
mapper's tables gate passed), plan_restore() the way back, accept() moves the accepted state.
The writes are WRITE blocks in chain-row space (the plug-in's rows start at row 10), the form
apollo.sys checks and relocates for SERVICE.

Protocol: one JSON object per line on stdin, one reply per line on stdout ({"id", "ok", ...};
"error" when ok is false). Words are unsigned 32-bit integers; a saved word map ("diff") is
{"0x<row << 24 | word>": value} in the plug-in's own rows (row 0 = chain row 10), as the bundle's
startup shadow is keyed.
  {"op":"open","key":K,"bundle":"<file in bundles>","values":[...]|null,"diff":{...}|null}
       -> values, texts, maxima
  {"op":"plan","key":K,"index":i,"value":<integer 0..maximum>}
       -> proposal {chunks, observe, values, texts}, restore {chunks, observe, values, texts}
  {"op":"accept","key":K,"which":"proposal"|"restore"} -> values, texts, diff
  {"op":"close","key":K}   {"op":"quit"}

The bundles and the emulator run the user's own UA code read from the local install: nothing this
prints is kept anywhere but Apollo Control's own settings on this PC.
"""
import argparse
import json
import sys
import threading
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent


def find_plugins(arg):
    if arg:
        return Path(arg).resolve()
    for cand in (HERE.parent.parent / 'plugins', HERE.parent.parent.parent.parent / 'plugins'):
        if (cand / 'tools' / 'oa_offline.py').is_file():
            return cand
    raise SystemExit('apollo-win\\plugins not found: pass --plugins DIR')


def uints(seq):
    return [int(v) & 0xffffffff for v in seq]


class Session:
    def __init__(self, oa, prov, plugins, bundle, values, diff):
        path = plugins / 'bundles' / Path(bundle).name
        if not path.is_file():
            raise ValueError(f'no bundle {path.name}')
        self.provider, data = oa.provider_for(prov, path, plugins / 'store', 0)
        p = self.provider
        self.startup = p._startup_shadow()
        p.reset()                                   # the load-time state (learned defaults: no emulator yet)
        if values is not None:
            values = tuple(int(v) for v in values)
            if len(values) != len(p.maxima) or any(not 0 <= v <= m for v, m in zip(values, p.maxima)):
                raise ValueError('saved values do not fit this bundle')
            shadow = dict(self.startup)
            for k, v in (diff or {}).items():
                shadow[int(k, 0)] = int(v) & 0xffffffff
            with p._lock:
                p.cap = None
                p.state = prov.ProfiledState.make(values, shadow)
                p._texts = None
        self.pending = None

    def snapshot(self):
        p = self.provider
        return dict(values=list(p.state.values), texts=list(p._texts_now()), maxima=list(p.maxima))

    def plan(self, index, value):
        p = self.provider
        if not 0 <= index < len(p.maxima) or not 0 <= value <= p.maxima[index]:
            raise ValueError('parameter or value out of range')
        field = f'parameters/{index}/NormalizedValue'
        prior = p.snapshot(field)
        prior_texts = list(p._texts_now())
        proposal = p.plan_control(field, value / p.maxima[index] if p.maxima[index] else 0.0)
        cap = p.cap
        texts = [cap.format_parameter(i)['console_trimmed'] for i in range(len(p.maxima))]
        restore = p.plan_restore(prior, proposal)
        self.pending = dict(proposal=proposal, restore=restore)

        def out(plan, values, texts):
            chunks = [list(c) for c in self._chunks(plan.writes)]
            observe = [[r, w, uints(v)] for r, w, v in plan.observations]
            return dict(chunks=chunks, observe=observe, values=list(values), texts=texts)
        return dict(proposal=out(proposal, proposal.state.values, texts),
                    restore=out(restore, prior.state.values, prior_texts))

    @staticmethod
    def _chunks(words):
        import plugin_native
        return plugin_native._write_chunks(words) if words else ()

    def accept(self, which):
        if not self.pending or which not in self.pending:
            raise ValueError('nothing planned')
        p = self.provider
        p.accept(self.pending[which])
        self.pending = None
        shadow = p.state.shadow_dict()
        diff = {f'0x{k:x}': v for k, v in sorted(shadow.items()) if self.startup.get(k) != v}
        out = self.snapshot()
        out['diff'] = diff
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--plugins')
    args = ap.parse_args(argv)
    if sys.flags.utf8_mode != 1:
        sys.exit('run with python -X utf8 (the bundles and specs are UTF-8)')
    plugins = find_plugins(args.plugins)
    sys.path.insert(0, str(plugins / 'tools'))
    import oa_offline as oa                         # noqa: E402  (vendored upstream carrier, offline)
    _nc, prov, _index = oa.load(plugins / 'store', plugins / 'bundles')
    sessions = {}
    lock = threading.Lock()
    out = sys.stdout
    print(json.dumps(dict(ok=True, ready=True)), file=out, flush=True)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        rid = None
        try:
            req = json.loads(line)
            rid = req.get('id')
            op = req.get('op')
            if op == 'quit':
                print(json.dumps(dict(id=rid, ok=True)), file=out, flush=True)
                break
            key = str(req.get('key', ''))
            with lock:
                if op == 'open':
                    s = Session(oa, prov, plugins, req['bundle'], req.get('values'), req.get('diff'))
                    sessions[key] = s
                    reply = s.snapshot()
                elif op == 'plan':
                    reply = sessions[key].plan(int(req['index']), int(req['value']))
                elif op == 'accept':
                    reply = sessions[key].accept(req.get('which', 'proposal'))
                elif op == 'close':
                    sessions.pop(key, None)
                    reply = {}
                else:
                    raise ValueError(f'unknown op {op!r}')
            reply.update(id=rid, ok=True)
        except KeyError as e:
            reply = dict(id=rid, ok=False, error=f'not open or missing field: {e}')
        except Exception as e:                      # noqa: BLE001  (one bad request must not end the helper)
            reply = dict(id=rid, ok=False, error=f'{type(e).__name__}: {e}')
            traceback.print_exc(file=sys.stderr)
        print(json.dumps(reply), file=out, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
