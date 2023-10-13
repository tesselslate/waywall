wl_scanner = find_program(wayland_scanner.get_variable('wayland_scanner'), native : true)
protocol_dir = wayland_protocols.get_variable('pkgdatadir') + '/'

protocol_xmls = [
  protocol_dir + 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  protocol_dir + 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  'wlr-export-dmabuf-unstable-v1.xml',
]

foreach proto : protocol_xmls
  protocol_headers += custom_target(
    proto.underscorify() + '-header',
    input : proto,
    output : '@BASENAME@-protocol.h',
    command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(
    proto.underscorify() + '-src',
    input : proto,
    output : '@BASENAME@.c',
    command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
