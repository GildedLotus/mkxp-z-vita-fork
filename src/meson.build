physfs = dependency('physfs', version: '>=2.1', static: true)
openal = dependency('openal', static: true, method: 'pkg-config')
theora = dependency('theora', static: true)
vorbisfile = dependency('vorbisfile', static: true)
vorbis = dependency('vorbis', static: true)
ogg = dependency('ogg', static: true)
sdl2 = dependency('SDL2', static: true)
sdl_sound = compilers['cpp'].find_library('SDL2_sound')
sdl2_ttf = dependency('SDL2_ttf', static: true)
freetype = dependency('freetype2', static: true)
pixman = dependency('pixman-1', static: true)
png = dependency('libpng', static: true)
zlib = dependency('zlib', static: true)
uchardet = dependency('uchardet', static: true)

# Vita dependencies are resolved from the pinned SDK pkg-config prefix.
sdl2_image = dependency('SDL2_image', static: true, method: 'pkg-config')

bz2 = compilers['cpp'].find_library('bz2')

# Vita packages link dependency libraries and compiler runtimes statically.
global_link_args += ['-static-libgcc', '-static-libstdc++']
global_args += '-DAL_LIBTYPE_STATIC'

alcdev_struct = 'ALCdevice_struct'
if openal.type_name() == 'pkgconfig'
    if openal.version().version_compare('>=1.20.1')
        alcdev_struct = 'ALCdevice'
    endif
endif

global_args += '-DMKXPZ_ALCDEVICE=' + alcdev_struct


global_include_dirs += include_directories('.',
    'audio',
    'crypto',
    'display', 'display/gl', 'display/libnsgif', 'display/libnsgif/utils',
    'etc',
    'filesystem', 'filesystem/ghc',
    'input',
    'system',
    'util', 'util/sigslot'
)

global_dependencies += [openal, zlib, bz2, sdl2, sdl_sound, pixman, physfs, theora, vorbisfile, vorbis, ogg, sdl2_ttf, freetype, sdl2_image, png, uchardet]

main_source = files(
    'main.cpp',
    'config.cpp',
    'eventthread.cpp',
    'sharedstate.cpp',
    
    'audio/alstream.cpp',
    'audio/audio.cpp',
    'audio/audiostream.cpp',
    'audio/fluid-fun.cpp',
    'audio/midisource.cpp',
    'audio/sdlsoundsource.cpp',
    'audio/soundemitter.cpp',
    'audio/vorbissource.cpp',
    'theoraplay/theoraplay.c',

    'crypto/rgssad.cpp',

    'display/autotiles.cpp',
    'display/autotilesvx.cpp',
    'display/bitmap.cpp',
    'display/font.cpp',
    'display/graphics.cpp',
    'display/plane.cpp',
    'display/sprite.cpp',
    'display/tilemap.cpp',
    'display/tilemapvx.cpp',
    'display/viewport.cpp',
    'display/window.cpp',
    'display/windowvx.cpp',

    'display/libnsgif/libnsgif.c',
    'display/libnsgif/lzw.c',

    'display/gl/gl-fun.cpp',
    'display/gl/gl-meta.cpp',
    'display/gl/glstate.cpp',
    'display/gl/scene.cpp',
    'display/gl/shader.cpp',
    'display/gl/texpool.cpp',
    'display/gl/tileatlas.cpp',
    'display/gl/tileatlasvx.cpp',
    'display/gl/tilequad.cpp',
    'display/gl/vertex.cpp',

    'util/iniconfig.cpp',
    
    'etc/etc.cpp',
    'etc/table.cpp',

    'filesystem/filesystem.cpp',
    'filesystem/filesystemImpl.cpp',
    
    'input/input.cpp',
    'input/keybindings.cpp',

    'system/systemImpl.cpp'
)

global_sources += main_source

# ====================
# PS Vita platform (vita/)
# ====================
#
# The Vita sources live in ../vita and are compiled into the executable
# itself, not passed as cross-file link args, so meson find_library() tests
# are unaffected. Their headers resolve through these include directories.
global_include_dirs += include_directories(
    '../vita/glue',
    '../vita/swraster',
    '../vita/overlay',
    '../vita/textpanel',
    '../vita/launcher',
)

# Boot: log redirect, boot clocks, crash-safe log sink, marker files.
global_sources += files('../vita/glue/vita_glue.c')
global_sources += files('util/loadprofile.cpp')
global_link_args += ['-Wl,--wrap=IMG_LoadPNG_RW', '-Wl,--wrap=_write_r']

# Generation-preserving file publication shared by the fatal report, settings,
# launcher state, last-error and log-rotation writers.
global_sources += files('../vita/glue/vita_publish.c')

# Software raster core for the CPU-authoritative Bitmap backend.
if get_option('software_bitmaps') == true
    global_sources += files('../vita/swraster/swraster.cpp')
endif

# The CPU-composed on-screen overlay (FPS line, frame-profile HUD, settings
# menu). Same guard as the raster core: the overlay is drawn from the
# GPUBudget-bounded present path, which only exists under this backend.
if get_option('software_bitmaps') == true
    global_sources += files('../vita/overlay/overlay.c')
endif

# The error / message panel and the CPU text core it composes with. Two files,
# two different reasons for the guard each one has:
#
#   textpanel.cpp   includes swraster.h, so it is named under the same option
#                   the raster core is.
#   display/erroroverlay.cpp
#                   compiled unconditionally: one declaration in
#                   erroroverlay.h that is always satisfied. Off a Vita, or
#                   without the software backend, its whole body preprocesses
#                   away to an empty vitaShowPanel() and desktop mkxp-z keeps
#                   its real message box.
if get_option('software_bitmaps') == true
    global_sources += files('../vita/textpanel/textpanel.cpp')
endif
global_sources += files('display/erroroverlay.cpp')

# Fatal-path reporting: message logging and the last-error breadcrumb the
# launcher reads. Unconditional: the C library is the only thing it depends
# on, so it builds everywhere.
global_sources += files('vita_fatal.cpp')

# The launcher. vita_boot.cpp is the only file here that knows about the
# engine at all: it parses the root config with the engine's own
# util/json5pp.hpp and reports through src/vita_fatal.cpp, so last-error.txt
# keeps exactly one producer. The rest is a C99 library and front end.
if get_option('vita_launcher') == true
    global_sources += files(
        '../vita/launcher/vita_boot.cpp',
        '../vita/launcher/vita_launcher.c',
        '../vita/launcher/preflight.c',
        '../vita/launcher/launcher_view.c',
        '../vita/launcher/launcher_gl.c',
        '../vita/launcher/vita_loadexec.c',
        '../vita/launcher/game_ini.c',
        '../vita/launcher/game_scan.c',
        '../vita/launcher/launch_args.c',
        '../vita/launcher/launcher_model.c',
        '../vita/launcher/launcher_state.c',
        '../vita/launcher/utf8_util.c',
        '../vita/launcher/last_error.c',
    )
endif
