/* SPDX-License-Identifier: MIT */
#ifndef WAYLAND_SERVER_DISPLAY_H
#define WAYLAND_SERVER_DISPLAY_H

#include "wayland-server-core.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * The definition of the server's display, for the code that allocates it. It is separate from wayland-server-core.h,
 * which only declares it, since the client's display has the same name.
 */
poor_loop_group_declare(wl_display_io);

/* Every socket and client holds the display group until it is done */
struct wl_display {
	wl_display_io io;
	struct poor_loop *loop;
	struct poor_loop_timer timer;
	wl_display_done_fn *done;
	wl_display_alloc_fn *alloc;
	wl_display_free_fn *free;
	uint32_t next_global_name;
	uint32_t serial;
	uint32_t generation;
	uint64_t ack_slots;
	struct wl_list registry_resource_list;
	struct wl_list global_list;
	struct wl_list socket_list;
	struct wl_list client_list;
	struct wl_list protocol_loggers;
	struct wl_priv_signal destroy_signal;
	struct wl_priv_signal create_client_signal;
	const uint32_t *shm_formats;
	size_t shm_formats_count;
	wl_display_global_filter_func_t global_filter;
	void *global_filter_data;
	bool destroyed;
};

#ifdef  __cplusplus
}
#endif

#endif
