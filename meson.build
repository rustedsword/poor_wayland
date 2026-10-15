project(
	'poor_wayland', 'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>= 1.4.0',
	default_options: [
		'warning_level=2',
		'buildtype=debugoptimized',
		'c_std=c23',
	]
)

fs = import('fs')
pkgconfig = import('pkgconfig')

wayland_version = meson.project_version().split('.')

config_h = configuration_data()
config_h.set_quoted('PACKAGE', meson.project_name())
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())

compiler_flags = [
	'-Wno-unused-parameter',
	'-Wstrict-prototypes',
	'-Wmissing-prototypes',
	'-fvisibility=hidden',
]

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments(compiler_flags),
	language: 'c'
)

foreach h: [ 'sys/prctl.h' ]
	config_h.set('HAVE_' + h.underscorify().to_upper(), cc.has_header(h))
endforeach

foreach f: [ 'prctl', 'gettid' ]
	config_h.set('HAVE_' + f.underscorify().to_upper(), cc.has_function(f))
endforeach

if get_option('libraries')
	if host_machine.system() != 'linux'
		error('Wayland libraries run on io_uring, which needs Linux')
	endif
	# The subproject, not an installed poor_loop, so the headers match the library. First, so its bundled liburing
	# and poor_base are used if they aren't installed.
	poor_loop_dep = subproject('poor_loop').get_variable('poor_loop_dep')
	liburing_dep = dependency('liburing', version: '>=2.7', include_type: 'system')
	ffi_dep = dependency('libffi')
endif

configure_file(
	output: 'config.h',
	configuration: config_h,
)

wayland_protocol_xml = files('protocol/wayland.xml')

root_inc = include_directories('.')
protocol_inc = include_directories('protocol')
src_inc = include_directories('src')

subdir('src')

if get_option('tests')
	subdir('tests')
endif
if get_option('examples')
	subdir('examples')
endif
if get_option('documentation')
	subdir('doc')
endif

if get_option('scanner')
	install_data([
		'protocol/wayland.xml',
		'protocol/wayland.dtd',
	],
		install_dir: join_paths(get_option('prefix'), get_option('datadir'), meson.project_name()),
	)
endif
