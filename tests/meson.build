if not get_option('libraries')
	error('-Dtests=true requires -Dlibraries=true')
endif

test_runner = static_library(
	'test-runner',
	sources: [
		'test-runner.c',
		'test-helpers.c',
	],
	include_directories: [ root_inc, src_inc ],
	dependencies: [
		cc.find_library('dl', required: false),
		dependency('threads'),
	]
)

test_runner_dep = declare_dependency(
	link_with: test_runner,
	include_directories: [ src_inc ],
	dependencies: [
		dependency('threads'),
		cc.find_library('dl', required: false)
	]
)

tests_protocol_xml = files('../protocol/tests.xml')

tests_server_protocol_h = custom_target(
	'test server protocol header',
	command: [ wayland_scanner_for_build, '-s', 'server-header', '@INPUT@', '@OUTPUT@' ],
	input: tests_protocol_xml,
	output: 'tests-server-protocol.h'
)

tests_client_protocol_h = custom_target(
	'test client protocol header',
	command: [ wayland_scanner_for_build, '-s', 'client-header', '@INPUT@', '@OUTPUT@' ],
	input: tests_protocol_xml,
	output: 'tests-client-protocol.h'
)

tests_protocol_c = custom_target(
	'test protocol source',
	command: [ wayland_scanner_for_build, '-s', 'public-code', '@INPUT@', '@OUTPUT@' ],
	input: tests_protocol_xml,
	output: 'tests-protocol.c'
)

exec_fd_leak_checker = executable(
	'exec-fd-leak-checker',
	'exec-fd-leak-checker.c',
	dependencies: test_runner_dep
)

sed_path = find_program('sed').full_path()

if get_option('scanner')
	scanner_test_env = [
		'TEST_DATA_DIR=@0@/data'.format(meson.current_source_dir()),
		'TEST_OUTPUT_DIR=@0@/output'.format(meson.current_build_dir()),
		'SED=@0@'.format(sed_path),
		'WAYLAND_SCANNER=@0@'.format(wayland_scanner.full_path()),
	]

	test(
		'scanner-test',
		find_program('scanner-test.sh'),
		env: scanner_test_env,
	)

	run_target(
		'gen-scanner-test',
		command: find_program('scanner-test-gen.sh'),
		env: scanner_test_env,
	)
endif

client_deps = [ wayland_client_dep, wayland_private_dep, wayland_util_dep, ffi_dep ]
server_deps = [ wayland_server_dep, wayland_private_dep, wayland_util_dep, ffi_dep ]

tests = {
	'array-test': {
		'deps': [ wayland_util_dep ],
	},
	'list-test': {
		'deps': [ wayland_private_dep, wayland_util_dep ],
	},
	'map-test': {
		'deps': [ wayland_private_dep, wayland_util_dep ],
	},
	'fixed-test': {
		'deps': [ wayland_private_dep, wayland_util_dep ],
	},
	'interface-test': {
		'extra_sources': [ wayland_client_protocol_h ],
		'deps': client_deps,
	},
	'message-test': {
		'extra_sources': [ wayland_client_protocol_h ],
		'deps': client_deps,
	},
	'connection-test': {
		'extra_sources': [ wayland_client_protocol_h ],
		'deps': client_deps,
	},
	'signal-test': {
		'extra_sources': [ wayland_server_protocol_h ],
		'deps': server_deps,
	},
	'newsignal-test': {
		'extra_sources': [
			# wayland-server.c is needed here to access wl_priv_* functions
			files('../src/wayland-server.c'),
			wayland_server_protocol_h,
			wayland_protocol_c,
		],
		'deps': [ wayland_private_dep, wayland_util_dep, ffi_dep, poor_loop_dep ],
	},
	'display-test': {
		'extra_sources': [
			'test-server.c',
			wayland_client_protocol_h,
			wayland_server_protocol_h,
			tests_server_protocol_h,
			tests_client_protocol_h,
			tests_protocol_c,
		],
		'deps': [ wayland_client_dep, wayland_server_dep ],
		'runtime_deps': [ exec_fd_leak_checker ],
	},
	'server-test': {
		'extra_sources': [
			wayland_server_protocol_h,
			tests_server_protocol_h,
			tests_protocol_c,
		],
		'deps': [ wayland_server_dep ],
	},
	'headers-test': {
		'extra_sources': [
			wayland_client_protocol_h,
			wayland_server_protocol_h,
			'headers-protocol-test.c',
			wayland_client_protocol_core_h,
			wayland_server_protocol_core_h,
			'headers-protocol-core-test.c',
		],
		'deps': [ poor_loop_dep ],
	},
	'enum-validator-test': {
		'deps': [ poor_loop_dep ],
	},
}

# Tests that run io_uring run with each ring setup
ring_tests = [ 'connection-test', 'display-test', 'server-test' ]

if build_swapchain
	tests += {
		'swapchain-test': {
			'extra_sources': [
				'swapchain-server.c',
				wayland_client_protocol_h,
				wayland_server_protocol_h,
				linux_dmabuf_server_protocol_h,
			],
			'deps': [ wayland_client_dep, wayland_server_dep, wayland_swapchain_dep ],
		},
		'gbm-test': {
			'deps': [ wayland_swapchain_dep ],
		},
	}
	ring_tests += [ 'swapchain-test', 'gbm-test' ]
endif

foreach test_name, test_extras : tests
	test_extra_sources = test_extras.get('extra_sources', [])
	test_runtime_deps = test_extras.get('runtime_deps', [])
	test_sources = [ test_name + '.c' ] + test_extra_sources
	test_deps = [ test_runner_dep ] + test_extras.get('deps', [])
	bin = executable(test_name, test_sources, dependencies: test_deps)
	foreach ring : test_name in ring_tests ? [ 'default', 'coop', 'defer', 'sqpoll' ] : [ 'default' ]
		test(
			test_name in ring_tests ? test_name + '-' + ring : test_name,
			bin,
			depends: test_runtime_deps,
			env: [
				'TEST_SRC_DIR=@0@'.format(meson.current_source_dir()),
				'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()),
				'TEST_RING=' + ring,
			],
		)
	endforeach
endforeach
