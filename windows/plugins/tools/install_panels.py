# SPDX-License-Identifier: GPL-2.0-only
"""Windows port of open-apollo's scripts/install-panels.sh: install the pinned plug-in panel specs.

Port by Avium, 2026-11-14 (same checks as the shell script, in Python so it runs on Windows):
the pin (version, SHA-256, URL) comes from configs/plugin-panels.conf; the tarball is downloaded
into a NEW folder of its own (DOWNLOADS/panels-<version>/), or taken from --from; it must hash to
the pin before anything else reads it; every member must be a plain file or folder under one
open-apollo-panels-<version>/ top with no '..'; VERSION must equal the pin; only
specs/PluginSpec_[a-z0-9_]+.json files are installed, with VERSION, LICENSE, NOTICE.md, README.md
and a .source marker (the SHA-256), swapped into --dest in one rename.

  python -I install_panels.py --conf CONF --downloads DIR --dest DIR [--from TARBALL]

Exit status: 0 installed (or already current), 1 refused, 2 bad option, 3 could not download.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

MAX_BYTES = 64 << 20
SPEC = re.compile(r'PluginSpec_[a-z0-9_]+\.json')
TOP = re.compile(r'(open-apollo-panels-[0-9][0-9A-Za-z.+-]*)(/|$)')


def read_conf(path):
    pin = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        key, sep, value = line.partition('=')
        if sep and key in ('PANELS_VERSION', 'PANELS_SHA256', 'PANELS_URL'):
            pin[key] = value.strip()
    if not re.fullmatch(r'[0-9][0-9A-Za-z.+-]*', pin.get('PANELS_VERSION', '')) or \
            not re.fullmatch(r'[0-9a-f]{64}', pin.get('PANELS_SHA256', '')):
        raise SystemExit(f'[FAIL]  {path} needs PANELS_VERSION and a 64-digit PANELS_SHA256')
    return pin


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, out):
    if not url.startswith('https://'):
        raise SystemExit(f'[FAIL]  PANELS_URL must be https: {url}')
    print(f'[INFO]  Downloading {url}', flush=True)
    req = urllib.request.Request(url, headers={'User-Agent': 'panel-specs-installer'})
    with urllib.request.urlopen(req, timeout=60) as r, open(out, 'wb') as f:
        total = 0
        while True:
            block = r.read(1 << 16)
            if not block:
                break
            total += len(block)
            if total > MAX_BYTES:
                raise SystemExit(f'[FAIL]  The tarball is larger than {MAX_BYTES >> 20} MB')
            f.write(block)


def check_listing(tar):
    """The one top folder, or SystemExit: plain files and folders only, no '..' or '.'."""
    top = None
    for m in tar.getmembers():
        if not (m.isfile() or m.isdir()):
            raise SystemExit(f'[FAIL]  The tarball holds a link or a special file: {m.name}')
        match = TOP.match(m.name)
        if not match:
            raise SystemExit(f'[FAIL]  Unexpected entry in the tarball: {m.name}')
        top = top or match.group(1)
        if match.group(1) != top:
            raise SystemExit('[FAIL]  The tarball has more than one top folder')
        if '/../' in f'/{m.name}/' or '/./' in f'/{m.name}/' or '\\' in m.name or ':' in m.name:
            raise SystemExit(f'[FAIL]  Unexpected path in the tarball: {m.name}')
    if top is None:
        raise SystemExit('[FAIL]  The tarball is empty')
    return top


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--conf', type=Path, required=True, help='configs/plugin-panels.conf (the pin)')
    ap.add_argument('--downloads', type=Path, required=True, help='parent of the new download folder')
    ap.add_argument('--dest', type=Path, required=True, help='where the specs are installed')
    ap.add_argument('--from', dest='tarball', type=Path, help='use this tarball instead of downloading')
    args = ap.parse_args(argv)
    pin = read_conf(args.conf)
    version, expect = pin['PANELS_VERSION'], pin['PANELS_SHA256']
    dest = args.dest.resolve()
    source = dest / '.source'
    if source.is_file() and source.read_text(encoding='utf-8').strip() == expect:
        print(f"[ OK ]  Plug-in panel specs {(dest / 'VERSION').read_text(encoding='utf-8').strip()} are already in {dest}")
        return 0
    if dest.exists() and not (source.is_file() or all(
            SPEC.fullmatch(p.name) or p.name in ('VERSION', 'LICENSE', 'NOTICE.md', 'README.md') for p in dest.iterdir())):
        raise SystemExit(f"[FAIL]  {dest} holds files these specs don't; not replacing it")

    if args.tarball:
        tarball = args.tarball.resolve()
    else:
        folder = args.downloads.resolve() / f'panels-{version}'
        folder.mkdir(parents=True, exist_ok=False)   # a new, empty folder for this download
        tarball = folder / f'open-apollo-panels-{version}.tar.gz'
        try:
            download(pin['PANELS_URL'].replace('{version}', version), tarball)
        except OSError as error:
            print(f"[FAIL]  Couldn't download the plug-in panel specs: {error}")
            return 3
    if tarball.stat().st_size > MAX_BYTES:
        raise SystemExit(f'[FAIL]  The tarball is larger than {MAX_BYTES >> 20} MB')
    got = sha256(tarball)
    if got != expect:
        raise SystemExit(f'[FAIL]  SHA-256 mismatch: expected {expect}, got {got}')
    print(f'[ OK ]  SHA-256 {got} matches the pin ({tarball.stat().st_size} bytes)')

    with tarfile.open(tarball, 'r:gz') as tar:
        top = check_listing(tar)
        with tempfile.TemporaryDirectory(prefix='panels-') as tmp:
            tar.extractall(tmp, filter='data')
            src = Path(tmp) / top
            got_version = (src / 'VERSION').read_bytes()[:64].decode('utf-8', 'replace').strip() \
                if (src / 'VERSION').is_file() else ''
            if got_version != version:
                raise SystemExit(f"[FAIL]  The tarball is version '{got_version}', the pin says {version}")
            specs = sorted((src / 'specs').iterdir()) if (src / 'specs').is_dir() else []
            for f in specs:
                if not SPEC.fullmatch(f.name) or not f.is_file():
                    raise SystemExit(f'[FAIL]  Unexpected file in specs/: {f.name}')
            if not specs:
                raise SystemExit('[FAIL]  The tarball holds no specs')
            new, old = dest.with_name(dest.name + f'.new.{os.getpid()}'), dest.with_name(dest.name + f'.old.{os.getpid()}')
            shutil.rmtree(new, ignore_errors=True)
            new.mkdir(parents=True)
            for f in specs:
                shutil.copyfile(f, new / f.name)
            for name in ('VERSION', 'LICENSE', 'NOTICE.md', 'README.md'):
                if (src / name).is_file():
                    shutil.copyfile(src / name, new / name)
            (new / '.source').write_text(expect + '\n', encoding='utf-8')
            if dest.exists():
                dest.rename(old)
            try:
                new.rename(dest)
            except OSError:
                if old.exists():
                    old.rename(dest)
                shutil.rmtree(new, ignore_errors=True)
                raise
            shutil.rmtree(old, ignore_errors=True)
    print(f'[ OK ]  Installed {len(specs)} plug-in panel specs ({version}) in {dest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
