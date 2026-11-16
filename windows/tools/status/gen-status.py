#!/usr/bin/env python3
"""Status badges, progress maps and the status page for the Windows port.

Reads docs/status/parity.json (open-apollo's 1,015 UA-driver behaviours with open-apollo's own status from its
Oct 11 2026 export and ours from the audit), models.json, plugins.json and windows-only.json, and writes into
the same folder:
  badge-parity.svg, badge-hardware.svg, badge-models.svg, badge-plugins.svg
  progress-light.svg, progress-dark.svg   (two maps side by side: open-apollo and the Windows port)
  README.md                               (per-area table, where the port is ahead, what is open)

Usage: python -I tools/status/gen-status.py [docs/status]
No third-party packages. Re-run after every audit; commit the outputs.
"""
import html
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'docs', 'status')

AREAS = ['Kext classes', 'Mixer params', 'HW interface', 'Mixer Engine', 'Plugin Support', 'User client',
         'Windows+USB', 'Lifecycle', 'DSP+firmware', 'Model types', 'Device mgr', 'Transport+clock', 'Services',
         'Windows PCIe']
UP_ORDER = {'verified': 0, 'done': 1, 'plugin': 2, 're': 3, 'blocked': 4}
WIN_ORDER = {'verified': 0, 'done': 1, 'partial': 2, 'notdone': 3, 'na': 4}
UP_NAME = {'verified': 'verified on hardware', 'done': 'done in code', 'plugin': 'plug-in hosting, in progress',
           're': 'still to figure out', 'blocked': 'needs hardware'}
WIN_NAME = {'verified': 'verified on hardware', 'done': 'done in code', 'partial': 'partial or being built',
            'notdone': 'not done', 'na': 'not needed on Windows'}

# Palettes checked with the dataviz validator (CVD separation, lightness band, contrast) on each surface.
THEMES = {
    'light': {'bg': '#ffffff', 'fg': '#1f2328', 'muted': '#656d76', 'ver': '#1f883d', 'part': '#d4a017',
              'plug': '#2f6fe0', 'hw': '#d1242f', 're': '#b33dc6'},
    'dark': {'bg': '#0d1117', 'fg': '#e6edf3', 'muted': '#7d8590', 'ver': '#2ea043', 'part': '#b38410',
             'plug': '#3b73e6', 'hw': '#da3633', 're': '#b33dc6'},
}


def mix(a, b, t):
    """t of colour a over colour b."""
    pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return '#' + ''.join('%02x' % round(x * t + y * (1 - t)) for x, y in zip(pa, pb))


def load(name):
    with open(os.path.join(OUT, name), encoding='utf-8') as f:
        return json.load(f)


def done(rows, key):
    """Behaviours verified or done on one side ('upstream' or 'ours')."""
    return sum(1 for r in rows if r[key] in ('verified', 'done'))


# ---- squarified treemap (Bruls, Huizing, van Wijk) ----
def squarify(values, x, y, w, h):
    rects, items = [], sorted(enumerate(values), key=lambda kv: -kv[1])
    total = float(sum(values))
    scale = (w * h) / total if total else 0
    pending = [(i, v * scale) for i, v in items]

    def worst(row, side):
        s = sum(a for _, a in row)
        return max(max(side * side * a / (s * s), (s * s) / (side * side * a)) for _, a in row)

    while pending:
        side = min(w, h)
        row = [pending.pop(0)]
        while pending and worst(row + [pending[0]], side) <= worst(row, side):
            row.append(pending.pop(0))
        s = sum(a for _, a in row)
        if w >= h:
            cw = s / h
            yy = y
            for i, a in row:
                rects.append((i, x, yy, cw, a / cw))
                yy += a / cw
            x, w = x + cw, w - cw
        else:
            ch = s / w
            xx = x
            for i, a in row:
                rects.append((i, xx, y, a / ch, ch))
                xx += a / ch
            y, h = y + ch, h - ch
    return sorted(rects)


def esc(s):
    return html.escape(str(s), quote=True)


