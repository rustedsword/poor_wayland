/*
 * Copyright © 2008-2012 Kristian Høgsberg
 * Copyright © 2010-2012 Intel Corporation
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

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "poor_wayland_client_display.h"
#include "poor_wayland_client.h"
#include "wayland-private.h"

enum poor_wl_proxy_flag {
	POOR_WL_PROXY_FLAG_ID_DELETED = (1 << 0),
};

poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, socket_op, display_on_socket);
poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, connect_op, display_on_connect);
poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, recv_op, display_on_recv);
poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, send_op, display_on_send);
poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, timer, display_on_timer);

static bool debug_client;
static bool debug_color;

static struct poor_wl_display *display_of(poor_wl_display_io *io)
{
	return container_of(io, struct poor_wl_display, io);
}

static struct io_uring_sqe *display_sqe(struct poor_wl_display *display, struct poor_loop_op *op)
{
	return poor_wl_need_sqe(poor_loop_group_get_sqe_or_submit(display->loop, &display->io, op));
}

static void display_release(struct poor_wl_display *display)
{
	if (poor_loop_group_release(&display->io))
		poor_loop_group_call_done(poor_wl_display_io, display->loop, &display->io);
}

/* The display timer sends the queued requests or reports an error when the loop iteration ends */
static void display_schedule(struct poor_wl_display *display)
{
	if (!display->closing && !poor_loop_timer_armed(&display->timer))
		poor_loop_group_timer_arm(display->loop, &display->io, &display->timer, 0);
}

/* For local errors: no memory, server hung up */
static void display_fatal_error(struct poor_wl_display *display, int error)
{
	if (display->last_error)
		return;
	display->last_error = error ? error : EFAULT;
	display_schedule(display);
}

/* For error events, which tell the object and its interface */
static void display_protocol_error(struct poor_wl_display *display, uint32_t code, uint32_t id, const struct poor_wl_interface *intf)
{
	int err = EPROTO;

	if (display->last_error)
		return;
	if (intf && poor_wl_interface_equal(intf, &poor_wl_display_interface)) {
		switch (code) {
		case POOR_WL_DISPLAY_ERROR_INVALID_OBJECT:
		case POOR_WL_DISPLAY_ERROR_INVALID_METHOD:
			err = EINVAL;
			break;
		case POOR_WL_DISPLAY_ERROR_NO_MEMORY:
			err = ENOMEM;
			break;
		case POOR_WL_DISPLAY_ERROR_IMPLEMENTATION:
			err = EPROTO;
			break;
		default:
			err = EFAULT;
		}
	}
	display->protocol_error.code = code;
	display->protocol_error.id = id;
	display->protocol_error.interface = intf;
	display_fatal_error(display, err);
}

static void proxy_init(struct poor_wl_proxy *proxy, struct poor_wl_display *display, const struct poor_wl_interface *interface,
		       uint32_t version)
{
	proxy->object.interface = interface;
	proxy->object.implementation = nullptr;
	proxy->display = display;
	proxy->user_data = nullptr;
	proxy->dispatcher = nullptr;
	proxy->tag = nullptr;
	proxy->version = version;
	proxy->flags = 0;
}

/* The memory of the new proxy comes in the new_id argument */
static struct poor_wl_proxy *create_outgoing_proxy(struct poor_wl_proxy *factory, const struct poor_wl_message *message,
					      union poor_wl_argument *args, const struct poor_wl_interface *interface, uint32_t version)
{
	const char *signature = message->signature;
	struct argument_details arg;
	int count = arg_count_for_signature(signature);

	for (int i = 0; i < count; i++) {
		struct poor_wl_proxy *proxy;

		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_NEW_ID)
			continue;
		proxy = (struct poor_wl_proxy *)args[i].o;
		assert(proxy);
		proxy_init(proxy, factory->display, interface, version);
		proxy->object.id = poor_wl_map_insert_new(&factory->display->objects, 0, proxy);
		return proxy->object.id ? proxy : nullptr;
	}
	return nullptr;
}

