test_sources = [
  '../waywall/util/log.c',
  '../waywall/util/prelude.c',
]

test(
  'inotify.c',
  executable('inotify',
    '../waywall/inotify.c',
    'inotify.c',
    test_sources,

    dependencies: [wayland_server],
    include_directories: [includes],
  ),
)
test('util/serial.h', executable('serial', 'serial.c', test_sources, include_directories: [includes]))
