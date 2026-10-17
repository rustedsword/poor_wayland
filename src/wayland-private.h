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

#ifndef POOR_WAYLAND_PRIVATE_H
#define POOR_WAYLAND_PRIVATE_H

#include <errno.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "poor_wayland_connection.h"
#include "poor_wayland_util.h"

#define POOR_WL_MAP_SERVER_SIDE 0
#define POOR_WL_MAP_CLIENT_SIDE 1
#define POOR_WL_SERVER_ID_START 0xff000000
#define POOR_WL_CLOSURE_MAX_ARGS 20

#define POOR_WL_DEBUG_COLOR_RESET "\e[0m"
#define POOR_WL_DEBUG_COLOR_RED "\e[31m"
#define POOR_WL_DEBUG_COLOR_GREEN "\e[32m"
#define POOR_WL_DEBUG_COLOR_YELLOW "\e[33m"
#define POOR_WL_DEBUG_COLOR_BLUE "\e[34m"
#define POOR_WL_DEBUG_COLOR_MAGENTA "\e[35m"
#define POOR_WL_DEBUG_COLOR_CYAN "\e[36m"

/**
 * Argument types used in signatures.
 */
enum poor_wl_arg_type {
	POOR_WL_ARG_INT = 'i',
	POOR_WL_ARG_UINT = 'u',
	POOR_WL_ARG_FIXED = 'f',
	POOR_WL_ARG_STRING = 's',
	POOR_WL_ARG_OBJECT = 'o',
	POOR_WL_ARG_NEW_ID = 'n',
	POOR_WL_ARG_ARRAY = 'a',
	POOR_WL_ARG_FD = 'h',
};

int poor_wl_interface_equal(const struct poor_wl_interface *iface1, const struct poor_wl_interface *iface2);

/* The map stores one bit of flags per entry. A zombie is a client side object that the client destroyed, its data is the interface. */
enum poor_wl_map_entry_flags {
	POOR_WL_MAP_ENTRY_ZOMBIE = (1 << 0),
};

typedef enum poor_wl_iterator_result (*poor_wl_iterator_func_t)(void *element, void *data, uint32_t flags);

void poor_wl_map_init(struct poor_wl_map *map, uint32_t side, const struct poor_wl_connection_memory *memory);
uint32_t poor_wl_map_insert_new(struct poor_wl_map *map, uint32_t flags, void *data);
int poor_wl_map_insert_at(struct poor_wl_map *map, uint32_t flags, uint32_t i, void *data);
int poor_wl_map_reserve_new(struct poor_wl_map *map, uint32_t i);
void poor_wl_map_remove(struct poor_wl_map *map, uint32_t i);
void *poor_wl_map_lookup(struct poor_wl_map *map, uint32_t i);
uint32_t poor_wl_map_lookup_flags(struct poor_wl_map *map, uint32_t i);
void poor_wl_map_for_each(struct poor_wl_map *map, poor_wl_iterator_func_t func, void *data);

void poor_wl_connection_init(struct poor_wl_connection *connection, const struct poor_wl_connection_memory *memory);
bool poor_wl_connection_can_recv(const struct poor_wl_connection *connection);
void poor_wl_connection_prep_recv(struct poor_wl_connection *connection, struct io_uring_sqe *sqe, int fd);
int poor_wl_connection_recv_done(struct poor_wl_connection *connection, struct poor_loop *loop, int res);
bool poor_wl_connection_has_output(const struct poor_wl_connection *connection);
void poor_wl_connection_prep_send(struct poor_wl_connection *connection, struct io_uring_sqe *sqe, int fd, int flags);
void poor_wl_connection_send_done(struct poor_wl_connection *connection, struct poor_loop *loop, int res);
void poor_wl_connection_close_fds(struct poor_wl_connection *connection, struct poor_loop *loop);
void poor_wl_connection_close_fds_in(struct poor_wl_connection *connection, struct poor_loop *loop, int max);
uint32_t poor_wl_connection_pending_input(const struct poor_wl_connection *connection);
void poor_wl_connection_copy_impl(const struct poor_wl_connection *connection, void *data, size_t size);
#define poor_wl_connection_copy(connection, arrm) poor_wl_connection_copy_impl(connection, auto_arr(arrm), ARRAY_SIZE_BYTES(arrm))
void poor_wl_connection_consume(struct poor_wl_connection *connection, size_t size);

