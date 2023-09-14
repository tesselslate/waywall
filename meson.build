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
    '-Wno-unused-parameter',
  ],
  language: 'c',
)

wlroots_subproject = subproject(
  'wlroots',
  default_options : [
    'examples=false',
  ],
)
if not wlroots_subproject.get_variable('features').get('xwayland')
  error('xwayland is required')
endif

wayland_server = dependency('wayland-server')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native: true)
wlroots = wlroots_subproject.get_variable('wlroots')
xkbcommon = dependency('xkbcommon')

cc = meson.get_compiler('c')
if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif

executable('waywall',
  'main.c',

  dependencies: [
    wlroots,
    wayland_server,
    xkbcommon,
  ],
)
