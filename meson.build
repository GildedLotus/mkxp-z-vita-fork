project('mkxp-z', 'c', 'cpp', version: '2.4.2', meson_version: '>=0.56.0', default_options: ['cpp_std=c++14', 'buildtype=release'])

xxd = find_program('xxd', native: true)

git_hash = run_command('git', 'rev-parse', '--short', 'HEAD').stdout().strip()

compilers = {'cpp': meson.get_compiler('cpp')}
if compilers['cpp'].get_define('__vita__') == ''
    error('Use vita/scripts/configure-vita.sh to cross-build the PS Vita player.')
endif

global_sources = []
global_dependencies = []
global_include_dirs = []
global_args = []
global_link_args = []

global_args += '-DMKXPZ_BUILD_MESON'
global_args += '-DMKXPZ_VERSION="@0@"'.format(meson.project_version())
global_args += '-DMKXPZ_GIT_HASH="@0@"'.format(git_hash)
global_args += '-DHAVE_NANOSLEEP'
# ====================
# Ext libs
# ====================

# vitaGL provides the GLES2 API; desktop OpenGL is not a build target.
global_args += '-DGLES2_HEADER'

# Expensive Vita render/input diagnostics are opt-in. Keep boot/error logs.
if get_option('vita_frame_trace')
    global_args += '-DVITA_GLUE_FRAME_TRACE=1'
endif

# CPU-authoritative Bitmap backend. Every Bitmap keeps
# its pixels in client memory, all Bitmap mutation is software (vita/swraster),
# and the GL texture is a lazily created, lazily uploaded, sample-only cache.
# A Bitmap then never owns a render target, which is what exhausts the
# fixed pool of GPU render surfaces. Off by default: stock mkxp-z
# behaviour is untouched.
if get_option('software_bitmaps') == true
    global_args += '-DMKXPZ_SOFTWARE_BITMAPS'
endif

# Launcher mode. One eboot: with no
# --game and no pinned root config it draws the game list with SDL2 alone and
# never loads Ruby, and picking a game is sceAppMgrLoadExec of the same eboot.
# MRI cannot be re-initialised in process, so "quit to the launcher" has to be
# a process replacement, and a second eboot would duplicate the whole player
# executable just to draw a list.
# Off by default: without it src/main.cpp is the pinned player it always was.
if get_option('vita_launcher') == true
    global_args += '-DMKXPZ_VITA_LAUNCHER'
endif

# ====================
# Main source
# ====================

# Suppress warnings
global_args += ['-Wno-non-virtual-dtor', '-Wno-reorder', '-Wno-uninitialized', '-Wno-unknown-pragmas', '-Wno-stringop-truncation']
if compilers['cpp'].get_id() == 'clang'
    global_args += ['-Wno-undefined-var-template', '-Wno-delete-non-abstract-non-virtual-dtor']
endif
# Decide whether or not to use MiniFFI
miniffi = get_option('use_miniffi')
if miniffi == true
    miniffi = true
    global_args += '-DMKXPZ_MINIFFI'
endif

# Defines
if get_option('workdir_current')
    global_args += '-DWORKDIR_CURRENT'
endif

subdir('src')
subdir('binding')
subdir('shader')
subdir('assets')

global_include_dirs += include_directories('src', 'binding')

# Keep the Vita build/package target stable; package-vpk.sh owns installation.
exe_name = meson.project_name() + '.' + host_machine.cpu()

executable(exe_name,
    sources: global_sources,
    dependencies: global_dependencies,
    include_directories: global_include_dirs,
    link_args: global_link_args,
    cpp_args: global_args,
    install: false
)
