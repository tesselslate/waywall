test_sources = [
  '../waywall/util.c',
]

test(
  'server/inotify.c',
  executable('inotify',
    '../waywall/inotify.c',
    'inotify.c',
    test_sources,

    dependencies: [wayland_server],
    include_directories: [includes],
  ),
)
test('server/serial.h', executable('serial', 'serial.c', test_sources, include_directories: [includes]))
