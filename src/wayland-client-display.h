/* SPDX-License-Identifier: MIT */
#ifndef WAYLAND_CLIENT_DISPLAY_H
#define WAYLAND_CLIENT_DISPLAY_H

#include <sys/un.h>

#include "wayland-client-core.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * The definition of the client's display, for the code that allocates it. It is separate from wayland-client-core.h,
 * which only declares it, since the server's display has the same name.
 */
poor_loop_group_declare(wl_display_io);

struct wl_display {
	struct wl_proxy proxy;
	wl_display_io io;
	struct wl_connection connection;
	struct poor_loop *loop;
	struct poor_loop_op socket_op;
	struct poor_loop_op connect_op;
	struct poor_loop_op recv_op;
	struct poor_loop_op send_op;
	struct poor_loop_timer timer;
	struct wl_map objects;
	wl_display_error_fn *error;
	wl_display_done_fn *done;
	wl_display_new_proxy_fn *new_proxy;
	struct sockaddr_un addr;
	socklen_t addr_len;
	int fd;
	int last_error;
	struct {
		uint32_t code;
		const struct wl_interface *interface;
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
