project('steam-tdp-bridge', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2', 'optimization=2'])

gio = dependency('gio-2.0')
glib = dependency('glib-2.0')

sources = files(
  'source/main.c',
  'source/sysfs.c',
  'source/config.c',
  'source/steam.c',
  'source/tdp.c',
)

executable('steam-tdp-bridge',
  sources,
  dependencies: [gio, glib],
  install: true,
  install_dir: get_option('bindir'))