def map_svg(rows, key, order, fill, x0, y0, W, H, t, hatch_id):
    """One parity map: areas as a treemap, one square per behaviour, sorted by status inside each area."""
    by = {}
    for r in rows:
        by.setdefault(r['section'], []).append(r)
    areas = [a for a in AREAS if a in by]
    rects = squarify([len(by[a]) for a in areas], 0, 0, W, H)
    out = []
    for i, rx, ry, rw, rh in rects:
        a = areas[i]
        pad = 2.0
        bx, by_, bw, bh = x0 + rx + pad, y0 + ry + pad, rw - 2 * pad, rh - 2 * pad
        items = sorted(by[a], key=lambda r: (order.get(r[key], 9), r['n']))
        count = '%d/%d' % (done(items, key), len(items))
        name_w, count_w = len(a) * 5.6, len(count) * 5.6 + 8
        if name_w + count_w < bw - 2:
            label, show_count = a, True
        elif name_w < bw - 2:
            label, show_count = a, False
        else:
            label, show_count = a[:max(3, int((bw - 8) / 5.6))] + '…', False
        out.append('<text x="%.1f" y="%.1f" class="l">%s</text>' % (bx + 1, by_ + 10, esc(label)))
        if show_count:
            out.append('<text x="%.1f" y="%.1f" class="c" text-anchor="end">%s</text>' % (bx + bw - 1, by_ + 10, count))
        cy0, ch = by_ + 14, bh - 14
        n = len(items)
        s = math.floor(math.sqrt(bw * ch / n))
        while s > 2 and math.floor(bw / s) * math.floor(ch / s) < n:
            s -= 1
        cols = max(1, math.floor(bw / s))
        gap = 1.5 if s >= 8 else 1.0
        for k, r in enumerate(items):
            cx, cyy = bx + (k % cols) * s, cy0 + (k // cols) * s
            st = r[key]
            title = '%s %s: %s | ours: %s' % (r['section'], r['id'], r['behaviour'], WIN_NAME.get(r['ours'], r['ours'])) \
                if key == 'ours' else '%s %s: %s | open-apollo: %s' % (r['section'], r['id'], r['behaviour'], UP_NAME.get(st, st))
            if st == 'na':
                out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="1" fill="none" stroke="%s" '
                           'stroke-width="0.75"><title>%s</title></rect>' % (cx + .4, cyy + .4, s - gap - .8, s - gap - .8, t['muted'], esc(title)))
            else:
                f = 'url(#%s)' % hatch_id if fill[st] == 'hatch' else fill[st]
                out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="1" fill="%s"><title>%s</title></rect>'
                           % (cx, cyy, s - gap, s - gap, f, esc(title)))
    return out


