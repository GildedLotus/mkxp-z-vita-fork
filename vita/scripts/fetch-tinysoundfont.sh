#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch only the two pinned headers (tsf.h MIT, tml.h zlib).
# TSF_PREFIX overrides build/third-party/tinysoundfont. --check is offline.
# TSF_SOURCE_DIR permits installing a verified local copy.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 - "$ROOT/vita/scripts/fetch-tinysoundfont.json" \
  "${TSF_PREFIX:-$ROOT/build/third-party/tinysoundfont}" "${1:-}" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

manifest, prefix, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
record = json.loads(manifest.read_text())
if mode not in ('', '--check'):
    sys.exit('usage: fetch-tinysoundfont.sh [--check]')
if set(record['files']) != {'tsf.h', 'tml.h'} or not re.fullmatch('[0-9a-f]{40}', record['commit']):
    sys.exit('fetch-tinysoundfont: invalid pin manifest')


def verified(directory):
    result = {}
    for name, digest in record['files'].items():
        path = directory / name
        if not path.is_file():
            return None
        blob = path.read_bytes()
        if hashlib.sha256(blob).hexdigest() != digest:
            return None
        result[name] = blob
    return result


def license_text(headers):
    notices = []
    for name, blob in headers.items():
        notice = blob.decode('utf-8').split('   LICENSE ', 1)[1].split('*/', 1)[0]
        notices.append(name + ' — upstream license\n' + '\n'.join(line[3:] if line.startswith('   ') else line
                                                   for line in notice.strip('\r\n').splitlines()).strip() + '\n')
    return '\n'.join(notices).encode('utf-8')


headers = verified(prefix)
if headers and (prefix / 'LICENSE').is_file() and (prefix / 'LICENSE').read_bytes() == license_text(headers):
    print('fetch-tinysoundfont: verified headers and LICENSE already present')
    sys.exit(0)
if mode == '--check':
    sys.exit('fetch-tinysoundfont: missing or unverified headers/LICENSE; run the fetch script explicitly')
prefix.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='.tinysoundfont-', dir=prefix.parent) as scratch:
    stage = Path(scratch)
    for name in record['files']:
        if headers:
            (stage / name).write_bytes(headers[name])
        elif os.environ.get('TSF_SOURCE_DIR'):
            (stage / name).write_bytes((Path(os.environ['TSF_SOURCE_DIR']) / name).read_bytes())
        else:
            url = 'https://raw.githubusercontent.com/schellingb/TinySoundFont/' + record['commit'] + '/' + name
            subprocess.run(['curl', '--proto', '=https', '--tlsv1.2', '--fail', '--silent',
                            '--show-error', url, '-o', str(stage / name)], check=True)
    headers = verified(stage)
    if headers is None:
        sys.exit('fetch-tinysoundfont: SHA-256 mismatch; nothing installed')
    (stage / 'LICENSE').write_bytes(license_text(headers))
    prefix.mkdir(parents=True, exist_ok=True)
    for name in (*record['files'], 'LICENSE'):
        os.replace(stage / name, prefix / name)
print('fetch-tinysoundfont: installed verified tsf.h, tml.h and upstream license notices')
PY