/*
 * The proxy becomes a zombie until the server deletes its id, so that events sent to it before the server knew are
 * discarded with their fds.
 */
POOR_WL_EXPORT void poor_wl_proxy_destroy(struct poor_wl_proxy *proxy)
{
	struct poor_wl_display *display = proxy->display;

	if (proxy->flags & POOR_WL_PROXY_FLAG_ID_DELETED)
		poor_wl_map_remove(&display->objects, proxy->object.id);
	else
		poor_wl_map_insert_at(&display->objects, POOR_WL_MAP_ENTRY_ZOMBIE, proxy->object.id, (void *)proxy->object.interface);
}

/*
 * Set the proxy's listener and user data. \c implementation is a vector of function pointers, \c implementation[n]
 * handles the event with opcode \c n. Fails if a listener is set already.
 */
POOR_WL_EXPORT int poor_wl_proxy_add_listener(struct poor_wl_proxy *proxy, void (**implementation)(void), void *data)
{
	if (proxy->object.implementation || proxy->dispatcher) {
		poor_wl_log("proxy %p already has listener\n", (void *)proxy);
		return -1;
	}
	proxy->object.implementation = implementation;
	proxy->user_data = data;
	return 0;
}

POOR_WL_EXPORT const void *poor_wl_proxy_get_listener(struct poor_wl_proxy *proxy)
{
	return proxy->object.implementation;
}

/* Set a dispatcher with its implementation, for language bindings. Fails if a listener is set already. */
POOR_WL_EXPORT int poor_wl_proxy_add_dispatcher(struct poor_wl_proxy *proxy, poor_wl_dispatcher_func_t dispatcher, const void *implementation,
				      void *data)
{
	if (proxy->object.implementation || proxy->dispatcher) {
		poor_wl_log("proxy %p already has listener\n", (void *)proxy);
		return -1;
	}
	proxy->object.implementation = implementation;
	proxy->dispatcher = dispatcher;
	proxy->user_data = data;
	return 0;
}

POOR_WL_EXPORT struct poor_wl_proxy *poor_wl_proxy_marshal_flags(struct poor_wl_proxy *proxy, uint32_t opcode, const struct poor_wl_interface *interface,
						  uint32_t version, uint32_t flags, ...)
{
	union poor_wl_argument args[POOR_WL_CLOSURE_MAX_ARGS];
	va_list ap;

	va_start(ap, flags);
	poor_wl_argument_from_va_list(proxy->object.interface->methods[opcode].signature, args, POOR_WL_CLOSURE_MAX_ARGS, ap);
	va_end(ap);
	return poor_wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
}

/*
 * Queue a request. For a new_id argument the argument is the memory of the new proxy, which is returned initialized
 * with 'interface' and 'version', or NULL if the client has too many objects. The connection takes the fds, which it
 * closes once they are sent or if the request fails. POOR_WL_MARSHAL_FLAG_DESTROY destroys 'proxy' afterwards.
 */
POOR_WL_EXPORT struct poor_wl_proxy *poor_wl_proxy_marshal_array_flags(struct poor_wl_proxy *proxy, uint32_t opcode,
							const struct poor_wl_interface *interface, uint32_t version, uint32_t flags,
							union poor_wl_argument *args)
{
	const struct poor_wl_message *message = &proxy->object.interface->methods[opcode];
	struct poor_wl_display *display = proxy->display;
	struct poor_wl_proxy *new_proxy = nullptr;
	struct poor_wl_closure closure;

