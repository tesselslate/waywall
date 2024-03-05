executable('waywall',
  'server/buffer.c',
  'server/remote_buffer.c',
  'server/server.c',
  'server/ui.c',
  'server/wl_compositor.c',
  'server/wl_shm.c',
  'server/wp_linux_dmabuf.c',
  'server/xdg_decoration.c',
  'server/xdg_shell.c',
  'main.c',
  'util.c',

  protocol_headers,
  protocol_sources,

  dependencies: [
    wayland_client,
    wayland_server,
  ],

  include_directories: includes,
)
