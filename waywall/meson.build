executable('waywall',
  protocol_sources, protocol_headers,

  'compositor/compositor.c',
  'compositor/hview.c',
  'compositor/input.c',
  'compositor/render.c',
  'compositor/scene_window.c',
  'compositor/xwayland.c',
  'compositor/window_util.c',

  'config.c',
  'cpu.c',
  'instance.c',
  'ninb.c',
  'reset_counter.c',
  'util.c',
  'wall.c',
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
