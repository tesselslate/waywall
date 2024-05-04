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

libzip = dependency('libzip')
luajit = dependency('luajit')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

waywall_deps = [
  cc.find_library('m'),

  libzip,
  luajit,
  wayland_client,
  wayland_cursor,
  wayland_server,
  xkbcommon,
]

wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner')

if get_option('xwayland').enabled()
  waywall_deps += dependency('xcb')
  waywall_deps += dependency('xcb-composite')
  waywall_deps += dependency('xcb-errors')
  waywall_deps += dependency('xcb-res')
  waywall_deps += dependency('xwayland')

  add_project_arguments(
    [
      '-DWAYWALL_XWAYLAND',
    ],
    language: 'c',
  )
endif

subdir('protocol')
subdir('test')
subdir('waywall')
