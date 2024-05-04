project('waywall', 'c',
  version: '0.0.0',
  license: 'GPL3',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(
  [
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
  ],
  language: 'c',
)

includes = include_directories('include')

cc = meson.get_compiler('c')

libm = cc.find_library('m')
libzip = dependency('libzip')
luajit = dependency('luajit')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

xcb = dependency('xcb')
xcb_composite = dependency('xcb-composite')
xcb_errors = dependency('xcb-errors')
xcb_res = dependency('xcb-res')
xwayland = dependency('xwayland')

subdir('protocol')
subdir('test')
subdir('waywall')
