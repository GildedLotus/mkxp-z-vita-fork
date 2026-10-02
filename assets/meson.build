embedded_assets = [
    'liberation.ttf'
]

embedded_assets_f = files(embedded_assets)

count = 0
foreach file : embedded_assets_f
    # Rewrite xxd's path-derived symbols to the stable ___assets_<basename>
    # form used by font.cpp. Meson hands xxd
    # a build-dir-relative @INPUT@ path, whose symbol name would otherwise
    # depend on the source/build layout.
    global_sources += custom_target(embedded_assets[count],
    input: file,
    output: '@0@.xxd'.format(embedded_assets[count]),
    command: [
        'python3', '-c',
        '''
import os, re, subprocess, sys
f = sys.argv[1]
base = os.path.basename(f).replace('.', '_').replace('-', '_')
out = subprocess.check_output(['xxd', '-i', f], text=True)
out = re.sub(r'unsigned char [A-Za-z0-9_]+', 'unsigned char ___assets_' + base, out, count=1)
out = re.sub(r'unsigned int [A-Za-z0-9_]+', 'unsigned int ___assets_' + base + '_len', out, count=1)
sys.stdout.write(out)
''',
        '@INPUT@',
    ],
    capture: true,
    depend_files: embedded_assets_f[count]
    )
    count += 1
endforeach
