project('sway-nook', 'c', version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

gtk = dependency('gtk+-3.0')
layer = dependency('gtk-layer-shell-0')
json = dependency('json-glib-1.0')
deps = [gtk, layer, json]
inc = include_directories('src')

common = files('src/model.c', 'src/theme.c', 'src/ipc.c', 'src/snapshot.c')
data_dir = get_option('prefix') / get_option('datadir') / 'sway-nook'
executable('sway-nook', common, files('src/main.c', 'src/cli.c', 'src/ui.c'),
  include_directories: inc, dependencies: deps, install: true,
  c_args: ['-DNOOK_DATA_DIR="' + data_dir + '"'])
install_data('data/style.css', install_dir: data_dir)
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses' / 'sway-nook')
install_data('config/theme.ini', install_dir: get_option('datadir') / 'doc' / 'sway-nook')
install_data('config/sway-nook.conf', install_dir: get_option('datadir') / 'doc' / 'sway-nook')

test_binary = executable('test-native', files('tests/test_native.c', 'src/model.c', 'src/theme.c', 'src/snapshot.c'),
  include_directories: inc, dependencies: deps)
test('native', test_binary)
