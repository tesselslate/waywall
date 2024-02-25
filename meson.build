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

wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner')
wayland_server = dependency('wayland-server')

subdir('protocol')
subdir('waywall')
