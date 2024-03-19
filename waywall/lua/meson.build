luajit_program = find_program('luajit')

lua_files = [
  'api.lua',
  'helpers.lua',
  'init.lua',
]

lua_headers = []

foreach lua : lua_files
  lua_headers += custom_target(
    lua,
    input: lua,
    output: '@BASENAME@.h',
    command: [luajit_program, '-b', '@INPUT@', '@OUTPUT@'],
  )
endforeach
