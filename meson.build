project('waywall', 'c',
  version: '0.0.0',
  license: 'GPL3',
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
source_root = meson.current_source_dir().split('/')
build_root = meson.global_build_root().split('/')
relative_dir_parts = []
i = 0
in_prefix = true
foreach p : build_root
    if i >= source_root.length() or not in_prefix or p != source_root[i]
        in_prefix = false
        relative_dir_parts += '..'
    endif
    i += 1
endforeach
i = 0
in_prefix = true
foreach p : source_root
    if i >= build_root.length() or not in_prefix or build_root[i] != p
        in_prefix = false
        relative_dir_parts += p
    endif
    i += 1
endforeach
relative_dir = join_paths(relative_dir_parts) + '/'

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

waywall_deps = [
  cc.find_library('m'),

  luajit,
  wayland_client,
  wayland_cursor,
  wayland_server,
  xkbcommon,
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
