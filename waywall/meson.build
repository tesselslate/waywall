executable('waywall',
  protocol_sources, protocol_headers,

  'compositor/compositor.c',
  'config.c',
  'cpu.c',
  'instance.c',
  'reset_counter.c',
  'scene_window.c',
  'util.c',
  'waywall.c',

  dependencies : [
    toml,
    wlroots,
    wayland_client,
    wayland_server,
    xcb,
    xkbcommon,
    zip,
  ],

  include_directories : includes,
)