def progress_svg(rows, t, name):
    W, H, gutter, top, legend, m = 560, 400, 36, 34, 64, 8
    total_w = W * 2 + gutter + 2 * m
    total_h = top + H + legend + m
    up_fill = {'verified': t['ver'], 'done': mix(t['ver'], t['bg'], .42), 'plugin': t['plug'], 're': t['re'],
               'blocked': t['hw']}
    win_fill = {'verified': t['ver'], 'done': mix(t['ver'], t['bg'], .42), 'partial': 'hatch',
                'notdone': mix(t['muted'], t['bg'], .32), 'na': 'none'}
    n = len(rows)
    up_done, win_done = done(rows, 'upstream'), done(rows, 'ours')
    hatch = 'hatch-' + name
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="img" '
         'aria-label="Parity maps: open-apollo %d of %d behaviours, the Windows port %d of %d">'
         % (total_w, total_h, total_w, total_h, up_done, n, win_done, n),
         '<style>text{font-family:-apple-system,"Segoe UI",Helvetica,Arial,sans-serif}'
         '.h{font-size:13px;font-weight:600;fill:%s}.s{font-size:12px;fill:%s}.l{font-size:10px;fill:%s}'
         '.c{font-size:10px;fill:%s}.g{font-size:11px;fill:%s}</style>' % (t['fg'], t['muted'], t['fg'], t['muted'], t['muted']),
         '<defs><pattern id="%s" width="4" height="4" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
         '<rect width="4" height="4" fill="%s"/><rect width="1.3" height="4" fill="%s" fill-opacity="0.55"/></pattern></defs>'
         % (hatch, t['part'], t['bg']),
         '<rect width="100%%" height="100%%" fill="%s"/>' % t['bg'],
         '<g transform="translate(%d,%d)">' % (m, m)]
    o.append('<text x="0" y="12" class="h">open-apollo (Linux), Oct 11 2026</text>')
    o.append('<text x="%d" y="12" class="s" text-anchor="end">%d of %d &#183; %.1f%%</text>' % (W, up_done, n, 100.0 * up_done / n))
    o.append('<text x="%d" y="12" class="h">Windows port, Nov 16 2026</text>' % (W + gutter))
    o.append('<text x="%d" y="12" class="s" text-anchor="end">%d of %d &#183; %.1f%%</text>' % (2 * W + gutter, win_done, n, 100.0 * win_done / n))
    o += map_svg(rows, 'upstream', UP_ORDER, up_fill, 0, top - 12, W, H, t, hatch)
    o += map_svg(rows, 'ours', WIN_ORDER, win_fill, W + gutter, top - 12, W, H, t, hatch)

    def legend_row(x, y, entries):
        out = []
        for label, f in entries:
            if f == 'none':
                out.append('<rect x="%d" y="%d" width="10" height="10" rx="2" fill="none" stroke="%s"/>' % (x, y, t['muted']))
            else:
                out.append('<rect x="%d" y="%d" width="10" height="10" rx="2" fill="%s"/>' % (x, y, f))
            out.append('<text x="%d" y="%d" class="g">%s</text>' % (x + 15, y + 9, esc(label)))
            x += 22 + int(len(label) * 6.1)
        return out
    ly = top + H + 8
    o += legend_row(0, ly, [('verified on hardware', up_fill['verified']), ('done in code', up_fill['done']),
                            ('plug-in hosting, in progress', up_fill['plugin'])])
    o += legend_row(0, ly + 18, [('still to figure out', up_fill['re']), ('needs hardware', up_fill['blocked'])])
    o += legend_row(W + gutter, ly, [('verified on hardware', win_fill['verified']), ('done in code', win_fill['done']),
                                     ('partial or being built', 'url(#%s)' % hatch)])
    o += legend_row(W + gutter, ly + 18, [('not done', win_fill['notdone']), ('not needed on Windows', 'none')])
    o.append('</g></svg>')
    return '\n'.join(o)


def badge(label, value, color):
    lw, vw = int(len(label) * 6.6) + 12, int(len(value) * 6.6) + 12
    w = lw + vw
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="20" role="img" aria-label="%s: %s">'
            '<title>%s: %s</title><rect width="%d" height="20" fill="#555"/><rect x="%d" width="%d" height="20" fill="%s"/>'
            '<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
            '<text x="%.1f" y="14">%s</text><text x="%.1f" y="14">%s</text></g></svg>'
            % (w, esc(label), esc(value), esc(label), esc(value), lw, lw, vw, color, lw / 2.0, esc(label), lw + vw / 2.0, esc(value)))


