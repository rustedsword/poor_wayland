/* SPDX-License-Identifier: MIT */
#ifndef POOR_WAYLAND_CLIENT_DISPLAY_H
#define POOR_WAYLAND_CLIENT_DISPLAY_H

#include <sys/un.h>

#include "poor_wayland_client_core.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * The definition of the client's display, for the code that allocates it. It is separate from poor_wayland_client_core.h,
 * which only declares it, since the server's display has the same name.
 */
poor_loop_group_declare(poor_wl_display_io);

struct poor_wl_display {
	struct poor_wl_proxy proxy;
	poor_wl_display_io io;
	struct poor_wl_connection connection;
	struct poor_loop *loop;
	struct poor_loop_op socket_op;
	struct poor_loop_op connect_op;
	struct poor_loop_op recv_op;
	struct poor_loop_op send_op;
	struct poor_loop_timer timer;
	struct poor_wl_map objects;
	poor_wl_display_error_fn *error;
	poor_wl_display_done_fn *done;
	poor_wl_display_new_proxy_fn *new_proxy;
	struct sockaddr_un addr;
	socklen_t addr_len;
	int fd;
	int last_error;
	struct {
		uint32_t code;
		const struct poor_wl_interface *interface;
		uint32_t id;
	} protocol_error;
	bool connected;
	bool hangup;
	bool error_reported;
	bool closing;
};

#ifdef  __cplusplus
}
#endif

#endif
