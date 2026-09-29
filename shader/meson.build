
embedded_shaders = [
    'common.h',
    'transSimple.frag',
    'trans.frag',
    'hue.frag',
    'sprite.frag',
    'plane.frag',
    'gray.frag',
    'bitmapBlit.frag',
    'flatColor.frag',
    'simple.frag',
    'simpleColor.frag',
    'simpleAlpha.frag',
    'simpleAlphaUni.frag',
    'tilemap.frag',
    'flashMap.frag',
    'minimal.vert',
    'simple.vert',
    'simpleColor.vert',
    'sprite.vert',
    'tilemap.vert',
    'tilemapvx.vert',
    'blur.frag',
    'blurH.vert',
    'blurV.vert',
    'simpleMatrix.vert',
    'kglInvert.frag',
    'kglCompressAlpha.frag',
    'kglSubtract.frag',
    'movieYuv.frag',
]

# Optional shaders: kglShadow H/V (KGL2 soft shadow), bicubic/lanczos3 scalers.
# Omitted entirely when optional_shaders is disabled so ShaderSet never
# constructs programs the driver may reject at boot.
if get_option('optional_shaders') == true
    embedded_shaders += [
        'kglShadowH.frag',
        'kglShadowV.frag',
        'bicubic.frag',
        'lanczos3.frag',
    ]
endif

# xBRZ shader is GPLv3.
if get_option('enable-https') == true
    embedded_shaders += 'xbrz.frag'
endif

embedded_shaders_f = files(embedded_shaders)

count = 0
foreach file : embedded_shaders_f
    global_sources += custom_target(embedded_shaders[count],
        input: file,
        output: '@0@.xxd'.format(embedded_shaders[count]),
        command: [
            # Rewrite xxd's path-derived symbols to the stable
            # ___shader_<basename> form shader.cpp expects (linux/Makefile
            # uses `xxd -i ../shader/<file>`). See assets/meson.build.
            'python3', '-c',
            '''
import os, re, subprocess, sys
f = sys.argv[1]
base = os.path.basename(f).replace('.', '_').replace('-', '_')
out = subprocess.check_output(['xxd', '-i', f], text=True)
out = re.sub(r'unsigned char [A-Za-z0-9_]+', 'unsigned char ___shader_' + base, out, count=1)
out = re.sub(r'unsigned int [A-Za-z0-9_]+', 'unsigned int ___shader_' + base + '_len', out, count=1)
sys.stdout.write(out)
''',
            '@INPUT@'
        ],
        capture: true,
        depend_files: embedded_shaders_f[count]
        )
    count += 1
endforeach