/* 'log' is the log handler of the side of the connection, the client or the server */
struct poor_wl_closure {
	poor_wl_log_func_t log;
	int count;
	const struct poor_wl_message *message;
	uint32_t opcode;
	uint32_t sender_id;
	union poor_wl_argument args[POOR_WL_CLOSURE_MAX_ARGS];
};

/* Strings and arrays of a received closure point here */
struct poor_wl_closure_data {
	struct poor_wl_array arrays[POOR_WL_CLOSURE_MAX_ARGS];
	uint32_t words[POOR_WL_MAX_MESSAGE_SIZE / sizeof(uint32_t)];
};

struct argument_details {
	enum poor_wl_arg_type type;
	int nullable;
};

const char *get_next_argument(const char *signature, struct argument_details *details);
int arg_count_for_signature(const char *signature);
int poor_wl_message_count_fds(const struct poor_wl_message *message);
int poor_wl_message_get_since(const struct poor_wl_message *message);
void poor_wl_argument_from_va_list(const char *signature, union poor_wl_argument *args, int count, va_list ap);

int poor_wl_closure_marshal(struct poor_wl_closure *closure, poor_wl_log_func_t log, struct poor_wl_object *sender,
			    uint32_t opcode, const union poor_wl_argument *args, const struct poor_wl_message *message);
int poor_wl_closure_send(const struct poor_wl_closure *closure, struct poor_wl_connection *connection, struct poor_loop *loop);
void poor_wl_message_close_fds(const struct poor_wl_message *message, const union poor_wl_argument *args, struct poor_loop *loop);
int poor_wl_connection_demarshal(struct poor_wl_connection *connection, struct poor_loop *loop, poor_wl_log_func_t log,
				 uint32_t size, struct poor_wl_map *objects, const struct poor_wl_message *message,
				 struct poor_wl_closure *closure, struct poor_wl_closure_data *data);
bool poor_wl_object_is_zombie(struct poor_wl_map *map, uint32_t id);
int poor_wl_closure_lookup_objects(struct poor_wl_closure *closure, struct poor_wl_map *objects);

void poor_wl_closure_dispatch(struct poor_wl_closure *closure, poor_wl_dispatcher_func_t dispatcher, struct poor_wl_object *target, uint32_t opcode);
void poor_wl_closure_close_fds(struct poor_wl_closure *closure, struct poor_loop *loop);
bool poor_wl_check_env_token(const char *env, const char *token);
void poor_wl_closure_print(const struct poor_wl_closure *closure, const struct poor_wl_object *target, bool send, bool discarded,
		      uint32_t (*n_parse)(const union poor_wl_argument *arg), bool color);

void poor_wl_log(poor_wl_log_func_t handler, const char *fmt, ...) POOR_WL_PRINTF(2, 3);
[[noreturn]] void poor_wl_abort(const char *fmt, ...) POOR_WL_PRINTF(1, 2);

/* A ring that fails to submit can't run the connection anymore */
static inline struct io_uring_sqe *poor_wl_need_sqe(struct io_uring_sqe *sqe)
{
	if (!sqe)
		poor_wl_abort("io_uring: %s\n", strerror(errno));
	return sqe;
}

static inline void poor_wl_cancel_op(struct poor_loop *loop, struct poor_loop_op *op)
{
	if (op->pending)
		io_uring_prep_cancel(poor_wl_need_sqe(poor_loop_get_untracked_sqe_or_submit(loop)), op, 0);
}

#endif