	if (interface) {
		new_proxy = create_outgoing_proxy(proxy, message, args, interface, version);
		if (!new_proxy)
			goto unsent;
	}
	if (display->last_error)
		goto unsent;
	if (poor_wl_closure_marshal(&closure, &proxy->object, opcode, args, message) < 0) {
		poor_wl_log("Error marshalling request for %s.%s: %s\n", proxy->object.interface->name, message->name, strerror(errno));
		display_fatal_error(display, errno);
		goto unsent;
	}
	if (debug_client)
		poor_wl_closure_print(&closure, &proxy->object, true, false, nullptr, debug_color);
	if (poor_wl_closure_send(&closure, &display->connection, display->loop) < 0) {
		poor_wl_log("Error sending request for %s.%s: %s\n", proxy->object.interface->name, message->name, strerror(errno));
		display_fatal_error(display, errno);
		goto out;
	}
	display_schedule(display);
	goto out;
unsent:
	poor_wl_message_close_fds(message, args, display->loop);
out:
	if (flags & POOR_WL_MARSHAL_FLAG_DESTROY)
		poor_wl_proxy_destroy(proxy);
	return new_proxy;
}

static void display_handle_error(void *, struct poor_wl_display *display, void *object, uint32_t code, const char *message)
{
	struct poor_wl_proxy *proxy = object;

	if (proxy) {
		poor_wl_log("%s#%u: error %d: %s\n", proxy->object.interface->name, proxy->object.id, code, message);
		display_protocol_error(display, code, proxy->object.id, proxy->object.interface);
	} else {
		poor_wl_log("[destroyed object]: error %d: %s\n", code, message);
		display_protocol_error(display, code, 0, nullptr);
	}
}

static void display_handle_delete_id(void *, struct poor_wl_display *display, uint32_t id)
{
	struct poor_wl_proxy *proxy = poor_wl_map_lookup(&display->objects, id);

	if (poor_wl_object_is_zombie(&display->objects, id))
		poor_wl_map_remove(&display->objects, id);
	else if (proxy)
		proxy->flags |= POOR_WL_PROXY_FLAG_ID_DELETED;
	else
		poor_wl_log("error: received delete_id for unknown id (%u)\n", id);
}

static const struct poor_wl_display_listener display_listener = {
	display_handle_error,
	display_handle_delete_id,
};

static int display_set_address(struct poor_wl_display *display, const char *name)
{
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	bool path_is_absolute;
	int name_size;

	if (!name)
		name = getenv("WAYLAND_DISPLAY");
	if (!name)
		name = "wayland-0";
	path_is_absolute = name[0] == '/';
	if ((!runtime_dir || runtime_dir[0] != '/') && !path_is_absolute) {
		poor_wl_log("error: XDG_RUNTIME_DIR is invalid or not set in the environment.\n");
		errno = ENOENT;
		return -1;
	}
	display->addr.sun_family = AF_LOCAL;
	if (path_is_absolute)
		name_size = snprintf_array(display->addr.sun_path, "%s", name) + 1;
	else
		name_size = snprintf_array(display->addr.sun_path, "%s/%s", runtime_dir, name) + 1;
	assert(name_size > 0);
	if (name_size > (int)ARRAY_SIZE(display->addr.sun_path)) {
		if (!path_is_absolute)
			poor_wl_log("error: socket path \"%s/%s\" plus null terminator exceeds %zu bytes\n", runtime_dir, name,
			       ARRAY_SIZE(display->addr.sun_path));
		else
			poor_wl_log("error: socket path \"%s\" plus null terminator exceeds %zu bytes\n", name,
			       ARRAY_SIZE(display->addr.sun_path));
		errno = ENAMETOOLONG;
		return -1;
	}
	display->addr_len = offsetof(struct sockaddr_un, sun_path) + name_size;
	return 0;
}

/*
 * The display proxy has version 0, so proxies created from it with unversioned API inherit it, and code that checks
 * proxy versions knows that it can't rely on them.
 */
