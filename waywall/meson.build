lua = [
  'boot.lua',
  'builtin_wall.lua',
]

lua_targets = []

foreach lua_file : lua
  lua_targets += custom_target(
    lua_file,
    input : lua_file,
    output : '@PLAINNAME@.o',
    command : [ luajit_program, '-b', '-g', '@INPUT@', '@OUTPUT@' ],
  )
endforeach

lua_lib = static_library('waywall-lua', lua_targets)

executable('waywall',
  protocol_sources, protocol_headers,

  'compositor/compositor.c',
  'compositor/hview.c',
  'compositor/input.c',
  'compositor/render.c',
  'compositor/scene_window.c',
  'compositor/xwayland.c',

  'config.c',
  'cpu.c',
  'instance.c',
  'layout.c',
  'ninb.c',
  'reset_counter.c',
  'util.c',
  'wall.c',
  'waywall.c',

  dependencies : [
    luajit,
    toml,
    wlroots,
    wayland_client,
    wayland_server,
    xcb,
    xkbcommon,
    zip,
  ],
  link_args : [
    '-Wl,-E',
  ],
  link_whole : [
    lua_lib,
  ],
  include_directories : includes,
)
