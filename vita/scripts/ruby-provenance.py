#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build-time receipts for MRI; never adopt an archive from patch names alone."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PIN = '4d85560cf65938d7883a323bf553acad1faf5eae'
RECORD = 'vita-provenance.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def files(base, paths):
    return {str(p.relative_to(base)): digest(p) for p in sorted(set(paths)) if p.is_file()}


def tree(base):
    return files(base, base.rglob('*'))


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def read(path):
    value = json.loads(path.read_text())
    require(isinstance(value, dict) and value.get('schema') == 1,
            'missing or unsupported build receipt')
    return value


def write(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    temporary.replace(path)


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def recipe():
    paths = list((ROOT / 'vita/patches/ruby').glob('*.patch'))
    paths += list((ROOT / 'vita/ruby').glob('*.c'))
    paths += list((ROOT / 'vita/ruby').glob('*.h'))
    paths += list((ROOT / 'vita/ruby/include').rglob('*.h'))
    paths += [ROOT / name for name in ('vita/ruby/config.site',
              'vita/scripts/build-ruby-vita.sh', 'vita/scripts/ruby-provenance.py')]
    return files(ROOT, paths)


def toolchain():
    result = {}
    for name in ('gcc', 'g++', 'ar', 'ranlib', 'objcopy', 'readelf', 'strip'):
        path = shutil.which('arm-vita-eabi-' + name)
        require(path, 'missing target tool ' + name)
        result[name] = digest(Path(path))
    for name in ('cc1', 'cc1plus', 'as', 'ld'):
        path = command('arm-vita-eabi-gcc', '-print-prog-name=' + name)
        result[name] = digest(Path(shutil.which(path) or path))
    sdk = Path(os.environ['VITASDK']) / 'arm-vita-eabi'
    result['headers'] = tree(sdk / 'include')
    result['libraries'] = {name: digest(sdk / 'lib' / name) for name in
                           ('libc.a', 'libm.a', 'libpthread.a', 'libz.a')}
    result['version'] = command('arm-vita-eabi-gcc', '--version').splitlines()[0]
    return result


def source(src):
    require(command('git', '-C', str(src), 'rev-parse', 'HEAD') == PIN,
            'Ruby source pin mismatch')
    names = command('git', '-C', str(src), 'ls-files', '-z').split('\0')
    # Include files introduced by patches as well as the configure generator output.
    for patch in (ROOT / 'vita/patches/ruby').glob('*.patch'):
        names += [line[6:].split('\t')[0] for line in patch.read_text().splitlines()
                  if line.startswith('+++ b/')]
    names += ['configure', 'tool/config.guess', 'tool/config.sub']
    hashes = files(src, (src / name for name in names if name))
    return {'pin': PIN, 'files_sha256': hashlib.sha256(
        json.dumps(hashes, sort_keys=True).encode()).hexdigest(), 'file_count': len(hashes)}


def inputs(src):
    return {'recipe': recipe(), 'toolchain': toolchain(), 'source': source(src),
            'configure_prefix': os.environ.get('CONFIGURE_PREFIX', '/mkxp-z'),
            'baseruby_sha256': digest(Path(os.environ['BASERUBY']))}


def config(build):
    paths = [build / name for name in ('Makefile', 'GNUmakefile', 'config.status')]
    paths += list((build / '.ext/include').glob('*/ruby/config.h'))
    require(all(p.is_file() for p in paths) and len(paths) > 3,
            'generated Ruby configuration missing')
    return files(build, paths)


def check_build(build, src):
    record = read(build / RECORD)
    require(record.get('state') == 'complete', 'Ruby build did not complete')
    require(record['inputs'] == inputs(src), 'Ruby build inputs changed')
    require(record['config'] == config(build), 'Ruby build configuration changed')
    require(record['archive_sha256'] == digest(build / 'libruby-vita.a'),
            'Ruby build archive changed')
    return record


def installed(prefix):
    result = tree(prefix / 'include')
    result = {'include/' + k: v for k, v in result.items()}
    result.update({'lib/' + k: v for k, v in tree(prefix / 'lib').items()})
    require('lib/libruby-static.a' in result and
            'include/ruby-3.1.0/ruby/config.h' in result and
            'lib/pkgconfig/ruby-3.1.pc' in result and 'lib/ruby/3.1.0/date.rb' in result,
            'Ruby prefix is incomplete')
    return result


def verify(prefix, archive=None, includes=None):
    record = read(prefix / RECORD)
    build = record['build']
    require(build['state'] == 'complete' and build['inputs']['source']['pin'] == PIN
            and build['inputs']['source']['file_count'] > 0 and build['config'],
            'incomplete Ruby build receipt')
    require(build['inputs']['recipe'] == recipe(), 'Ruby recipe/patches changed')
    require(build['inputs']['toolchain'] == toolchain(), 'Ruby toolchain changed')
    require(build['inputs']['configure_prefix'] == os.environ.get('CONFIGURE_PREFIX', '/mkxp-z'),
            'Ruby configure prefix changed')
    require(record['installed'] == installed(prefix), 'installed Ruby bytes changed')
    require(build['archive_sha256'] == record['installed']['lib/libruby-static.a'],
            'installed archive does not match completed Ruby build')
    if archive:
        require(archive.resolve() == (prefix / 'lib/libruby-static.a').resolve(),
                'MRI archive override is outside verified Ruby prefix')
    if includes:
        require(includes.resolve() == (prefix / 'include/ruby-3.1.0').resolve(),
                'MRI headers override is outside verified Ruby prefix')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare', 'configured', 'seal', 'check-build', 'install', 'verify'))
    for name in ('src', 'build', 'prefix', 'archive', 'includes'):
        parser.add_argument('--' + name, type=Path)
    args = parser.parse_args()
    if args.mode == 'verify':
        verify(args.prefix, args.archive, args.includes)
        print('Ruby provenance: verified')
        return
    build, src = args.build, args.src
    path = build / RECORD
    if args.mode == 'prepare':
        current = inputs(src)
        if path.exists():
            old = read(path)
            require(old.get('state') in ('configured', 'complete'), 'interrupted Ruby build')
            require(old['inputs'] == current, 'Ruby build inputs changed')
            require(old['config'] == config(build), 'Ruby build configuration changed')
            if old['state'] == 'complete':
                require(old['archive_sha256'] == digest(build / 'libruby-vita.a'),
                        'Ruby build archive changed')
        else:
            require(not any(build.iterdir()), 'unproven existing Ruby build directory')
        write(path, {'schema': 1, 'state': 'prepared', 'inputs': current})
    elif args.mode in ('configured', 'seal'):
        record = read(path)
        require(record['state'] == ('prepared' if args.mode == 'configured' else 'configured'),
                'invalid Ruby build phase')
        require(record['inputs'] == inputs(src), 'Ruby inputs changed during build')
        record['config'] = config(build)
        record['state'] = 'configured' if args.mode == 'configured' else 'complete'
        if args.mode == 'seal':
            record['archive_sha256'] = digest(build / 'libruby-vita.a')
        write(path, record)
    else:
        record = check_build(build, src)
        if args.mode == 'install':
            outputs = installed(args.prefix)
            require(outputs['lib/libruby-static.a'] == record['archive_sha256'],
                    'installed archive differs from build')
            write(args.prefix / RECORD, {'schema': 1, 'build': record, 'installed': outputs})


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f'Ruby provenance: {error}. Rebuild with vita/scripts/build-ruby-vita.sh build '
              'using private SRC, an empty BUILD and PREFIX; do not reuse or relabel old archives.',
              file=sys.stderr)
        sys.exit(1)
