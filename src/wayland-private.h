/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2013 Jason Ekstrand
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WAYLAND_PRIVATE_H
#define WAYLAND_PRIVATE_H

#include <errno.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "wayland-connection.h"
#include "wayland-util.h"

#define WL_MAP_SERVER_SIDE 0
#define WL_MAP_CLIENT_SIDE 1
#define WL_SERVER_ID_START 0xff000000
#define WL_CLOSURE_MAX_ARGS 20

#define WL_DEBUG_COLOR_RESET "\e[0m"
#define WL_DEBUG_COLOR_RED "\e[31m"
#define WL_DEBUG_COLOR_GREEN "\e[32m"
#define WL_DEBUG_COLOR_YELLOW "\e[33m"
#define WL_DEBUG_COLOR_BLUE "\e[34m"
#define WL_DEBUG_COLOR_MAGENTA "\e[35m"
#define WL_DEBUG_COLOR_CYAN "\e[36m"

/**
 * Argument types used in signatures.
 */
enum wl_arg_type {
	WL_ARG_INT = 'i',
	WL_ARG_UINT = 'u',
	WL_ARG_FIXED = 'f',
	WL_ARG_STRING = 's',
	WL_ARG_OBJECT = 'o',
	WL_ARG_NEW_ID = 'n',
	WL_ARG_ARRAY = 'a',
	WL_ARG_FD = 'h',
};

int wl_interface_equal(const struct wl_interface *iface1, const struct wl_interface *iface2);

/* The map stores one bit of flags per entry. A zombie is a client side object that the client destroyed, its data is the interface. */
enum wl_map_entry_flags {
	WL_MAP_ENTRY_ZOMBIE = (1 << 0),
};

typedef enum wl_iterator_result (*wl_iterator_func_t)(void *element, void *data, uint32_t flags);

void wl_map_init(struct wl_map *map, uint32_t side, const struct wl_connection_memory *memory);
uint32_t wl_map_insert_new(struct wl_map *map, uint32_t flags, void *data);
int wl_map_insert_at(struct wl_map *map, uint32_t flags, uint32_t i, void *data);
int wl_map_reserve_new(struct wl_map *map, uint32_t i);
void wl_map_remove(struct wl_map *map, uint32_t i);
void *wl_map_lookup(struct wl_map *map, uint32_t i);
uint32_t wl_map_lookup_flags(struct wl_map *map, uint32_t i);
void wl_map_for_each(struct wl_map *map, wl_iterator_func_t func, void *data);

void wl_connection_init(struct wl_connection *connection, const struct wl_connection_memory *memory);
bool wl_connection_can_recv(const struct wl_connection *connection);
void wl_connection_prep_recv(struct wl_connection *connection, struct io_uring_sqe *sqe, int fd);
int wl_connection_recv_done(struct wl_connection *connection, struct poor_loop *loop, int res);
bool wl_connection_has_output(const struct wl_connection *connection);
void wl_connection_prep_send(struct wl_connection *connection, struct io_uring_sqe *sqe, int fd, int flags);
void wl_connection_send_done(struct wl_connection *connection, struct poor_loop *loop, int res);
void wl_connection_close_fds(struct wl_connection *connection, struct poor_loop *loop);
void wl_connection_close_fds_in(struct wl_connection *connection, struct poor_loop *loop, int max);
uint32_t wl_connection_pending_input(const struct wl_connection *connection);
void wl_connection_copy(const struct wl_connection *connection, void *data, size_t size);
void wl_connection_consume(struct wl_connection *connection, size_t size);

struct wl_closure {
	int count;
	const struct wl_message *message;
	uint32_t opcode;
	uint32_t sender_id;
	union wl_argument args[WL_CLOSURE_MAX_ARGS];
};

/* Strings and arrays of a received closure point here */
struct wl_closure_data {
	struct wl_array arrays[WL_CLOSURE_MAX_ARGS];
	uint32_t words[WL_MAX_MESSAGE_SIZE / sizeof(uint32_t)];
};

struct argument_details {
	enum wl_arg_type type;
	int nullable;
};

const char *get_next_argument(const char *signature, struct argument_details *details);
int arg_count_for_signature(const char *signature);
int wl_message_count_fds(const struct wl_message *message);
int wl_message_get_since(const struct wl_message *message);
void wl_argument_from_va_list(const char *signature, union wl_argument *args, int count, va_list ap);

int wl_closure_marshal(struct wl_closure *closure, struct wl_object *sender, uint32_t opcode, const union wl_argument *args,
		       const struct wl_message *message);
int wl_closure_send(const struct wl_closure *closure, struct wl_connection *connection, struct poor_loop *loop);
int wl_connection_demarshal(struct wl_connection *connection, struct poor_loop *loop, uint32_t size, struct wl_map *objects,
			    const struct wl_message *message, struct wl_closure *closure, struct wl_closure_data *data);
bool wl_object_is_zombie(struct wl_map *map, uint32_t id);
int wl_closure_lookup_objects(struct wl_closure *closure, struct wl_map *objects);

enum wl_closure_invoke_flag {
	WL_CLOSURE_INVOKE_CLIENT = (1 << 0),
	WL_CLOSURE_INVOKE_SERVER = (1 << 1),
};

void wl_closure_invoke(struct wl_closure *closure, uint32_t flags, struct wl_object *target, uint32_t opcode, void *data);
void wl_closure_dispatch(struct wl_closure *closure, wl_dispatcher_func_t dispatcher, struct wl_object *target, uint32_t opcode);
void wl_closure_close_fds(struct wl_closure *closure, struct poor_loop *loop);
bool wl_check_env_token(const char *env, const char *token);
void wl_closure_print(const struct wl_closure *closure, const struct wl_object *target, bool send, bool discarded,
		      uint32_t (*n_parse)(const union wl_argument *arg), bool color);

extern wl_log_func_t wl_log_handler;

void wl_log(const char *fmt, ...) WL_PRINTF(1, 2);
[[noreturn]] void wl_abort(const char *fmt, ...) WL_PRINTF(1, 2);

/* A ring that fails to submit can't run the connection anymore */
static inline struct io_uring_sqe *wl_need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe)
		wl_abort("io_uring: %s\n", strerror(errno));
	return sqe;
}

static inline void wl_cancel_op(struct poor_loop *loop, struct poor_loop_op *op)
{
	if (op->pending)
		io_uring_prep_cancel(wl_need_sqe(poor_loop_get_untracked_sqe_or_submit(loop)), op, 0);
}

#endif
