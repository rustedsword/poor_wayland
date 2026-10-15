/* SPDX-License-Identifier: MIT */
#ifndef WAYLAND_CONNECTION_H
#define WAYLAND_CONNECTION_H

#include <poor_array.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "wayland-util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* File descriptors that go with one sendmsg() or recvmsg() */
constexpr unsigned WL_CONNECTION_MAX_FDS = 28;
/* Capacity of the queues of incoming and outgoing file descriptors */
constexpr unsigned WL_CONNECTION_FDS = 256;

union wl_map_entry {
	uintptr_t next;
	void *data;
};

struct wl_map {
	union wl_map_entry *client_entries, *server_entries;
	uint32_t client_size, server_size;
	uint32_t client_count, server_count;
	uint32_t side, free_list;
};

struct wl_ring_buffer {
	char *data;
	uint32_t size, head, tail;
};

struct wl_fd_ring {
	uint32_t head, tail;
	int fds[WL_CONNECTION_FDS];
};

/* The kernel uses the message headers, vectors and control buffers of a request in flight */
struct wl_connection {
	struct wl_ring_buffer in, out;
	struct wl_fd_ring fds_in, fds_out;
	struct msghdr in_msg, out_msg;
	struct iovec in_iov[2], out_iov[2];
	uint32_t out_fds;
	bool want_flush;
	alignas(struct cmsghdr) char in_cmsg[CMSG_SPACE(WL_CONNECTION_MAX_FDS * sizeof(int))];
	alignas(struct cmsghdr) char out_cmsg[CMSG_SPACE(WL_CONNECTION_MAX_FDS * sizeof(int))];
};

/*
 * Memory of a connection, owned by the caller until the connection is done.
 *
 * 'in' and 'out' hold incoming and outgoing messages, their sizes are powers of two and at least
 * WL_MAX_MESSAGE_SIZE. 'client_objects' and 'server_objects' map the ids allocated by the client and by
 * the server, which limits how many objects of each kind can exist at once.
 */
struct wl_connection_memory {
	char *in, *out;
	union wl_map_entry *client_objects, *server_objects;
	uint32_t in_size, out_size, client_objects_size, server_objects_size;
};

#define WL_CONNECTION_MEMORY(in_, out_, client_objects_, server_objects_) \
	((struct wl_connection_memory){ \
		.in = auto_arr(in_), \
		.out = auto_arr(out_), \
		.client_objects = auto_arr(client_objects_), \
		.server_objects = auto_arr(server_objects_), \
		.in_size = ARRAY_SIZE(in_), \
		.out_size = ARRAY_SIZE(out_), \
		.client_objects_size = ARRAY_SIZE(client_objects_), \
		.server_objects_size = ARRAY_SIZE(server_objects_), \
	})

#ifdef __cplusplus
}
#endif

#endif