static void display_init(struct poor_wl_display *display, struct poor_loop *loop, const struct poor_wl_connection_memory *memory,
			 poor_wl_display_error_fn *error, poor_wl_display_done_fn *done)
{
	const char *debug = getenv("WAYLAND_DEBUG"), *no_color = getenv("NO_COLOR"), *force_color = getenv("FORCE_COLOR");

	if (debug && (poor_wl_check_env_token(debug, "client") || poor_wl_check_env_token(debug, "1"))) {
		debug_client = true;
		if (isatty(STDERR_FILENO))
			debug_color = true;
	}
	if (force_color && force_color[0] != '\0')
		debug_color = true;
	if (no_color && no_color[0] != '\0')
		debug_color = false;

	assert(memory->client_objects_size >= 2);
	poor_loop_group_init(&display->io);
	poor_loop_group_acquire(&display->io);
	poor_loop_op_init(&display->socket_op, display_on_socket);
	poor_loop_op_init(&display->connect_op, display_on_connect);
	poor_loop_op_init(&display->recv_op, display_on_recv);
	poor_loop_op_init(&display->send_op, display_on_send);
	poor_loop_timer_init(&display->timer, display_on_timer);
	poor_wl_connection_init(&display->connection, memory);
	poor_wl_map_init(&display->objects, POOR_WL_MAP_CLIENT_SIDE, memory);
	poor_wl_map_insert_at(&display->objects, 0, 0, nullptr);
	proxy_init(&display->proxy, display, &poor_wl_display_interface, 0);
	display->proxy.object.id = poor_wl_map_insert_new(&display->objects, 0, &display->proxy);
	display->proxy.object.implementation = (void (**)(void))&display_listener;
	display->proxy.user_data = display;
	display->loop = loop;
	display->error = error;
	display->done = done;
	display->new_proxy = nullptr;
	display->fd = -1;
	display->last_error = 0;
	display->protocol_error.code = 0;
	display->protocol_error.interface = nullptr;
	display->protocol_error.id = 0;
	display->connected = display->hangup = display->error_reported = display->closing = false;
}

static void display_recv(struct poor_wl_display *display)
{
	if (!poor_wl_connection_can_recv(&display->connection)) {
		display_fatal_error(display, EOVERFLOW);
		return;
	}
	poor_wl_connection_prep_recv(&display->connection, display_sqe(display, &display->recv_op), display->fd);
}

static void display_send(struct poor_wl_display *display)
{
	if (display->connected && !display->hangup && !display->closing && !display->send_op.pending &&
	    poor_wl_connection_has_output(&display->connection))
		poor_wl_connection_prep_send(&display->connection, display_sqe(display, &display->send_op), display->fd, 0);
}

static void display_connected(struct poor_wl_display *display)
{
	display->connected = true;
	display_recv(display);
	display_send(display);
}

/* A send to a server that hung up isn't fatal yet, the protocol error that made it hang up may still be read */
POOR_LOOP_GROUP_OP(display_on_send, loop, io, op, cqe)
{
	struct poor_wl_display *display = display_of(io);

	poor_wl_connection_send_done(&display->connection, loop, cqe->res);
	if (display->closing)
		return;
	if (cqe->res == -EPIPE || cqe->res == -ECONNRESET) {
		display->hangup = true;
		return;
	}
	if (cqe->res < 0) {
		display_fatal_error(display, -cqe->res);
		return;
	}
	display_send(display);
}

static int create_proxies(struct poor_wl_display *display, struct poor_wl_proxy *sender, struct poor_wl_closure *closure)
{
	const char *signature = closure->message->signature;
	struct argument_details arg;

	for (int i = 0; i < closure->count; i++) {
		const struct poor_wl_interface *interface = closure->message->types[i];
		struct poor_wl_proxy *proxy;

		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_NEW_ID)
			continue;
		proxy = display->new_proxy ? display->new_proxy(display, sender, interface) : nullptr;
		if (!proxy) {
			poor_wl_log("no proxy for new %s#%u\n", interface->name, closure->args[i].n);
			errno = ENOMEM;
			return -1;
		}
		proxy_init(proxy, display, interface, sender->version);
		proxy->object.id = closure->args[i].n;
		if (poor_wl_map_insert_at(&display->objects, 0, proxy->object.id, proxy) < 0)
			return -1;
		closure->args[i].o = &proxy->object;
	}
	return 0;
}

