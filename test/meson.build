waywall_tests = {
  'str': ['util/prelude.c', 'util/str.c']
}

foreach name, extra_src : waywall_tests
  fixed_extra_src = []
  foreach _extra_src : extra_src
    fixed_extra_src += meson.global_source_root() + '/waywall/' + _extra_src
  endforeach

  test(name, executable('test_' + name,
    files(name + '.c'),
    files(fixed_extra_src),

    include_directories: includes,
  ))
endforeach
