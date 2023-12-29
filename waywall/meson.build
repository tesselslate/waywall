lua = [
  'boot.lua',
  'builtin_moving.lua',
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

  'compositor/buffer.c',
  'compositor/remote_buf.c',
  'compositor/server.c',
  'compositor/wl_compositor.c',
  'compositor/wl_output.c',
  'compositor/wl_seat.c',
  'compositor/wl_shm.c',
  'compositor/wp_relative_pointer.c',
  'compositor/wp_linux_dmabuf.c',
  'compositor/xdg_shell.c',
  'compositor/xdg_decoration.c',
  # 'compositor/wp_pointer_constraints.c',
  'util.c',
  # 'config.c',
  # 'cpu.c',
  # 'instance.c',
  # 'layout.c',
  # 'ninb.c',
  # 'reset_counter.c',
  # 'util.c',
  # 'wall.c',
  # 'waywall.c',

  dependencies : [
    luajit,
    toml,
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
