project('waywall', 'c',
  version: '0.0.0',
  license: 'GPL3',
  meson_version: '>=1.3.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(
  [
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
  ],
  language: 'c',
)

includes = include_directories('include')

cc = meson.get_compiler('c')

# Remove path start from __FILE__ macro expansion (from wlroots)
fs = import('fs')
relative_dir = fs.relative_to(meson.current_source_dir(), meson.global_build_root()) + '/'
if cc.has_argument('-fmacro-prefix-map=/prefix/to/hide=')
    add_project_arguments(
        '-fmacro-prefix-map=@0@='.format(relative_dir),
        language: 'c',
    )
endif

# Compile-time dependencies
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = dependency('wayland-scanner')

# Runtime dependencies
luajit = dependency('luajit')
wayland_client = dependency('wayland-client')
wayland_cursor = dependency('wayland-cursor')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

egl = dependency('egl')
glesv2 = dependency('glesv2')
spng = dependency('spng')
wayland_egl = dependency('wayland-egl')

waywall_deps = [
  cc.find_library('m'),

  luajit,
  wayland_client,
  wayland_cursor,
  wayland_server,
  xkbcommon,

  egl,
  glesv2,
  spng,
  wayland_egl,
]

# Xwayland support
if get_option('xwayland').enabled()
  waywall_deps += dependency('xcb')
  waywall_deps += dependency('xcb-composite')
  waywall_deps += dependency('xcb-res')
  waywall_deps += dependency('xcb-xtest')
  waywall_deps += dependency('xwayland')

  add_project_arguments(
    [
      '-DWAYWALL_XWAYLAND',
    ],
    language: 'c',
  )
endif

subdir('protocol')
subdir('waywall')
