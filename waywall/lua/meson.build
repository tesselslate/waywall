luajit_cmd = find_program('luajit')

waywall_lua_files = [
  'api.lua',
  'helpers.lua',
  'init.lua',
]

waywall_lua = []

foreach lua : waywall_lua_files
  waywall_lua += custom_target(
    lua,
    input: lua,
    output: '@BASENAME@.h',
    command: [luajit_cmd, '-b', '-g', '@INPUT@', '@OUTPUT@'],
  )
endforeach