static uint32_t id_from_object(const union poor_wl_argument *arg)
{
	return arg->o ? arg->o->id : 0;
}

static void log_discarded(uint32_t id, uint32_t opcode, bool zombie, int fds, uint32_t size)
{
	struct timespec tp;

	clock_gettime(CLOCK_REALTIME, &tp);
	poor_wl_log("%s[%02u:%02u:%02u.%06u] %sdiscarded %s[%s]%s#%u%s.[event %u]%s(%d fd, %u byte)\n",
	       debug_color ? POOR_WL_DEBUG_COLOR_GREEN : "", (unsigned)((tp.tv_sec / 3600) % 24), (unsigned)((tp.tv_sec / 60) % 60),
	       (unsigned)(tp.tv_sec % 60), (unsigned)(tp.tv_nsec / 1000), debug_color ? POOR_WL_DEBUG_COLOR_RED : "",
	       debug_color ? POOR_WL_DEBUG_COLOR_BLUE : "", zombie ? "zombie" : "unknown", debug_color ? POOR_WL_DEBUG_COLOR_MAGENTA : "",
	       id, debug_color ? POOR_WL_DEBUG_COLOR_BLUE : "", opcode, debug_color ? POOR_WL_DEBUG_COLOR_RESET : "", fds, size);
}

/*
 * Returns the size of the dispatched message, 0 if it hasn't fully arrived, or -1 on error. A message longer than
 * POOR_WL_MAX_MESSAGE_SIZE could never be dispatched, so it fails the connection.
 */
static int display_dispatch_message(struct poor_wl_display *display, uint32_t len)
{
	struct poor_wl_connection *connection = &display->connection;
	const struct poor_wl_message *message;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	uint32_t p[2], id, opcode, size;
	struct poor_wl_proxy *proxy;

	poor_wl_connection_copy(connection, p);
	id = p[0];
	opcode = p[1] & 0xffff;
	size = p[1] >> 16;
	if (size > POOR_WL_MAX_MESSAGE_SIZE) {
		poor_wl_log("Message length %u exceeds limit %d\n", size, POOR_WL_MAX_MESSAGE_SIZE);
		errno = E2BIG;
		return -1;
	}
	if (len < size)
		return 0;

	proxy = poor_wl_map_lookup(&display->objects, id);
	if (!proxy || poor_wl_object_is_zombie(&display->objects, id)) {
		const struct poor_wl_interface *interface = (const void *)proxy;
		int fds = interface && opcode < (uint32_t)interface->event_count ? poor_wl_message_count_fds(&interface->events[opcode]) : 0;

		if (debug_client)
			log_discarded(id, opcode, interface != nullptr, fds, size);
		poor_wl_connection_close_fds_in(connection, display->loop, fds);
		poor_wl_connection_consume(connection, size);
		return size;
	}
	if (opcode >= (uint32_t)proxy->object.interface->event_count) {
		poor_wl_log("interface '%s' has no event %u\n", proxy->object.interface->name, opcode);
		errno = EPROTO;
		return -1;
	}
	message = &proxy->object.interface->events[opcode];
	if (poor_wl_connection_demarshal(connection, display->loop, size, &display->objects, message, &closure, &data) < 0)
		return -1;
	if (create_proxies(display, proxy, &closure) < 0 || poor_wl_closure_lookup_objects(&closure, &display->objects) < 0) {
		poor_wl_closure_close_fds(&closure, display->loop);
		return -1;
	}
	if (debug_client)
		poor_wl_closure_print(&closure, &proxy->object, false, !proxy->dispatcher && !proxy->object.implementation,
				 id_from_object, debug_color);
	if (proxy->dispatcher)
		poor_wl_closure_dispatch(&closure, proxy->dispatcher, &proxy->object, opcode);
	else if (proxy->object.implementation)
		poor_wl_closure_invoke(&closure, POOR_WL_CLOSURE_INVOKE_CLIENT, &proxy->object, opcode, proxy->user_data);
	else
		poor_wl_closure_close_fds(&closure, display->loop);
	return size;
}

