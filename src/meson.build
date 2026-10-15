if not (get_option('scanner') or get_option('libraries'))
	error('Either -Dscanner=true or -Dlibraries=true is required')
endif

wayland_version_h = configuration_data()
wayland_version_h.set('POOR_WAYLAND_VERSION', meson.project_version())
wayland_version_h.set('POOR_WAYLAND_VERSION_MAJOR', wayland_version[0].to_int())
wayland_version_h.set('POOR_WAYLAND_VERSION_MINOR', wayland_version[1].to_int())
wayland_version_h.set('POOR_WAYLAND_VERSION_MICRO', wayland_version[2].to_int())
configure_file(
	input: 'poor_wayland_version.h.in',
	output: 'poor_wayland_version.h',
	configuration: wayland_version_h,
	install: get_option('libraries'),
	install_dir: join_paths(get_option('prefix'), get_option('includedir'))
)


mathlib_dep = cc.find_library('m', required: false)

wayland_util = static_library(
	'wayland-util',
	sources: 'wayland-util.c'
)

# Whole, so both libraries export the poor_wl_list and poor_wl_array functions
wayland_util_dep = declare_dependency(
	link_whole: wayland_util,
	include_directories: include_directories('.'),
	dependencies: mathlib_dep
)

if get_option('scanner')
	# poor_wayland_scanner

	scanner_deps = [ dependency('expat') ]
	scanner_args = [ '-include', 'config.h' ]

	if get_option('dtd_validation')
		scanner_deps += dependency('libxml-2.0')
		scanner_args += '-DHAVE_LIBXML=1'
	endif

	prog_embed = find_program('embed.py', native: true)

	embed_dtd = custom_target(
		'wayland.dtd.h',
		input: '../protocol/wayland.dtd',
		output: 'wayland.dtd.h',
		command: [ prog_embed, '@INPUT@', 'wayland_dtd' ],
		capture: true
	)

	wayland_scanner_sources = [ 'scanner.c', embed_dtd ]
	wayland_scanner_includes = [ root_inc, protocol_inc ]

	wayland_scanner = executable(
		'poor_wayland_scanner',
		wayland_scanner_sources,
		c_args: scanner_args,
		include_directories: wayland_scanner_includes,
		dependencies: [ scanner_deps, wayland_util_dep, ],
		install: true
	)

	pkgconfig.generate(
		name: 'poor_wayland_scanner',
		description: 'Code generator for poor_wayland protocols',
		version: meson.project_version(),
		variables: [
			'datarootdir=' + join_paths('${prefix}', get_option('datadir')),
			'pkgdatadir=' + join_paths('${pc_sysrootdir}${datarootdir}', meson.project_name()),
			'poor_wayland_scanner=${bindir}/poor_wayland_scanner'
		],
		filebase: 'poor_wayland_scanner'
	)

	if meson.can_run_host_binaries()
		meson.override_find_program('poor_wayland_scanner', wayland_scanner)
		meson.override_dependency('poor_wayland_scanner', declare_dependency(
			variables: { 'poor_wayland_scanner': 'poor_wayland_scanner' },
		))
	endif
endif

if not meson.can_run_host_binaries() or not get_option('scanner')
	scanner_dep = dependency('poor_wayland_scanner', native: true, version: meson.project_version())
	wayland_scanner_for_build = find_program(scanner_dep.get_variable(pkgconfig: 'poor_wayland_scanner'))
else
	wayland_scanner_for_build = wayland_scanner
endif

