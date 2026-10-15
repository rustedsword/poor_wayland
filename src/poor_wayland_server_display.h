/* SPDX-License-Identifier: MIT */
#ifndef POOR_WAYLAND_SERVER_DISPLAY_H
#define POOR_WAYLAND_SERVER_DISPLAY_H

#include "poor_wayland_server_core.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * The definition of the server's display, for the code that allocates it. It is separate from poor_wayland_server_core.h,
 * which only declares it, since the client's display has the same name.
 */
poor_loop_group_declare(poor_wl_display_io);

/* Every socket and client holds the display group until it is done */
struct poor_wl_display {
	poor_wl_display_io io;
	struct poor_loop *loop;
	struct poor_loop_timer timer;
	poor_wl_display_done_fn *done;
	poor_wl_display_alloc_fn *alloc;
	poor_wl_display_free_fn *free;
	uint32_t next_global_name;
	uint32_t serial;
	uint32_t generation;
	uint64_t ack_slots;
	struct poor_wl_list registry_resource_list;
	struct poor_wl_list global_list;
	struct poor_wl_list socket_list;
	struct poor_wl_list client_list;
	struct poor_wl_list protocol_loggers;
	struct poor_wl_priv_signal destroy_signal;
	struct poor_wl_priv_signal create_client_signal;
	const uint32_t *shm_formats;
	size_t shm_formats_count;
	poor_wl_display_global_filter_func_t global_filter;
	void *global_filter_data;
	bool destroyed;
};

#ifdef  __cplusplus
}
#endif

#endif
