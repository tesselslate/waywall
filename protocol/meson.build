wayland_scanner_cmd = find_program(
  wayland_scanner.get_variable('wayland_scanner'),
  native: true,
)
wayland_protocols_dir = wayland_protocols.get_variable('pkgdatadir') + '/'

protocol_headers = []
protocol_sources = []

protocol_xmls = [
  'stable/linux-dmabuf/linux-dmabuf-v1.xml',
]

foreach xml : protocol_xmls
  protocol_headers += custom_target(
    xml.underscorify() + '-client-header',
    input: wayland_protocols_dir + xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner_cmd, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_headers += custom_target(
    xml.underscorify() + '-server-header',
    input: wayland_protocols_dir + xml,
    output: '@BASENAME@-server-protocol.h',
    command: [wayland_scanner_cmd, 'server-header', '@INPUT@', '@OUTPUT@'],
  )

  protocol_sources += custom_target(
    xml.underscorify() + '-src',
    input: wayland_protocols_dir + xml,
    output: '@BASENAME@.c',
    command: [wayland_scanner_cmd, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach