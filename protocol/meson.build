protocol_sources = []
protocol_headers = []

wl_scanner = find_program(wayland_scanner.get_variable('wayland_scanner'), native : true)
protocol_dir = wayland_protocols.get_variable('pkgdatadir') + '/'

protocol_xmls = [
  protocol_dir + 'stable/viewporter/viewporter.xml',
  protocol_dir + 'stable/xdg-shell/xdg-shell.xml',
  protocol_dir + 'staging/tearing-control/tearing-control-v1.xml',
  protocol_dir + 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml',
  protocol_dir + 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  protocol_dir + 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  protocol_dir + 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
]

foreach proto : protocol_xmls
  protocol_headers += custom_target(
    proto.underscorify() + '-client-header',
    input : proto,
    output : '@BASENAME@-client-protocol.h',
    command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_headers += custom_target(
    proto.underscorify() + '-server-header',
    input : proto,
    output : '@BASENAME@-server-protocol.h',
    command : [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(
    proto.underscorify() + '-src',
    input : proto,
    output : '@BASENAME@.c',
    command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
