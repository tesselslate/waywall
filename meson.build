project('waywall', 'c',
  version : '0.0.0',
  license : 'GPL3',
  default_options : [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(
  [
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
  ],
  language : 'c',
)

luajit_program = find_program('luajit')

luajit = dependency('luajit')
obs = dependency('libobs')
toml = subproject('tomlc99').get_variable('toml')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native : true)
xcb = dependency('xcb')
xkbcommon = dependency('xkbcommon')
zip = dependency('libzip')

cc = meson.get_compiler('c')

includes = include_directories('include')

subdir('protocol')
subdir('waywall')

executable('waywall-launch', 'waywall-launch.c')

# shared_library('waywall-obs',
#   protocol_sources, protocol_headers,
#   'waywall-obs.c',
#   dependencies : [
#     obs,
#     wayland_client,
#   ],
# )
