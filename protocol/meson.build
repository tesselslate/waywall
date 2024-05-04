wayland_scanner_cmd = find_program(
  wayland_scanner.get_variable('wayland_scanner'),
  native: true,
)
wp_dir = wayland_protocols.get_variable('pkgdatadir') + '/'

protocol_headers = []
protocol_sources = []

protocol_xmls = [
  # mesa
  'wayland-drm.xml',

  # standardized protocols (available from wayland-protocols)
  wp_dir + 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  wp_dir + 'stable/viewporter/viewporter.xml',
  wp_dir + 'stable/xdg-shell/xdg-shell.xml',
  wp_dir + 'staging/cursor-shape/cursor-shape-v1.xml',
  wp_dir + 'staging/xwayland-shell/xwayland-shell-v1.xml',
  wp_dir + 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  wp_dir + 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  wp_dir + 'unstable/tablet/tablet-unstable-v2.xml',
  wp_dir + 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
]

foreach xml : protocol_xmls
  protocol_headers += custom_target(
    xml.underscorify() + '-client-header',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner_cmd, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_headers += custom_target(
    xml.underscorify() + '-server-header',
    input: xml,
    output: '@BASENAME@-server-protocol.h',
    command: [wayland_scanner_cmd, 'server-header', '@INPUT@', '@OUTPUT@'],
  )

  protocol_sources += custom_target(
    xml.underscorify() + '-src',
    input: xml,
    output: '@BASENAME@.c',
    command: [wayland_scanner_cmd, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
