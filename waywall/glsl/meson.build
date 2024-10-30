luajit_cmd = find_program('luajit')
embed_lua = find_program('./embed.lua', native: true)

waywall_glsl_files = [
  'texcopy.frag',
  'texcopy.vert',
]

waywall_glsl = []

foreach glsl : waywall_glsl_files
  waywall_glsl += custom_target(
    glsl,
    input: glsl,
    output: '@PLAINNAME@.h',
    command: [luajit_cmd, embed_lua, '@INPUT@', '@OUTPUT@'],
  )
endforeach