/* Listeners can disconnect the display or fail it, which stops the dispatch */
static void display_dispatch(struct poor_wl_display *display)
{
	uint32_t len;
	int size;

	while (!display->closing && !display->last_error && (len = poor_wl_connection_pending_input(&display->connection)) >= 8) {
		size = display_dispatch_message(display, len);
		if (size < 0)
			display_fatal_error(display, errno);
		if (size <= 0)
			return;
	}
}

/* A server that hangs up after an error event is caught by the error, so EOF is only an error without one */
POOR_LOOP_GROUP_OP(display_on_recv, loop, io, op, cqe)
{
	struct poor_wl_display *display = display_of(io);

	if (display->closing)
		return;
	if (cqe->res <= 0) {
		display_fatal_error(display, cqe->res ? -cqe->res : EPIPE);
		return;
	}
	if (poor_wl_connection_recv_done(&display->connection, loop, cqe->res) < 0) {
		display_fatal_error(display, errno);
		return;
	}
	display_dispatch(display);
	if (!display->closing && !display->last_error)
		display_recv(display);
}

POOR_LOOP_GROUP_OP(display_on_socket, loop, io, op, cqe)
{
	struct poor_wl_display *display = display_of(io);

	if (cqe->res >= 0)
		display->fd = cqe->res;
	if (display->closing)
		return;
	if (cqe->res < 0) {
		display_fatal_error(display, -cqe->res);
		return;
	}
	io_uring_prep_connect(display_sqe(display, &display->connect_op), display->fd, (struct sockaddr *)&display->addr,
			      display->addr_len);
}

POOR_LOOP_GROUP_OP(display_on_connect, loop, io, op, cqe)
{
	struct poor_wl_display *display = display_of(io);

	if (display->closing)
		return;
	if (cqe->res < 0)
		display_fatal_error(display, -cqe->res);
	else
		display_connected(display);
}

POOR_LOOP_GROUP_TIMER(display_on_timer, loop, io, timer)
{
	struct poor_wl_display *display = display_of(io);

	if (display->closing)
		return;
	if (!display->last_error) {
		display_send(display);
	} else if (!display->error_reported) {
		display->error_reported = true;
		display->error(display);
	}
}

POOR_LOOP_GROUP_DONE(poor_wl_display_io, loop, io)
{
	struct poor_wl_display *display = display_of(io);

	poor_wl_connection_close_fds(&display->connection, loop);
	if (display->fd >= 0)
		poor_loop_close_fd_or_submit(loop, display->fd, IOSQE_ASYNC);
	display->done(display);
}

POOR_WL_EXPORT void poor_wl_display_connect_to_fd(struct poor_wl_display *display, struct poor_loop *loop,
					const struct poor_wl_connection_memory *memory, int fd, poor_wl_display_error_fn *error,
					poor_wl_display_done_fn *done)
{
	display_init(display, loop, memory, error, done);
	display->fd = fd;
	display_connected(display);
}

