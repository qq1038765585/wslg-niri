project('wsland-api', 'c', version : '1.0.0', default_options: ['warning_level=3', 'c_std=c11'])

cc = meson.get_compiler('c')
pkgconfig = import('pkgconfig')

add_project_arguments(cc.get_supported_arguments([
 '-D_GNU_SOURCE',
 '-Wno-unused-parameter',
 '-Werror=false'
]), language: 'c')

dep_winpr = dependency('winpr3', version : '>=3.32.0')
dep_freerdp = dependency('freerdp3', version : '>=3.32.0')
dep_freerdp_server = dependency('freerdp-server3', version : '>=3.32.0')
dep_xkbcommon = dependency('xkbcommon', version : '>=1.11.0')
dep_openssl = dependency('openssl', version: '>= 3.0.0')
dep_wayland = dependency('wayland-server', required: true)
def_pixman = dependency('pixman-1', required: true)
def_cairo = dependency('cairo', required: true)

include_dir = include_directories('include')

wsland_lib = shared_library(
    'wsland',
    sources: [
        'src/server.c',
        'src/client.c',
        'src/handle.c',
        'src/crypto.c',
        'src/channel/audiorx.c',
        'src/channel/audiotx.c',
        'src/channel/clipboard.c',
        'src/channel/drdynvc.c',
        'src/channel/gfxredir.c',
        'src/channel/rdpgfx.c',
        'src/channel/disp.c',
        'src/channel/rail.c',
    ],
    include_directories: include_dir,
    dependencies: [dep_winpr, dep_freerdp, dep_freerdp_server, dep_openssl, def_cairo, def_pixman, dep_wayland],
    install: true,
)

install_headers(
    'include/wsland/wsland.h',
    subdir: 'wsland',
)

pkgconfig.generate(
    wsland_lib,
    name: 'wsland',
    description: 'Wsland FreeRDP RAIL server',
    version: meson.project_version(),
    filebase: 'wsland',
    requires: [
        'winpr3',
        'freerdp3',
        'freerdp-server3',
        'wayland-server',
        'xkbcommon',
        'openssl',
        'pixman-1',
        'cairo',
    ],
)
