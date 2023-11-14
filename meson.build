project('waywall', 'c',
  version : '0.0.0',
  license : 'GPL3',
  default_options : [
    'c_std=c11',
    'warning_level=2',

    'default_library=static', # TODO: Don't force static linking (required right now for xwm_destroy)
    'wlroots:allocators=auto',
    'wlroots:backends=x11',
    'wlroots:examples=false',
    'wlroots:renderers=auto',
    'wlroots:session=disabled',
    'wlroots:xcb-errors=auto',
    'wlroots:xwayland=enabled',
  ],
)

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
  ],
  language : 'c',
)

wlroots_subproject = subproject('wlroots')
if not wlroots_subproject.get_variable('features').get('xwayland')
  error('wlroots xwayland is required')
endif

luajit_program = find_program('luajit')

luajit = dependency('luajit')
obs = dependency('libobs')
toml = subproject('tomlc99').get_variable('toml')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native : true)
wlroots = wlroots_subproject.get_variable('wlroots')
xcb = dependency('xcb')
xkbcommon = dependency('xkbcommon')
zip = dependency('libzip')

cc = meson.get_compiler('c')

includes = include_directories('include')

subdir('protocol')
subdir('waywall')

executable('waywall-launch', 'waywall-launch.c')

shared_library('waywall-obs',
  protocol_sources, protocol_headers,
  'waywall-obs.c',
  dependencies : [
    obs,
    wayland_client,
  ],
)