if get_option('libraries')
	# wayland libraries

	threads_dep = dependency('threads', required: false)

	wayland_private = static_library(
		'wayland-private',
		sources: [
			'connection.c',
			'wayland-private.c',
		],
		dependencies: [ ffi_dep, poor_loop_dep ]
	)

	wayland_private_dep = declare_dependency(
		link_with: wayland_private,
		include_directories: include_directories('.'),
		dependencies: [ poor_loop_dep ]
	)

	generated_headers = [
		{
			'scanner_args': ['server-header'],
			'output': 'poor_wayland_server_protocol.h',
			'variable': 'wayland_server_protocol_h',
			'install': true,
		},
		{
			'scanner_args': ['server-header', '-c'],
			'output': 'poor_wayland_server_protocol_core.h',
			'variable': 'wayland_server_protocol_core_h',
			'install': false,
		},
		{
			'scanner_args': ['client-header'],
			'output': 'poor_wayland_client_protocol.h',
			'variable': 'wayland_client_protocol_h',
			'install': true,
		},
		{
			'scanner_args': ['client-header', '-c'],
			'output': 'poor_wayland_client_protocol_core.h',
			'variable': 'wayland_client_protocol_core_h',
			'install': false,
		},
	]

	foreach gen: generated_headers
		scanner_args = gen['scanner_args']
		output_file = gen['output']
		install_file = gen['install']
		install_dir = join_paths(get_option('prefix'), get_option('includedir'))
		target_name = gen['variable']

		target = custom_target(
			target_name,
			command: [
				wayland_scanner_for_build, '-s', scanner_args,
				'@INPUT@', '@OUTPUT@'
			],
			input: wayland_protocol_xml,
			output: output_file,
			install: install_file,
			install_dir: install_dir
		)

		set_variable(target_name, target)
	endforeach

	wayland_protocol_c = custom_target(
		'protocol source',
		command: [
			wayland_scanner_for_build, '-s', 'public-code', '@INPUT@', '@OUTPUT@'
		],
		input: wayland_protocol_xml,
		output: 'poor_wayland_protocol.c'
	)

	wayland_server = library(
		'poor_wayland_server',
		sources: [
			wayland_server_protocol_core_h,
			wayland_server_protocol_h,
			wayland_protocol_c,
			'wayland-server.c',
			'wayland-shm.c',
		],
		version: meson.project_version(),
		soversion: '0',
		dependencies: [
			ffi_dep,
			wayland_private_dep,
			wayland_util_dep,
			mathlib_dep,
			threads_dep,
			poor_loop_dep,
		],
		include_directories: root_inc,
		install: true
	)

	wayland_server_dep = declare_dependency(
		link_with: wayland_server,
		include_directories: [ root_inc, include_directories('.') ],
		dependencies: [ ffi_dep, mathlib_dep, threads_dep, poor_loop_dep ],
		sources: [
			wayland_server_protocol_core_h,
			wayland_server_protocol_h
		]
	)

	pkgconfig.generate(
		wayland_server,
		name: 'poor_wayland_server',
		description: 'Wayland server library on io_uring',
		version: meson.project_version(),
		filebase: 'poor_wayland_server',
		libraries: mathlib_dep,
		requires: [ 'poor_loop', 'liburing' ],
		variables: [
			'datarootdir=' + join_paths('${prefix}', get_option('datadir')),
			'pkgdatadir=' + join_paths('${pc_sysrootdir}${datarootdir}', meson.project_name())
		]
	)

	meson.override_dependency('poor_wayland_server', wayland_server_dep)

	wayland_client = library(
		'poor_wayland_client',
		sources: [
			wayland_client_protocol_core_h,
			wayland_client_protocol_h,
			wayland_protocol_c,
			'wayland-client.c'
		],
		version: meson.project_version(),
		soversion: '0',
		dependencies: [
			ffi_dep,
			wayland_private_dep,
			wayland_util_dep,
			mathlib_dep,
			poor_loop_dep,
		],
		include_directories: root_inc,
		install: true
	)

	pkgconfig.generate(
		wayland_client,
		name: 'poor_wayland_client',
		description: 'Wayland client library on io_uring',
		version: meson.project_version(),
		filebase: 'poor_wayland_client',
		libraries: mathlib_dep,
		requires: [ 'poor_loop', 'liburing' ],
		variables: [
			'datarootdir=' + join_paths('${prefix}', get_option('datadir')),
			'pkgdatadir=' + join_paths('${pc_sysrootdir}${datarootdir}', meson.project_name())
		]
	)

	wayland_client_dep = declare_dependency(
		link_with: wayland_client,
		include_directories: [ root_inc, include_directories('.') ],
		dependencies: [ poor_loop_dep ],
		sources: [
			wayland_client_protocol_core_h,
			wayland_client_protocol_h
		]
	)

	meson.override_dependency('poor_wayland_client', wayland_client_dep)

	install_headers([
		'poor_wayland_util.h',
		'poor_wayland_connection.h',
		'poor_wayland_server.h',
		'poor_wayland_server_core.h',
		'poor_wayland_server_display.h',
		'poor_wayland_client.h',
		'poor_wayland_client_core.h',
		'poor_wayland_client_display.h',
	])
endif
