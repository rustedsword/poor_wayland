xdg_shell_xml = files('../refs/wayland-protocols/stable/xdg-shell/xdg-shell.xml')

xdg_shell_client_protocol_h = custom_target(
	'xdg-shell client header',
	command: [ wayland_scanner_for_build, '-s', 'client-header', '@INPUT@', '@OUTPUT@' ],
	input: xdg_shell_xml,
	output: 'xdg-shell-client-protocol.h',
)

xdg_shell_protocol_c = custom_target(
	'xdg-shell protocol source',
	command: [ wayland_scanner_for_build, '-s', 'private-code', '@INPUT@', '@OUTPUT@' ],
	input: xdg_shell_xml,
	output: 'xdg-shell-protocol.c',
)

executable(
	'simple-shm',
	'simple-shm.c',
	xdg_shell_client_protocol_h,
	xdg_shell_protocol_c,
	dependencies: wayland_client_dep,
)
