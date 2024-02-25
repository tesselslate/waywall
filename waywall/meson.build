executable('waywall',
  'server/buffer.c',
  'server/server.c',
  'server/wl_shm.c',
  'main.c',
  'util.c',

  dependencies: [
    wayland_client,
    wayland_server,
  ],

  include_directories: includes,
)