/* With WAYLAND_SOCKET this is not thread safe, since unsetenv() isn't */
POOR_WL_EXPORT int poor_wl_display_connect(struct poor_wl_display *display, struct poor_loop *loop, const struct poor_wl_connection_memory *memory,
				 const char *name, poor_wl_display_error_fn *error, poor_wl_display_done_fn *done)
{
	const char *connection = getenv("WAYLAND_SOCKET");
	int prev_errno, flags, fd;
	char *end;

	if (connection) {
		prev_errno = errno;
		errno = 0;
		fd = strtol(connection, &end, 10);
		if (errno != 0 || connection == end || *end != '\0') {
			errno = EINVAL;
			return -1;
		}
		errno = prev_errno;
		flags = fcntl(fd, F_GETFD);
		if (flags == -1 && errno == EBADF)
			return -1;
		if (flags != -1)
			fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		unsetenv("WAYLAND_SOCKET");
		poor_wl_display_connect_to_fd(display, loop, memory, fd, error, done);
		return 0;
	}
	if (display_set_address(display, name) < 0)
		return -1;
	display_init(display, loop, memory, error, done);
	io_uring_prep_socket(display_sqe(display, &display->socket_op), PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
	return 0;
}

/* The timer keeps 'done' out of this call */
POOR_WL_EXPORT void poor_wl_display_disconnect(struct poor_wl_display *display)
{
	struct poor_loop *loop = display->loop;

	assert(!display->closing);
	display->closing = true;
	poor_wl_cancel_op(loop, &display->socket_op);
	poor_wl_cancel_op(loop, &display->connect_op);
	poor_wl_cancel_op(loop, &display->recv_op);
	poor_wl_cancel_op(loop, &display->send_op);
	poor_loop_group_timer_arm(loop, &display->io, &display->timer, 0);
	display_release(display);
}

POOR_WL_EXPORT void poor_wl_display_set_new_proxy(struct poor_wl_display *display, poor_wl_display_new_proxy_fn *new_proxy)
{
	display->new_proxy = new_proxy;
}

POOR_WL_EXPORT int poor_wl_display_get_fd(struct poor_wl_display *display)
{
	return display->fd;
}

POOR_WL_EXPORT int poor_wl_display_flush(struct poor_wl_display *display)
{
	if (display->last_error) {
		errno = display->last_error;
		return -1;
	}
	display_send(display);
	return 0;
}

/* Errors are fatal: once this is not 0, the display can only be disconnected */
POOR_WL_EXPORT int poor_wl_display_get_error(struct poor_wl_display *display)
{
	return display->last_error;
}

/*
 * Retrieve the error code of a protocol error, and if they are not NULL, the interface and the id of the object it
 * happened at. The object may already be destroyed.
 */
POOR_WL_EXPORT uint32_t poor_wl_display_get_protocol_error(struct poor_wl_display *display, const struct poor_wl_interface **interface,
						 uint32_t *id)
{
	if (interface)
		*interface = display->protocol_error.interface;
	if (id)
		*id = display->protocol_error.id;
	return display->protocol_error.code;
}

POOR_WL_EXPORT void poor_wl_proxy_set_user_data(struct poor_wl_proxy *proxy, void *user_data)
{
	proxy->user_data = user_data;
}

POOR_WL_EXPORT void *poor_wl_proxy_get_user_data(struct poor_wl_proxy *proxy)
{
	return proxy->user_data;
}

/* The version is 0 if the proxy was created with unversioned API, and for the poor_wl_display */
POOR_WL_EXPORT uint32_t poor_wl_proxy_get_version(struct poor_wl_proxy *proxy)
{
	return proxy->version;
}

POOR_WL_EXPORT uint32_t poor_wl_proxy_get_id(struct poor_wl_proxy *proxy)
{
	return proxy->object.id;
}

/*
 * A toolkit or application can tag proxies to tell its own from the ones of other parts. A tag is the address of a
 * pointer to a constant string, so it can be logged.
 */
POOR_WL_EXPORT void poor_wl_proxy_set_tag(struct poor_wl_proxy *proxy, const char *const *tag)
{
	proxy->tag = tag;
}

POOR_WL_EXPORT const char *const *poor_wl_proxy_get_tag(struct poor_wl_proxy *proxy)
{
	return proxy->tag;
}

POOR_WL_EXPORT const char *poor_wl_proxy_get_class(struct poor_wl_proxy *proxy)
{
	return proxy->object.interface->name;
}

POOR_WL_EXPORT const struct poor_wl_interface *poor_wl_proxy_get_interface(struct poor_wl_proxy *proxy)
{
	return proxy->object.interface;
}

POOR_WL_EXPORT struct poor_wl_display *poor_wl_proxy_get_display(struct poor_wl_proxy *proxy)
{
	return proxy->display;
}

POOR_WL_EXPORT void poor_wl_log_set_handler_client(poor_wl_log_func_t handler)
{
	poor_wl_log_handler = handler;
}