def main():
    rows = load('parity.json')
    models = load('models.json')
    plugins = {p['metric']: p for p in load('plugins.json')}
    only = load('windows-only.json')
    n = len(rows)
    win_ver = sum(1 for r in rows if r['ours'] == 'verified')
    win_done, up_done = done(rows, 'ours'), done(rows, 'upstream')
    pct = 100.0 * win_done / n
    ours_models = [m['model'] for m in models if m['ours'] == 'verified']

    def write(name, text):
        with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)

    write('badge-parity.svg', badge('UA driver parity', '%.1f%%' % pct, '#7aa83b' if pct >= 70 else '#a4a61d'))
    write('badge-hardware.svg', badge('verified on hardware', '%d of %s' % (win_ver, format(n, ',')), '#2da44e'))
    write('badge-models.svg', badge('models verified', '%d: %s' % (len(ours_models), ', '.join(m.replace('Apollo ', '') for m in ours_models)), '#0969da'))
    write('badge-plugins.svg', badge('plug-ins live on the Apollo', str(plugins['live_gate']['ours']), '#bf8700'))
    for name, t in THEMES.items():
        write('progress-%s.svg' % name, progress_svg(rows, t, name))

    # ---- the status page ----
    secs = []
    for a in AREAS:
        rs = [r for r in rows if r['section'] == a]
        if not rs:
            continue
        c = lambda k, v: sum(1 for r in rs if r[k] == v)
        secs.append((a, len(rs), c('upstream', 'verified') + c('upstream', 'done'), c('ours', 'verified') + c('ours', 'done'),
                     c('ours', 'verified'), c('ours', 'partial'), c('ours', 'notdone'), c('ours', 'na')))
    ahead = [r for r in rows if r['upstream'] in ('blocked', 're', 'plugin') and r['ours'] in ('verified', 'done')]
    behind = [r for r in rows if r['upstream'] in ('verified', 'done') and r['ours'] in ('partial', 'notdone')]

    def by_area(sel):
        """One line per area: its count and its first four behaviours."""
        out = []
        for a in AREAS:
            rs = [r for r in sel if r['section'] == a]
            if rs:
                out.append('- **%s** (%d): %s' % (a, len(rs), '; '.join(r['behaviour'].rstrip('…') for r in rs[:4])))
        return out

    md = ['# Windows port status', '',
          'Generated by `tools/status/gen-status.py` from `parity.json`. The behaviour list and open-apollo\'s statuses',
          'are open-apollo\'s parity map (exported Oct 11 2026): every behaviour of UA\'s macOS kext, Mixer Engine and',
          'Windows drivers that open-apollo reverse-engineered. Our status per behaviour comes from an audit of this',
          'port on Nov 15 2026, updated Nov 16; `evidence` in `parity.json` says where each one lives or what is missing.', '',
          '<picture>', '  <source media="(prefers-color-scheme: dark)" srcset="progress-dark.svg">',
          '  <img alt="Parity maps: one square per UA driver behaviour, open-apollo on the left, the Windows port on the right" src="progress-light.svg">',
          '</picture>', '',
          '| | open-apollo (Oct 11) | Windows port (Nov 16) |', '|---|---|---|',
          '| Done in code or verified | %d of %d (%.1f%%) | %d of %d (%.1f%%) |' % (up_done, n, 100.0 * up_done / n, win_done, n, pct),
          '| Tagged verified on hardware | %d | %d |' % (sum(1 for r in rows if r['upstream'] == 'verified'), win_ver),
          '| Models verified | %s | %s |' % (', '.join(m['model'] for m in models if m['open_apollo'] == 'verified'), ', '.join(ours_models)),
          '| UAD plug-ins live on the Apollo | %d | %d |' % (plugins['live_gate']['open_apollo'], plugins['live_gate']['ours']), '',
          'open-apollo tags only a few behaviours as verified; much of its "done" work has also run on its own units,',
          'so the hardware row partly compares how each project records proof.', '',
          '## By area', '',
          '| Area | Behaviours | open-apollo done | Ours done | Ours on hardware | Ours partial | Ours not done | Not needed |',
          '|---|---:|---:|---:|---:|---:|---:|---:|']
    md += ['| %s | %d | %d | %d | %d | %d | %d | %d |' % s for s in sorted(secs, key=lambda s: -s[1])]
    md += ['', '## Ahead of open-apollo (%d)' % len(ahead), '',
           'Behaviours open-apollo marks as needing hardware, still to figure out, or plug-in hosting in progress, that this',
           'port has done or verified.', '']
    md += by_area(ahead)
    md += ['', '## Open on our side (%d)' % len(behind), '',
           'Behaviours open-apollo has done that this port has not, or only partly.', '']
    md += by_area(behind)
    md += ['', '## Windows work outside the list', '']
    md += ['- **%s** (%s): %s' % (w['feature'], w['status'], w['detail']) for w in only]
    md += ['', '## How it was scored', '',
           '- **verified**: ran on an Apollo x8p or Twin X Gen 1 with a passing result.',
           '- **done**: the code exists and passes the offline test suites.',
           '- **partial**: gated, stubbed, or being built.',
           '- **not needed on Windows**: a macOS mechanism with no function of its own in this design.',
           'Parity on both sides counts verified plus done over all %d behaviours.' % n, '']
    write('README.md', '\n'.join(md))
    print('parity %.1f%% (%d/%d), verified %d, ahead %d, open %d' % (pct, win_done, n, win_ver, len(ahead), len(behind)))


if __name__ == '__main__':
    main()
