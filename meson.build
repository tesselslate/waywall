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

toml = subproject('tomlc99').get_variable('toml')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner', native : true)
wlroots = wlroots_subproject.get_variable('wlroots')
xcb = dependency('xcb')
xkbcommon = dependency('xkbcommon')

cc = meson.get_compiler('c')
if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif

wl_scanner = find_program(wayland_scanner.get_variable('wayland_scanner'), native : true)
wl_proto_dir = wayland_protocols.get_variable('pkgdatadir')
wl_proto_xml = [
  'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
]

wl_proto_header = []
wl_proto_src = []
foreach proto : wl_proto_xml
  wl_proto_header += custom_target(
    proto.underscorify() + '-header',
    input : wl_proto_dir + '/' + proto,
    output : '@BASENAME@-protocol.h',
    command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  wl_proto_src += custom_target(
    proto.underscorify() + '-src',
    input : wl_proto_dir + '/' + proto,
    output : '@BASENAME@.c',
    command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

executable('waywall',
  wl_proto_src, wl_proto_header,
  'compositor.c', 'compositor.h',
  'config.c', 'config.h',
  'util.c', 'util.h',
  'waywall.c',

  dependencies : [
    toml,
    wlroots,
    wayland_client,
    wayland_server,
    xcb,
    xkbcommon,
  ],
)
