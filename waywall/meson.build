subdir('lua')

waywall_src = files(
  'config/action.c',
  'config/api.c',
  'config/config.c',
  'config/internal.c',
  'server/backend.c',
  'server/buffer.c',
  'server/cursor.c',
  'server/fake_input.c',
  'server/remote_buffer.c',
  'server/server.c',
  'server/ui.c',
  'server/wl_compositor.c',
  'server/wl_data_device_manager.c',
  'server/wl_drm.c',
  'server/wl_output.c',
  'server/wl_seat.c',
  'server/wl_shm.c',
  'server/wp_pointer_constraints.c',
  'server/wp_linux_dmabuf.c',
  'server/wp_relative_pointer.c',
  'server/xdg_decoration.c',
  'server/xdg_shell.c',
  'util/log.c',
  'util/prelude.c',
  'util/str.c',
  'util/syscall.c',
  'util/sysinfo.c',
  'util/zip.c',
  'counter.c',
  'inotify.c',
  'instance.c',
  'main.c',
  'reload.c',
  'subproc.c',
  'timer.c',
  'wrap.c',
)

if get_option('xwayland').enabled()
  waywall_src += files(
    'server/xserver.c',
    'server/xwayland.c',
    'server/xwayland_shell.c',
    'server/xwm.c',
  )
endif

executable('waywall',
  waywall_src,
  waywall_lua,
  protocol_headers,
  protocol_sources,

  dependencies: waywall_deps,
  include_directories: includes,
)
