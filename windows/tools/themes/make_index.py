"""Writes app/ui/themes/index.js from every app/ui/themes/<id>/manifest.json (docs/THEMES.md 2, theme packs).

index.js lists the packs (their manifests, inlined, so the page needs no fetch: it also runs from file://) and writes
each pack's theme.css and renderers.js into the page while it parses, so every pack is ready before app.js boots.
Adding a theme: a new folder with manifest.json, theme.css and renderers.js, then run this (python -I make_index.py).
"""
import json, os, sys

here = os.path.dirname(os.path.abspath(__file__))
themes = os.path.normpath(os.path.join(here, '..', '..', 'app', 'ui', 'themes'))
packs = []
for name in sorted(os.listdir(themes)):
    m = os.path.join(themes, name, 'manifest.json')
    if not os.path.isfile(m):
        continue
    with open(m, encoding='utf-8') as f:
        man = json.load(f)
    if man.get('id') != name:
        sys.exit(f'{m}: id must be the folder name ({name})')
    for part in ('theme.css', 'renderers.js'):
        if not os.path.isfile(os.path.join(themes, name, part)):
            sys.exit(f'{name}: {part} missing')
    packs.append(man)
packs.sort(key=lambda p: (p.get('order', 99), p['id']))
body = json.dumps(packs, indent=2, ensure_ascii=False)
js = (
    "'use strict';\n"
    "/* Theme packs (docs/THEMES.md 2). Made by tools/themes/make_index.py from themes/<id>/manifest.json: do not edit by hand. */\n"
    f"window.ApolloThemes = {{ manifests: {body} }};\n"
    "for (const m of window.ApolloThemes.manifests) {\n"
    "  document.write(`<link rel=\"stylesheet\" href=\"themes/${m.id}/theme.css\"><script src=\"themes/${m.id}/renderers.js\"><\\/script>`);\n"
    "}\n"
)
with open(os.path.join(themes, 'index.js'), 'w', encoding='utf-8', newline='\n') as f:
    f.write(js)
print(f'{len(packs)} packs: ' + ', '.join(p['id'] for p in packs))
