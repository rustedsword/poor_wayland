/*
 * Copyright © 2008 Kristian Høgsberg
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
#include <inttypes.h>
#include <poor_loop_uring_array.h>
#include <poor_stdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wayland-private.h"
#include "poor_wayland_server_display.h"
#include "wayland-server-private.h"
#include "poor_wayland_server.h"

#define LOCK_SUFFIX ".lock"

constexpr int MAX_DISPLAYNO = 32;
constexpr uint64_t ACCEPT_RETRY_NS = 100'000'000;

static_assert(sizeof(((struct poor_wl_client *)nullptr)->cred) == sizeof(struct ucred));

/* A registry remembers when it was created, which tells the globals it saw, and the removals it acknowledged */
struct registry {
	struct poor_wl_registry_resource resource;
	uint32_t generation;
	uint64_t acked;
};

poor_loop_group_attach(poor_wl_display_io, struct poor_wl_display, io, timer, display_on_timer);
poor_loop_group_attach(poor_wl_client_io, struct poor_wl_client, io, recv_op, client_on_recv);
poor_loop_group_attach(poor_wl_client_io, struct poor_wl_client, io, send_op, client_on_send);
poor_loop_group_attach(poor_wl_client_io, struct poor_wl_client, io, cred_op, client_on_cred);
poor_loop_group_attach(poor_wl_client_io, struct poor_wl_client, io, timer, client_on_timer);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, lock_op, socket_on_lock);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, stat_op, socket_on_stat);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, unlink_op, socket_on_unlink);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, socket_op, socket_on_socket);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, bind_op, socket_on_bind);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, listen_op, socket_on_listen);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, accept_op, socket_on_accept);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, cleanup_op, socket_on_cleanup);
poor_loop_group_attach(poor_wl_socket_io, struct poor_wl_socket, io, timer, socket_on_timer);

static bool debug_server;
static bool debug_color;

static struct poor_wl_client *client_of(poor_wl_client_io *io)
{
	return container_of(io, struct poor_wl_client, io);
}

static struct poor_wl_socket *socket_of(poor_wl_socket_io *io)
{
	return container_of(io, struct poor_wl_socket, io);
}

static void display_release(struct poor_wl_display *display)
{
	if (poor_loop_group_release(&display->io))
		poor_loop_group_call_done(poor_wl_display_io, display->loop, &display->io);
}

static void client_release(struct poor_wl_client *client)
{
	if (poor_loop_group_release(&client->io))
		poor_loop_group_call_done(poor_wl_client_io, client->display->loop, &client->io);
}

static void socket_release(struct poor_wl_socket *socket)
{
	if (poor_loop_group_release(&socket->io))
		poor_loop_group_call_done(poor_wl_socket_io, socket->display->loop, &socket->io);
}

static struct io_uring_sqe *client_sqe(struct poor_wl_client *client, struct poor_loop_op *op)
{
	return poor_wl_need_sqe(poor_loop_group_get_sqe_or_submit(client->display->loop, &client->io, op));
}

static struct io_uring_sqe *socket_sqe(struct poor_wl_socket *socket, struct poor_loop_op *op)
{
	return poor_wl_need_sqe(poor_loop_group_get_sqe_or_submit(socket->display->loop, &socket->io, op));
}

/* The client timer flushes or destroys the client when the loop iteration ends */
static void client_schedule(struct poor_wl_client *client)
{
	if (!client->destroyed && !poor_loop_timer_armed(&client->timer))
		poor_loop_group_timer_arm(client->display->loop, &client->io, &client->timer, 0);
}

static void client_fail(struct poor_wl_client *client)
{
	client->error = true;
	client_schedule(client);
}

static void log_closure(struct poor_wl_resource *resource, struct poor_wl_closure *closure, bool send)
{
	struct poor_wl_display *display = resource->client->display;
	struct poor_wl_protocol_logger *protocol_logger;
	struct poor_wl_protocol_logger_message message;

	if (debug_server)
		poor_wl_closure_print(closure, &resource->object, send, false, nullptr, debug_color);
	if (poor_wl_list_empty(&display->protocol_loggers))
		return;
	message.resource = resource;
	message.message_opcode = closure->opcode;
	message.message = closure->message;
	message.arguments_count = closure->count;
	message.arguments = closure->args;
	poor_wl_list_for_each(protocol_logger, &display->protocol_loggers, link)
		protocol_logger->func(protocol_logger->user_data, send ? POOR_WL_PROTOCOL_LOGGER_EVENT : POOR_WL_PROTOCOL_LOGGER_REQUEST,
				      &message);
}

static bool verify_objects(struct poor_wl_resource *resource, uint32_t opcode, union poor_wl_argument *args)
{
	struct poor_wl_object *object = &resource->object;
	const char *signature = object->interface->events[opcode].signature;
	struct argument_details arg;
	struct poor_wl_resource *res;

	foreach_array_index(array_ptr(args, arg_count_for_signature(signature)), i) {
		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_NEW_ID && arg.type != POOR_WL_ARG_OBJECT)
			continue;
		res = (struct poor_wl_resource *)args[i].o;
		if (res && res->client != resource->client) {
			poor_wl_log(poor_wl_server_log_handler,
				    "compositor bug: The compositor tried to use an object from one client in a '%s.%s' for a "
				    "different client.\n", object->interface->name, object->interface->events[opcode].name);
			return false;
		}
	}
	return true;
}

static void handle_array(struct poor_wl_resource *resource, uint32_t opcode, union poor_wl_argument *args, bool post)
{
	struct poor_wl_client *client = resource->client;
	struct poor_wl_object *object = &resource->object;
	struct poor_wl_closure closure;

	if (client->error || client->destroyed) {
		poor_wl_message_close_fds(&object->interface->events[opcode], args, client->display->loop);
		return;
	}
	if (!verify_objects(resource, opcode, args) ||
	    poor_wl_closure_marshal(&closure, poor_wl_server_log_handler, object, opcode, args,
				    &object->interface->events[opcode]) < 0) {
		poor_wl_message_close_fds(&object->interface->events[opcode], args, client->display->loop);
		client_fail(client);
		return;
	}
	log_closure(resource, &closure, true);
	if (poor_wl_closure_send(&closure, &client->connection, client->display->loop) < 0) {
		client_fail(client);
		return;
	}
	if (post) {
		client->connection.want_flush = true;
		client_schedule(client);
	}
}

POOR_WL_EXPORT void poor_wl_resource_post_event_array(struct poor_wl_resource *resource, uint32_t opcode, union poor_wl_argument *args)
{
	handle_array(resource, opcode, args, true);
}

POOR_WL_EXPORT void poor_wl_resource_post_event(struct poor_wl_resource *resource, uint32_t opcode, ...)
{
	union poor_wl_argument args[POOR_WL_CLOSURE_MAX_ARGS];
	va_list ap;

	va_start(ap, opcode);
	poor_wl_argument_from_va_list(resource->object.interface->events[opcode].signature, args, POOR_WL_CLOSURE_MAX_ARGS, ap);
	va_end(ap);
	poor_wl_resource_post_event_array(resource, opcode, args);
}

POOR_WL_EXPORT void poor_wl_resource_queue_event_array(struct poor_wl_resource *resource, uint32_t opcode, union poor_wl_argument *args)
{
	handle_array(resource, opcode, args, false);
}

POOR_WL_EXPORT void poor_wl_resource_queue_event(struct poor_wl_resource *resource, uint32_t opcode, ...)
{
	union poor_wl_argument args[POOR_WL_CLOSURE_MAX_ARGS];
	va_list ap;

	va_start(ap, opcode);
	poor_wl_argument_from_va_list(resource->object.interface->events[opcode].signature, args, POOR_WL_CLOSURE_MAX_ARGS, ap);
	va_end(ap);
	poor_wl_resource_queue_event_array(resource, opcode, args);
}

/*
 * When a client aborts, its resources are destroyed in id order, so the display resource goes first. Errors from
 * destroying the later ones aren't sent to the dead client.
 */
POOR_WL_EXPORT void poor_wl_resource_post_error_vargs(struct poor_wl_resource *resource, uint32_t code, const char *msg, va_list argp)
{
	struct poor_wl_client *client = resource->client;
	char buffer[128];

	vsnprintf_array(buffer, msg, argp);
	if (client->error || !client->display_resource)
		return;
	poor_wl_resource_post_event(client->display_resource, POOR_WL_DISPLAY_ERROR, resource, code, buffer);
	client_fail(client);
}

POOR_WL_EXPORT void poor_wl_resource_post_error(struct poor_wl_resource *resource, uint32_t code, const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	poor_wl_resource_post_error_vargs(resource, code, msg, ap);
	va_end(ap);
}

static void destroy_client_with_error(struct poor_wl_client *client, const char *reason)
{
	poor_wl_log(poor_wl_server_log_handler, "%s (pid %u)\n", reason, client->cred.pid);
	poor_wl_client_destroy(client);
}

static void client_recv(struct poor_wl_client *client)
{
	if (!poor_wl_connection_can_recv(&client->connection)) {
		destroy_client_with_error(client, "too many file descriptors from client");
		return;
	}
	poor_wl_connection_prep_recv(&client->connection, client_sqe(client, &client->recv_op), client->fd);
}

static void client_send(struct poor_wl_client *client, int flags)
{
	poor_wl_connection_prep_send(&client->connection, client_sqe(client, &client->send_op), client->fd, flags);
}

/*
 * A message longer than POOR_WL_MAX_MESSAGE_SIZE could never be dispatched, so the client is disconnected with a
 * protocol error. The limit doesn't depend on the buffers the compositor chose.
 */
static void client_dispatch(struct poor_wl_client *client)
{
	struct poor_wl_connection *connection = &client->connection;
	struct poor_loop *loop = client->display->loop;
	const struct poor_wl_message *message;
	struct poor_wl_closure_data data;
	struct poor_wl_resource *resource;
	struct poor_wl_closure closure;
	struct poor_wl_object *object;
	uint32_t p[2], len, opcode, size;
	int since;

	while (!client->error && !client->destroyed && (len = poor_wl_connection_pending_input(connection)) >= ARRAY_SIZE_BYTES(p)) {
		poor_wl_connection_copy(connection, p);
		opcode = p[1] & 0xffff;
		size = p[1] >> 16;
		if (size > POOR_WL_MAX_MESSAGE_SIZE) {
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_METHOD,
					       "message length %u exceeds %d", size, POOR_WL_MAX_MESSAGE_SIZE);
			break;
		}
		if (len < size)
			break;
		resource = poor_wl_map_lookup(&client->objects, p[0]);
		if (!resource) {
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid object %u", p[0]);
			break;
		}
		object = &resource->object;
		if (opcode >= (uint32_t)object->interface->method_count) {
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_METHOD,
					       "invalid method %d, object %s#%u", opcode, object->interface->name, object->id);
			break;
		}
		message = &object->interface->methods[opcode];
		since = poor_wl_message_get_since(message);
		if (resource->version > 0 && resource->version < since) {
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_METHOD,
					       "invalid version for %s#%u.%s (%d, need at least %d)", object->interface->name,
					       object->id, message->name, resource->version, since);
			break;
		}
		if (poor_wl_connection_demarshal(connection, loop, poor_wl_server_log_handler, size, &client->objects, message,
						 &closure, &data) < 0) {
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_METHOD,
					       "invalid arguments for %s#%u.%s", object->interface->name, object->id, message->name);
			break;
		}
		if (poor_wl_closure_lookup_objects(&closure, &client->objects) < 0) {
			poor_wl_closure_close_fds(&closure, loop);
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_METHOD,
					       "invalid arguments for %s#%u.%s", object->interface->name, object->id, message->name);
			break;
		}
		log_closure(resource, &closure, false);
		if (resource->dispatcher)
			poor_wl_closure_dispatch(&closure, resource->dispatcher, object, opcode);
		else
			poor_wl_closure_invoke(&closure, POOR_WL_CLOSURE_INVOKE_SERVER, object, opcode, client);
	}
}

/* A hangup ends the client quietly, after the requests that came before it */
POOR_LOOP_GROUP_OP(client_on_recv, loop, io, op, cqe)
{
	struct poor_wl_client *client = client_of(io);
	int ret = cqe->res > 0 ? poor_wl_connection_recv_done(&client->connection, loop, cqe->res) : 0;

	if (client->destroyed)
		return;
	if (!cqe->res) {
		poor_wl_client_destroy(client);
		return;
	}
	if (cqe->res < 0 || ret < 0) {
		destroy_client_with_error(client, "failed to read client connection");
		return;
	}
	client_dispatch(client);
	if (client->destroyed)
		return;
	if (client->error)
		destroy_client_with_error(client, "error in client communication");
	else
		client_recv(client);
}

/* A destroyed client gets what is left without waiting, as long as the socket takes it */
POOR_LOOP_GROUP_OP(client_on_send, loop, io, op, cqe)
{
	struct poor_wl_client *client = client_of(io);

	poor_wl_connection_send_done(&client->connection, loop, cqe->res);
	if (client->destroyed) {
		if (cqe->res > 0 && poor_wl_connection_has_output(&client->connection))
			client_send(client, MSG_DONTWAIT);
		return;
	}
	if (cqe->res < 0) {
		poor_wl_client_destroy(client);
		return;
	}
	if (poor_wl_connection_has_output(&client->connection))
		client_send(client, 0);
}

/* Requests are read once the credentials are known, so compositors can check them in any request */
POOR_LOOP_GROUP_OP(client_on_cred, loop, io, op, cqe)
{
	struct poor_wl_client *client = client_of(io);

	if (client->destroyed)
		return;
	if (cqe->res < 0) {
		poor_wl_log(poor_wl_server_log_handler, "failed to get client credentials: %s\n", strerror(-cqe->res));
		poor_wl_client_destroy(client);
		return;
	}
	client->ready = true;
	poor_wl_priv_signal_emit(&client->display->create_client_signal, client);
	if (!client->destroyed)
		client_recv(client);
}

POOR_LOOP_GROUP_TIMER(client_on_timer, loop, io, timer)
{
	struct poor_wl_client *client = client_of(io);

	if (client->destroyed)
		return;
	if (client->error)
		destroy_client_with_error(client, "error in client communication");
	else
		poor_wl_client_flush(client);
}

POOR_LOOP_GROUP_DONE(poor_wl_client_io, loop, io)
{
	struct poor_wl_client *client = client_of(io);
	struct poor_wl_display *display = client->display;

	poor_wl_connection_close_fds(&client->connection, loop);
	poor_loop_close_fd_or_submit(loop, client->fd, IOSQE_ASYNC);
	client->done(client);
	display_release(display);
}

POOR_WL_EXPORT void poor_wl_client_flush(struct poor_wl_client *client)
{
	if (client->destroyed || client->send_op.pending || !client->connection.want_flush)
		return;
	client->connection.want_flush = false;
	if (poor_wl_connection_has_output(&client->connection))
		client_send(client, 0);
}

POOR_WL_EXPORT struct poor_wl_display *poor_wl_client_get_display(struct poor_wl_client *client)
{
	return client->display;
}

static const struct poor_wl_display_interface display_interface;

static void destroy_client_display_resource(struct poor_wl_resource *resource)
{
	resource->client->display_resource = nullptr;
}

static void bind_display(struct poor_wl_client *client, struct poor_wl_display *display)
{
	[[maybe_unused]] int ret;

	client->display_resource = &client->display_object.resource;
	ret = poor_wl_resource_init(client->display_resource, client, &poor_wl_display_interface, 1, 1);
	assert(!ret);
	poor_wl_resource_set_implementation(client->display_resource, &display_interface, display, destroy_client_display_resource);
}

POOR_WL_EXPORT void poor_wl_client_init(struct poor_wl_client *client, struct poor_wl_display *display, int fd,
			      const struct poor_wl_connection_memory *memory, poor_wl_client_done_fn *done)
{
	assert(memory->client_objects_size >= 2 && !display->destroyed);
	poor_loop_group_init(&client->io);
	poor_loop_op_init(&client->recv_op, client_on_recv);
	poor_loop_op_init(&client->send_op, client_on_send);
	poor_loop_op_init(&client->cred_op, client_on_cred);
	poor_loop_timer_init(&client->timer, client_on_timer);
	poor_wl_connection_init(&client->connection, memory);
	poor_wl_map_init(&client->objects, POOR_WL_MAP_SERVER_SIDE, memory);
	poor_wl_map_insert_at(&client->objects, 0, 0, nullptr);
	poor_wl_priv_signal_init(&client->destroy_signal);
	poor_wl_priv_signal_init(&client->destroy_late_signal);
	poor_wl_priv_signal_init(&client->resource_created_signal);
	client->display = display;
	client->done = done;
	client->data = nullptr;
	client->data_dtor = nullptr;
	client->fd = fd;
	client->cred.pid = 0;
	client->ready = client->error = client->destroyed = false;
	bind_display(client, display);
	poor_wl_list_insert(display->client_list.prev, &client->link);
	poor_loop_group_acquire(&display->io);
	poor_loop_group_acquire(&client->io);
	io_uring_prep_cmd_sock_array(client_sqe(client, &client->cred_op), SOCKET_URING_OP_GETSOCKOPT, fd, SOL_SOCKET, SO_PEERCRED,
				     array_ptr(&client->cred));
}

POOR_WL_EXPORT void poor_wl_client_get_credentials(const struct poor_wl_client *client, pid_t *pid, uid_t *uid, gid_t *gid)
{
	assert(client->ready);
	if (pid)
		*pid = client->cred.pid;
	if (uid)
		*uid = client->cred.uid;
	if (gid)
		*gid = client->cred.gid;
}

POOR_WL_EXPORT int poor_wl_client_get_fd(struct poor_wl_client *client)
{
	return client->fd;
}

POOR_WL_EXPORT struct poor_wl_resource *poor_wl_client_get_object(struct poor_wl_client *client, uint32_t id)
{
	return poor_wl_map_lookup(&client->objects, id);
}

POOR_WL_EXPORT void poor_wl_client_post_no_memory(struct poor_wl_client *client)
{
	poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_NO_MEMORY, "no memory");
}

POOR_WL_EXPORT void poor_wl_client_post_implementation_error(struct poor_wl_client *client, char const *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	poor_wl_resource_post_error_vargs(client->display_resource, POOR_WL_DISPLAY_ERROR_IMPLEMENTATION, msg, ap);
	va_end(ap);
}

POOR_WL_EXPORT void poor_wl_resource_post_no_memory(struct poor_wl_resource *resource)
{
	poor_wl_resource_post_error(resource->client->display_resource, POOR_WL_DISPLAY_ERROR_NO_MEMORY, "no memory");
}

/*
 * The destructor is the last use of the resource, its memory can go away in it. Listeners and destruction code can
 * still find the resource in the client's object map.
 */
static enum poor_wl_iterator_result remove_and_destroy_resource(void *element, void *, uint32_t)
{
	struct poor_wl_resource *resource = element;
	struct poor_wl_client *client = resource->client;
	uint32_t id = resource->object.id;

	poor_wl_priv_signal_final_emit(&resource->destroy_signal, resource);
	if (resource->destroy)
		resource->destroy(resource);
	if (id < POOR_WL_SERVER_ID_START) {
		if (client->display_resource)
			poor_wl_resource_queue_event(client->display_resource, POOR_WL_DISPLAY_DELETE_ID, id);
		poor_wl_map_insert_at(&client->objects, 0, id, nullptr);
	} else {
		poor_wl_map_remove(&client->objects, id);
	}
	return POOR_WL_ITERATOR_CONTINUE;
}

POOR_WL_EXPORT void poor_wl_resource_destroy(struct poor_wl_resource *resource)
{
	remove_and_destroy_resource(resource, nullptr, 0);
}

POOR_WL_EXPORT uint32_t poor_wl_resource_get_id(const struct poor_wl_resource *resource)
{
	return resource->object.id;
}

POOR_WL_EXPORT struct poor_wl_list *poor_wl_resource_get_link(struct poor_wl_resource *resource)
{
	return &resource->link;
}

POOR_WL_EXPORT struct poor_wl_resource *poor_wl_resource_from_link(struct poor_wl_list *link)
{
	return container_of(link, struct poor_wl_resource, link);
}

POOR_WL_EXPORT struct poor_wl_resource *poor_wl_resource_find_for_client(struct poor_wl_list *list, struct poor_wl_client *client)
{
	struct poor_wl_resource *resource;

	if (!client)
		return nullptr;
	poor_wl_list_for_each(resource, list, link)
		if (resource->client == client)
			return resource;
	return nullptr;
}

POOR_WL_EXPORT struct poor_wl_client *poor_wl_resource_get_client(struct poor_wl_resource *resource)
{
	return resource->client;
}

POOR_WL_EXPORT void poor_wl_resource_set_user_data(struct poor_wl_resource *resource, void *data)
{
	resource->data = data;
}

POOR_WL_EXPORT void *poor_wl_resource_get_user_data(struct poor_wl_resource *resource)
{
	return resource->data;
}

POOR_WL_EXPORT int poor_wl_resource_get_version(const struct poor_wl_resource *resource)
{
	return resource->version;
}

POOR_WL_EXPORT void poor_wl_resource_set_destructor(struct poor_wl_resource *resource, poor_wl_resource_destroy_func_t destroy)
{
	resource->destroy = destroy;
}

POOR_WL_EXPORT int poor_wl_resource_instance_of(struct poor_wl_resource *resource, const struct poor_wl_interface *interface,
				      const void *implementation)
{
	return poor_wl_interface_equal(resource->object.interface, interface) && resource->object.implementation == implementation;
}

POOR_WL_EXPORT void poor_wl_resource_add_destroy_listener(struct poor_wl_resource *resource, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&resource->destroy_signal, listener);
}

POOR_WL_EXPORT struct poor_wl_listener *poor_wl_resource_get_destroy_listener(struct poor_wl_resource *resource, poor_wl_notify_func_t notify)
{
	return poor_wl_priv_signal_get(&resource->destroy_signal, notify);
}

POOR_WL_EXPORT const char *poor_wl_resource_get_class(const struct poor_wl_resource *resource)
{
	return resource->object.interface->name;
}

POOR_WL_EXPORT const struct poor_wl_interface *poor_wl_resource_get_interface(struct poor_wl_resource *resource)
{
	return resource->object.interface;
}

/* The listeners are called when poor_wl_client_destroy() starts, before the client's resources are destroyed */
POOR_WL_EXPORT void poor_wl_client_add_destroy_listener(struct poor_wl_client *client, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&client->destroy_signal, listener);
}

POOR_WL_EXPORT struct poor_wl_listener *poor_wl_client_get_destroy_listener(struct poor_wl_client *client, poor_wl_notify_func_t notify)
{
	return poor_wl_priv_signal_get(&client->destroy_signal, notify);
}

/* The listeners are called at the end of poor_wl_client_destroy(), after the client's resources are destroyed */
POOR_WL_EXPORT void poor_wl_client_add_destroy_late_listener(struct poor_wl_client *client, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&client->destroy_late_signal, listener);
}

POOR_WL_EXPORT struct poor_wl_listener *poor_wl_client_get_destroy_late_listener(struct poor_wl_client *client, poor_wl_notify_func_t notify)
{
	return poor_wl_priv_signal_get(&client->destroy_late_signal, notify);
}

/*
 * Queued events are sent if the socket takes them right away: a send in flight may be waiting for room, so it is
 * cancelled, and what it didn't send goes once more without waiting.
 */
POOR_WL_EXPORT void poor_wl_client_destroy(struct poor_wl_client *client)
{
	struct poor_loop *loop = client->display->loop;

	if (client->destroyed) {
		client->error = true;
		poor_wl_log(poor_wl_server_log_handler, "poor_wl_client_destroy: encountered re-entrant client destruction.\n");
		return;
	}
	client->destroyed = true;
	poor_wl_list_remove(&client->link);
	poor_wl_list_init(&client->link);
	poor_wl_priv_signal_final_emit(&client->destroy_signal, client);
	if (client->send_op.pending)
		poor_wl_cancel_op(loop, &client->send_op);
	else if (poor_wl_connection_has_output(&client->connection))
		client_send(client, MSG_DONTWAIT);
	poor_wl_map_for_each(&client->objects, remove_and_destroy_resource, nullptr);
	poor_wl_priv_signal_final_emit(&client->destroy_late_signal, client);
	poor_wl_list_remove(&client->resource_created_signal.listener_list);
	if (client->data_dtor)
		client->data_dtor(client->data);
	poor_wl_cancel_op(loop, &client->recv_op);
	poor_wl_cancel_op(loop, &client->cred_op);
	poor_loop_group_timer_arm(loop, &client->io, &client->timer, 0);
	client_release(client);
}

/*
 * If no poor_wl_global filter has been registered, this function will return true, allowing the poor_wl_global to be visible to
 * the poor_wl_client
 */
static bool poor_wl_global_is_visible(const struct poor_wl_client *client, const struct poor_wl_global *global)
{
	struct poor_wl_display *display = client->display;

	return !display->global_filter || display->global_filter(client, global, display->global_filter_data);
}

static bool registry_announced(struct registry *registry, const struct poor_wl_global *global)
{
	return poor_wl_global_is_visible(registry->resource.resource.client, global) &&
	       (!global->removed || global->removed_generation > registry->generation);
}

static void global_acked(struct poor_wl_global *global)
{
	if (!--global->pending_acks && global->withdrawn)
		global->withdrawn(global);
}

static void registry_bind(struct poor_wl_client *client, struct poor_wl_registry_resource *resource, uint32_t name,
			  const char *interface, uint32_t version, uint32_t id)
{
	struct poor_wl_display *display = resource->resource.data;
	struct poor_wl_global *global;

	poor_wl_list_for_each(global, &display->global_list, link)
		if (global->name == name)
			break;

	if (&global->link == &display->global_list || !poor_wl_global_is_visible(client, global))
		poor_wl_resource_post_error(&resource->resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "global %s (%" PRIu32 ") is unavailable", interface, name);
	else if (strcmp(global->interface->name, interface) != 0)
		poor_wl_resource_post_error(&resource->resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "invalid interface for global %" PRIu32 ": expected %s, got %s", name,
				       global->interface->name, interface);
	else if (version == 0)
		poor_wl_resource_post_error(&resource->resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "invalid version for global %s (%" PRIu32 "): 0 is not a valid version", interface, name);
	else if (global->version < version)
		poor_wl_resource_post_error(&resource->resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "invalid version for global %s (%" PRIu32 "): expected at most %" PRIu32 ", got %" PRIu32,
				       interface, name, global->version, version);
	else
		global->bind(client, global->data, version, id);
}

static const struct poor_wl_registry_interface registry_interface = {
	registry_bind,
};

/* The callback is created and destroyed right away, so it lives on the stack */
static void display_sync(struct poor_wl_client *client, struct poor_wl_display_resource *, uint32_t id)
{
	struct poor_wl_callback_resource callback;

	if (poor_wl_callback_resource_init(&callback, client, 1, id) < 0) {
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_callback_send_done(&callback, poor_wl_display_get_serial(client->display));
	poor_wl_resource_destroy(&callback.resource);
}

/* Dropping the registry acknowledges the removals of the globals it announced */
static void unbind_registry_resource(struct poor_wl_resource *resource)
{
	struct registry *registry = container_of(resource, struct registry, resource.resource);
	struct poor_wl_display *display = resource->data;
	struct poor_wl_global *global, *next;

	poor_wl_list_remove(&resource->link);
	poor_wl_list_for_each_safe(global, next, &display->global_list, link)
		if (global->removed && global->ack_slot >= 0 && !(registry->acked & (1ull << global->ack_slot)) &&
		    registry_announced(registry, global))
			global_acked(global);
	display->free(display, registry);
}

static void display_get_registry(struct poor_wl_client *client, struct poor_wl_display_resource *resource, uint32_t id)
{
	struct poor_wl_display *display = resource->resource.data;
	struct registry *registry = display->alloc ? display->alloc(display, client, sizeof(*registry)) : nullptr;
	struct poor_wl_global *global;

	if (!registry) {
		poor_wl_client_post_no_memory(client);
		return;
	}
	if (poor_wl_registry_resource_init(&registry->resource, client, 1, id) < 0) {
		display->free(display, registry);
		poor_wl_client_post_no_memory(client);
		return;
	}
	registry->generation = ++display->generation;
	registry->acked = 0;
	poor_wl_registry_resource_set_implementation(&registry->resource, &registry_interface, display, unbind_registry_resource);
	poor_wl_list_insert(&display->registry_list, &registry->resource.resource.link);
	poor_wl_list_for_each(global, &display->global_list, link)
		if (poor_wl_global_is_visible(client, global) && !global->removed)
			poor_wl_registry_send_global(&registry->resource, global->name, global->interface->name, global->version);
}

static const struct poor_wl_display_interface display_interface = {
	display_sync,
	display_get_registry,
};

POOR_LOOP_GROUP_TIMER(display_on_timer, loop, io, timer)
{
}

POOR_LOOP_GROUP_DONE(poor_wl_display_io, loop, io)
{
	struct poor_wl_display *display = container_of(io, struct poor_wl_display, io);

	display->done(display);
}

POOR_WL_EXPORT void poor_wl_display_init(struct poor_wl_display *display, struct poor_loop *loop, poor_wl_display_done_fn *done)
{
	const char *debug = getenv("WAYLAND_DEBUG"), *no_color = getenv("NO_COLOR"), *force_color = getenv("FORCE_COLOR");

	if (debug && (poor_wl_check_env_token(debug, "server") || poor_wl_check_env_token(debug, "1"))) {
		debug_server = true;
		if (isatty(STDERR_FILENO))
			debug_color = true;
	}
	if (force_color && force_color[0] != '\0')
		debug_color = true;
	if (no_color && no_color[0] != '\0')
		debug_color = false;

	poor_loop_group_init(&display->io);
	poor_loop_group_acquire(&display->io);
	poor_loop_timer_init(&display->timer, display_on_timer);
	display->loop = loop;
	display->done = done;
	display->alloc = nullptr;
	display->free = nullptr;
	display->next_global_name = 1;
	display->serial = 0;
	display->generation = 0;
	display->ack_slots = 0;
	poor_wl_list_init(&display->registry_list);
	poor_wl_list_init(&display->global_list);
	poor_wl_list_init(&display->socket_list);
	poor_wl_list_init(&display->client_list);
	poor_wl_list_init(&display->protocol_loggers);
	poor_wl_priv_signal_init(&display->destroy_signal);
	poor_wl_priv_signal_init(&display->create_client_signal);
	display->shm_formats = nullptr;
	display->shm_formats_count = 0;
	display->global_filter = nullptr;
	display->global_filter_data = nullptr;
	display->destroyed = false;
}

POOR_WL_EXPORT void poor_wl_display_set_allocator(struct poor_wl_display *display, poor_wl_display_alloc_fn *alloc, poor_wl_display_free_fn *free)
{
	display->alloc = alloc;
	display->free = free;
}

/* The timer keeps 'done' out of this call */
POOR_WL_EXPORT void poor_wl_display_destroy(struct poor_wl_display *display)
{
	struct poor_wl_socket *socket;

	assert(!display->destroyed);
	display->destroyed = true;
	poor_wl_priv_signal_final_emit(&display->destroy_signal, display);
	poor_wl_list_for_each(socket, &display->socket_list, link)
		poor_wl_socket_destroy(socket);
	poor_wl_display_destroy_clients(display);
	poor_loop_group_timer_arm(display->loop, &display->io, &display->timer, 0);
	display_release(display);
}

/*
 * Set a filter for the poor_wl_display to advertise or hide global objects to clients. It is used when globals are
 * advertised and bound. It should be installed before any client connects and should always take the same decision
 * for a client and a global.
 */
POOR_WL_EXPORT void poor_wl_display_set_global_filter(struct poor_wl_display *display, poor_wl_display_global_filter_func_t filter, void *data)
{
	display->global_filter = filter;
	display->global_filter_data = data;
}

POOR_WL_EXPORT void poor_wl_global_init(struct poor_wl_global *global, struct poor_wl_display *display, const struct poor_wl_interface *interface,
			      int version, void *data, poor_wl_global_bind_func_t bind)
{
	struct registry *registry;

	assert(version >= 1 && version <= interface->version);
	assert(display->next_global_name < UINT32_MAX);
	global->display = display;
	global->interface = interface;
	global->name = display->next_global_name++;
	global->version = version;
	global->pending_acks = 0;
	global->ack_slot = -1;
	global->data = data;
	global->bind = bind;
	global->withdrawn = nullptr;
	global->removed = false;
	poor_wl_list_insert(display->global_list.prev, &global->link);
	poor_wl_list_for_each(registry, &display->registry_list, resource.resource.link)
		if (poor_wl_global_is_visible(registry->resource.resource.client, global))
			poor_wl_registry_send_global(&registry->resource, global->name, global->interface->name, global->version);
}

static void global_unpublish(struct poor_wl_global *global)
{
	struct poor_wl_display *display = global->display;
	struct registry *registry;

	poor_wl_list_for_each(registry, &display->registry_list, resource.resource.link) {
		if (!registry_announced(registry, global))
			continue;
		global->pending_acks++;
		poor_wl_registry_send_global_remove(&registry->resource, global->name);
	}
	global->removed = true;
	global->removed_generation = ++display->generation;
}

/*
 * Broadcast a global remove event to all clients without destroying the global. This function can only be called
 * once per global.
 *
 * Due to the Wayland protocol being asynchronous, a removed global can still be bound for a while. The withdrawn
 * listener is called once every client has acknowledged the removal with wl_fixes.ack_global_remove, or has dropped
 * the registry, and then poor_wl_global_destroy() can be called. The acknowledgements are tracked with one of 64 bits,
 * so at most 64 removed globals can wait for poor_wl_global_destroy() at once.
 */
POOR_WL_EXPORT void poor_wl_global_remove(struct poor_wl_global *global)
{
	struct poor_wl_display *display = global->display;

	if (global->removed)
		poor_wl_abort("poor_wl_global_remove: called twice on the same global '%s#%" PRIu32 "'", global->interface->name,
			 global->name);
	assert(~display->ack_slots && "too many removed globals");
	global->ack_slot = __builtin_ctzll(~display->ack_slots);
	display->ack_slots |= 1ull << global->ack_slot;
	global_unpublish(global);
	if (global->withdrawn && !global->pending_acks)
		global->withdrawn(global);
}

POOR_WL_EXPORT void poor_wl_global_set_withdrawn_listener(struct poor_wl_global *global, poor_wl_global_withdrawn_func_t func)
{
	global->withdrawn = func;
}

POOR_WL_EXPORT void poor_wl_global_destroy(struct poor_wl_global *global)
{
	struct poor_wl_display *display = global->display;
	struct registry *registry;
	uint64_t bit;

	if (!global->removed)
		global_unpublish(global);
	if (global->ack_slot >= 0) {
		bit = 1ull << global->ack_slot;
		display->ack_slots &= ~bit;
		poor_wl_list_for_each(registry, &display->registry_list, resource.resource.link)
			registry->acked &= ~bit;
	}
	poor_wl_list_remove(&global->link);
}

POOR_WL_EXPORT const struct poor_wl_interface *poor_wl_global_get_interface(const struct poor_wl_global *global)
{
	return global->interface;
}

/* The name is 0 if the global is not visible to the client */
POOR_WL_EXPORT uint32_t poor_wl_global_get_name(const struct poor_wl_global *global, const struct poor_wl_client *client)
{
	return poor_wl_global_is_visible(client, global) ? global->name : 0;
}

POOR_WL_EXPORT uint32_t poor_wl_global_get_version(const struct poor_wl_global *global)
{
	return global->version;
}

POOR_WL_EXPORT struct poor_wl_display *poor_wl_global_get_display(const struct poor_wl_global *global)
{
	return global->display;
}

POOR_WL_EXPORT void *poor_wl_global_get_user_data(const struct poor_wl_global *global)
{
	return global->data;
}

POOR_WL_EXPORT void poor_wl_global_set_user_data(struct poor_wl_global *global, void *data)
{
	global->data = data;
}

POOR_WL_EXPORT uint32_t poor_wl_display_get_serial(const struct poor_wl_display *display)
{
	return display->serial;
}

POOR_WL_EXPORT uint32_t poor_wl_display_next_serial(struct poor_wl_display *display)
{
	return ++display->serial;
}

POOR_WL_EXPORT struct poor_loop *poor_wl_display_get_loop(struct poor_wl_display *display)
{
	return display->loop;
}

POOR_WL_EXPORT void poor_wl_display_flush_clients(struct poor_wl_display *display)
{
	struct poor_wl_client *client;

	poor_wl_list_for_each(client, &display->client_list, link)
		poor_wl_client_flush(client);
}

/* Destroying a client from a destroy listener is safe, but a client created by one isn't destroyed */
POOR_WL_EXPORT void poor_wl_display_destroy_clients(struct poor_wl_display *display)
{
	struct poor_wl_list tmp_client_list;

	poor_wl_list_init(&tmp_client_list);
	poor_wl_list_insert_list(&tmp_client_list, &display->client_list);
	poor_wl_list_init(&display->client_list);
	while (!poor_wl_list_empty(&tmp_client_list))
		poor_wl_client_destroy(poor_wl_client_from_link(tmp_client_list.next));
	if (!poor_wl_list_empty(&display->client_list))
		poor_wl_log(poor_wl_server_log_handler,
			    "poor_wl_display_destroy_clients: cannot destroy all clients because new ones were created by "
			    "destroy callbacks\n");
}

static void socket_lock(struct poor_wl_socket *socket)
{
	io_uring_prep_openat(socket_sqe(socket, &socket->lock_op), AT_FDCWD, socket->lock_addr, O_CREAT | O_CLOEXEC | O_RDWR,
			     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
}

static void socket_create(struct poor_wl_socket *socket)
{
	io_uring_prep_socket(socket_sqe(socket, &socket->socket_op), PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
}

static void socket_accept(struct poor_wl_socket *socket)
{
	io_uring_prep_accept(socket_sqe(socket, &socket->accept_op), socket->fd, nullptr, nullptr, SOCK_CLOEXEC);
}

static void socket_fail(struct poor_wl_socket *socket, int err)
{
	socket->error = err;
	poor_wl_socket_destroy(socket);
}

static int socket_set_name(struct poor_wl_socket *socket, const char *name)
{
	const char *runtime_dir = "", *separator = "";
	int name_size;

	if (name[0] != '/') {
		runtime_dir = getenv("XDG_RUNTIME_DIR");
		if (!runtime_dir || runtime_dir[0] != '/') {
			poor_wl_log(poor_wl_server_log_handler,
				    "error: XDG_RUNTIME_DIR is invalid or not set in the environment\n");
			errno = ENOENT;
			return -1;
		}
		separator = "/";
	}
	name_size = snprintf_array(socket->addr.sun_path, "%s%s%s", runtime_dir, separator, name) + 1;
	assert(name_size > 0);
	if (name_size > (int)ARRAY_SIZE(socket->addr.sun_path)) {
		poor_wl_log(poor_wl_server_log_handler, "error: socket path \"%s%s%s\" plus null terminator exceeds %zu bytes\n",
			    runtime_dir, separator, name, ARRAY_SIZE(socket->addr.sun_path));
		errno = ENAMETOOLONG;
		return -1;
	}
	socket->display_name = socket->addr.sun_path + name_size - 1 - strlen(name);
	snprintf_array(socket->lock_addr, "%s%s", socket->addr.sun_path, LOCK_SUFFIX);
	return 0;
}

static int socket_set_auto_name(struct poor_wl_socket *socket)
{
	char name[20];

	snprintf_array(name, "wayland-%d", socket->displayno);
	return socket_set_name(socket, name);
}

static void socket_start(struct poor_wl_socket *socket)
{
	struct poor_wl_display *display = socket->display;

	assert(!display->destroyed);
	poor_wl_list_insert(display->socket_list.prev, &socket->link);
	poor_loop_group_acquire(&display->io);
	poor_loop_group_acquire(&socket->io);
}

/* The lock and the socket of another compositor are left alone, and the next name is tried */
static void socket_lock_failed(struct poor_wl_socket *socket, int err)
{
	if (socket->fd_lock >= 0) {
		poor_loop_close_fd_or_submit(socket->display->loop, socket->fd_lock, 0);
		socket->fd_lock = -1;
	}
	if (socket->auto_name) {
		err = EINVAL;
		if (socket->displayno < MAX_DISPLAYNO) {
			socket->displayno++;
			if (!socket_set_auto_name(socket)) {
				socket_lock(socket);
				return;
			}
			err = errno;
		}
	}
	socket_fail(socket, err);
}

POOR_LOOP_GROUP_OP(socket_on_lock, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (socket->closing) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(loop, cqe->res, 0);
		return;
	}
	if (cqe->res < 0) {
		poor_wl_log(poor_wl_server_log_handler, "unable to open lockfile %s check permissions\n", socket->lock_addr);
		socket_lock_failed(socket, -cqe->res);
		return;
	}
	socket->fd_lock = cqe->res;
	if (flock(socket->fd_lock, LOCK_EX | LOCK_NB) < 0) {
		poor_wl_log(poor_wl_server_log_handler, "unable to lock lockfile %s, maybe another compositor is running\n",
			    socket->lock_addr);
		socket_lock_failed(socket, errno);
		return;
	}
	io_uring_prep_statx(socket_sqe(socket, &socket->stat_op), AT_FDCWD, socket->addr.sun_path, AT_SYMLINK_NOFOLLOW,
			    STATX_MODE, &socket->stx);
}

/* A socket left behind by a compositor that held the lock before is removed */
POOR_LOOP_GROUP_OP(socket_on_stat, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (socket->closing)
		return;
	if (cqe->res < 0 && cqe->res != -ENOENT) {
		poor_wl_log(poor_wl_server_log_handler, "did not manage to stat file %s\n", socket->addr.sun_path);
		socket_lock_failed(socket, -cqe->res);
		return;
	}
	if (!cqe->res && socket->stx.stx_mode & (S_IWUSR | S_IWGRP))
		io_uring_prep_unlinkat(socket_sqe(socket, &socket->unlink_op), AT_FDCWD, socket->addr.sun_path, 0);
	else
		socket_create(socket);
}

POOR_LOOP_GROUP_OP(socket_on_unlink, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (!socket->closing)
		socket_create(socket);
}

/* bind and listen are linked, so a failed bind cancels the listen */
POOR_LOOP_GROUP_OP(socket_on_socket, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);
	struct io_uring_sqe *sqe;

	if (cqe->res >= 0)
		socket->fd = cqe->res;
	if (socket->closing)
		return;
	if (cqe->res < 0) {
		socket_fail(socket, -cqe->res);
		return;
	}
	if (poor_loop_check_sq_space_or_submit(loop, 2))
		poor_wl_abort("io_uring: %s\n", strerror(errno));
	sqe = socket_sqe(socket, &socket->bind_op);
	io_uring_prep_bind(sqe, socket->fd, (struct sockaddr *)&socket->addr,
			   offsetof(struct sockaddr_un, sun_path) + strlen(socket->addr.sun_path));
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_prep_listen(socket_sqe(socket, &socket->listen_op), socket->fd, 128);
}

/* Called once both bind and listen completed, in whatever order their completions came */
static void socket_bound(struct poor_wl_socket *socket)
{
	if (socket->closing)
		return;
	if (socket->bind_error) {
		poor_wl_log(poor_wl_server_log_handler, "bind() failed with error: %s\n", strerror(socket->bind_error));
		socket_fail(socket, socket->bind_error);
		return;
	}
	if (socket->listen_error) {
		poor_wl_log(poor_wl_server_log_handler, "listen() failed with error: %s\n", strerror(socket->listen_error));
		socket_fail(socket, socket->listen_error);
		return;
	}
	socket->ready(socket);
	if (!socket->closing)
		socket_accept(socket);
}

POOR_LOOP_GROUP_OP(socket_on_bind, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (cqe->res < 0)
		socket->bind_error = -cqe->res;
	if (!socket->listen_op.pending)
		socket_bound(socket);
}

POOR_LOOP_GROUP_OP(socket_on_listen, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (cqe->res < 0)
		socket->listen_error = -cqe->res;
	if (!socket->bind_op.pending)
		socket_bound(socket);
}

/* Running out of fds or memory makes accept fail right away while a connection waits, so it is retried later */
POOR_LOOP_GROUP_OP(socket_on_accept, loop, io, op, cqe)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (socket->closing) {
		if (cqe->res >= 0)
			poor_loop_close_fd_or_submit(loop, cqe->res, IOSQE_ASYNC);
		return;
	}
	switch (cqe->res) {
	case -EMFILE:
	case -ENFILE:
	case -ENOBUFS:
	case -ENOMEM:
		poor_wl_log(poor_wl_server_log_handler, "failed to accept: %s\n", strerror(-cqe->res));
		poor_loop_group_timer_arm(loop, io, &socket->timer, poor_loop_now() + ACCEPT_RETRY_NS);
		return;
	case -EBADF:
	case -EINVAL:
	case -ENOTSOCK:
	case -EOPNOTSUPP:
		poor_wl_log(poor_wl_server_log_handler, "failed to accept: %s\n", strerror(-cqe->res));
		socket_fail(socket, -cqe->res);
		return;
	}
	if (cqe->res < 0)
		poor_wl_log(poor_wl_server_log_handler, "failed to accept: %s\n", strerror(-cqe->res));
	else
		socket->accept(socket, cqe->res);
	if (!socket->closing)
		socket_accept(socket);
}

POOR_LOOP_GROUP_OP(socket_on_cleanup, loop, io, op, cqe)
{
}

POOR_LOOP_GROUP_TIMER(socket_on_timer, loop, io, timer)
{
	struct poor_wl_socket *socket = socket_of(io);

	if (!socket->closing)
		socket_accept(socket);
}

/* The socket and the lock file are removed while the lock is still held, then both are closed */
POOR_LOOP_GROUP_DONE(poor_wl_socket_io, loop, io)
{
	struct poor_wl_socket *socket = socket_of(io);
	struct poor_wl_display *display = socket->display;
	struct io_uring_sqe *sqe;

	if (!socket->cleaned && socket->fd_lock >= 0) {
		socket->cleaned = true;
		if (poor_loop_check_sq_space_or_submit(loop, 2))
			poor_wl_abort("io_uring: %s\n", strerror(errno));
		sqe = socket_sqe(socket, &socket->unlink_op);
		io_uring_prep_unlinkat(sqe, AT_FDCWD, socket->addr.sun_path, 0);
		sqe->flags |= IOSQE_IO_HARDLINK;
		io_uring_prep_unlinkat(socket_sqe(socket, &socket->cleanup_op), AT_FDCWD, socket->lock_addr, 0);
		return;
	}
	if (socket->fd >= 0)
		poor_loop_close_fd_or_submit(loop, socket->fd, IOSQE_ASYNC);
	if (socket->fd_lock >= 0)
		poor_loop_close_fd_or_submit(loop, socket->fd_lock, 0);
	poor_wl_list_remove(&socket->link);
	socket->done(socket);
	display_release(display);
}

POOR_WL_EXPORT void poor_wl_socket_init(struct poor_wl_socket *socket, struct poor_wl_display *display, poor_wl_socket_ready_fn *ready,
			      poor_wl_socket_accept_fn *accept, poor_wl_socket_done_fn *done)
{
	poor_loop_group_init(&socket->io);
	poor_loop_op_init(&socket->lock_op, socket_on_lock);
	poor_loop_op_init(&socket->stat_op, socket_on_stat);
	poor_loop_op_init(&socket->unlink_op, socket_on_unlink);
	poor_loop_op_init(&socket->socket_op, socket_on_socket);
	poor_loop_op_init(&socket->bind_op, socket_on_bind);
	poor_loop_op_init(&socket->listen_op, socket_on_listen);
	poor_loop_op_init(&socket->accept_op, socket_on_accept);
	poor_loop_op_init(&socket->cleanup_op, socket_on_cleanup);
	poor_loop_timer_init(&socket->timer, socket_on_timer);
	socket->display = display;
	socket->ready = ready;
	socket->accept = accept;
	socket->done = done;
	socket->addr.sun_family = AF_LOCAL;
	socket->display_name = nullptr;
	socket->fd = socket->fd_lock = -1;
	socket->error = socket->bind_error = socket->listen_error = 0;
	socket->auto_name = socket->closing = socket->cleaned = false;
}

POOR_WL_EXPORT int poor_wl_socket_listen(struct poor_wl_socket *socket, const char *name)
{
	if (!name)
		name = getenv("WAYLAND_DISPLAY");
	if (!name)
		name = "wayland-0";
	if (socket_set_name(socket, name) < 0)
		return -1;
	socket_start(socket);
	socket_lock(socket);
	return 0;
}

POOR_WL_EXPORT int poor_wl_socket_listen_auto(struct poor_wl_socket *socket)
{
	socket->auto_name = true;
	socket->displayno = 0;
	if (socket_set_auto_name(socket) < 0)
		return -1;
	socket_start(socket);
	socket_lock(socket);
	return 0;
}

POOR_WL_EXPORT void poor_wl_socket_listen_fd(struct poor_wl_socket *socket, int fd)
{
	assert(fd >= 0);
	socket->fd = fd;
	socket_start(socket);
	socket_accept(socket);
}

/* The timer keeps 'done' out of this call */
POOR_WL_EXPORT void poor_wl_socket_destroy(struct poor_wl_socket *socket)
{
	struct poor_loop *loop = socket->display->loop;

	if (socket->closing)
		return;
	socket->closing = true;
	poor_wl_cancel_op(loop, &socket->lock_op);
	poor_wl_cancel_op(loop, &socket->stat_op);
	poor_wl_cancel_op(loop, &socket->unlink_op);
	poor_wl_cancel_op(loop, &socket->socket_op);
	poor_wl_cancel_op(loop, &socket->bind_op);
	poor_wl_cancel_op(loop, &socket->listen_op);
	poor_wl_cancel_op(loop, &socket->accept_op);
	poor_loop_group_timer_arm(loop, &socket->io, &socket->timer, 0);
	socket_release(socket);
}

POOR_WL_EXPORT const char *poor_wl_socket_get_name(const struct poor_wl_socket *socket)
{
	return socket->display_name;
}

POOR_WL_EXPORT void poor_wl_display_add_destroy_listener(struct poor_wl_display *display, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&display->destroy_signal, listener);
}

POOR_WL_EXPORT void poor_wl_display_add_client_created_listener(struct poor_wl_display *display, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&display->create_client_signal, listener);
}

POOR_WL_EXPORT struct poor_wl_listener *poor_wl_display_get_destroy_listener(struct poor_wl_display *display, poor_wl_notify_func_t notify)
{
	return poor_wl_priv_signal_get(&display->destroy_signal, notify);
}

POOR_WL_EXPORT void poor_wl_resource_set_implementation(struct poor_wl_resource *resource, const void *implementation, void *data,
					      poor_wl_resource_destroy_func_t destroy)
{
	resource->object.implementation = implementation;
	resource->data = data;
	resource->destroy = destroy;
	resource->dispatcher = nullptr;
}

POOR_WL_EXPORT void poor_wl_resource_set_dispatcher(struct poor_wl_resource *resource, poor_wl_dispatcher_func_t dispatcher,
					  const void *implementation, void *data, poor_wl_resource_destroy_func_t destroy)
{
	resource->dispatcher = dispatcher;
	resource->object.implementation = implementation;
	resource->data = data;
	resource->destroy = destroy;
}

POOR_WL_EXPORT int poor_wl_resource_init(struct poor_wl_resource *resource, struct poor_wl_client *client, const struct poor_wl_interface *interface,
			       int version, uint32_t id)
{
	if (!id) {
		id = poor_wl_map_insert_new(&client->objects, 0, nullptr);
		if (!id)
			return -1;
	}
	resource->object.id = id;
	resource->object.interface = interface;
	resource->object.implementation = nullptr;
	resource->destroy = nullptr;
	poor_wl_list_init(&resource->link);
	resource->client = client;
	resource->data = nullptr;
	resource->version = version;
	resource->dispatcher = nullptr;
	poor_wl_priv_signal_init(&resource->destroy_signal);
	if (poor_wl_map_insert_at(&client->objects, 0, id, resource) < 0) {
		if (errno == EINVAL)
			poor_wl_resource_post_error(client->display_resource, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid new id %d", id);
		return -1;
	}
	poor_wl_priv_signal_emit(&client->resource_created_signal, resource);
	return 0;
}

poor_wl_log_func_t poor_wl_server_log_handler;

POOR_WL_EXPORT void poor_wl_log_set_handler_server(poor_wl_log_func_t handler)
{
	poor_wl_server_log_handler = handler;
}

/*
 * Add a protocol logger that is called with every request and event. The messages are valid only during the call.
 */
POOR_WL_EXPORT void poor_wl_display_add_protocol_logger(struct poor_wl_display *display, struct poor_wl_protocol_logger *logger,
					      poor_wl_protocol_logger_func_t func, void *user_data)
{
	logger->func = func;
	logger->user_data = user_data;
	poor_wl_list_insert(&display->protocol_loggers, &logger->link);
}

POOR_WL_EXPORT void poor_wl_protocol_logger_destroy(struct poor_wl_protocol_logger *logger)
{
	poor_wl_list_remove(&logger->link);
}

POOR_WL_EXPORT void poor_wl_display_set_shm_formats_impl(struct poor_wl_display *display, size_t count, const uint32_t (*formats)[count])
{
	display->shm_formats = *formats;
	display->shm_formats_count = count;
}

POOR_WL_EXPORT struct poor_wl_list *poor_wl_display_get_client_list(struct poor_wl_display *display)
{
	return &display->client_list;
}

POOR_WL_EXPORT struct poor_wl_list *poor_wl_client_get_link(struct poor_wl_client *client)
{
	return &client->link;
}

POOR_WL_EXPORT struct poor_wl_client *poor_wl_client_from_link(struct poor_wl_list *link)
{
	return container_of(link, struct poor_wl_client, link);
}

POOR_WL_EXPORT void poor_wl_client_add_resource_created_listener(struct poor_wl_client *client, struct poor_wl_listener *listener)
{
	poor_wl_priv_signal_add(&client->resource_created_signal, listener);
}

struct poor_wl_resource_iterator_context {
	void *user_data;
	poor_wl_client_for_each_resource_iterator_func_t it;
};

static enum poor_wl_iterator_result resource_iterator_helper(void *res, void *user_data, uint32_t)
{
	struct poor_wl_resource_iterator_context *context = user_data;

	return context->it(res, context->user_data);
}

/* Creating and destroying resources while iterating is safe, but new resources may or may not be visited */
POOR_WL_EXPORT void poor_wl_client_for_each_resource(struct poor_wl_client *client, poor_wl_client_for_each_resource_iterator_func_t iterator,
					   void *user_data)
{
	struct poor_wl_resource_iterator_context context = {
		.user_data = user_data,
		.it = iterator,
	};

	poor_wl_map_for_each(&client->objects, resource_iterator_helper, &context);
}

static void handle_noop(struct poor_wl_listener *, void *)
{
}

/*
 * A cursor and an end marker go into the list: listeners left of the cursor were called and the ones right of the
 * end marker were added during the emission. Listeners can remove any listener.
 */
POOR_WL_EXPORT void poor_wl_signal_emit_mutable(struct poor_wl_signal *signal, void *data)
{
	struct poor_wl_listener cursor;
	struct poor_wl_listener end;

	poor_wl_list_insert(&signal->listener_list, &cursor.link);
	cursor.notify = handle_noop;
	poor_wl_list_insert(signal->listener_list.prev, &end.link);
	end.notify = handle_noop;
	while (cursor.link.next != &end.link) {
		struct poor_wl_list *pos = cursor.link.next;
		struct poor_wl_listener *l = container_of(pos, struct poor_wl_listener, link);

		poor_wl_list_remove(&cursor.link);
		poor_wl_list_insert(pos, &cursor.link);
		l->notify(l, data);
	}
	poor_wl_list_remove(&cursor.link);
	poor_wl_list_remove(&end.link);
}

/* \cond INTERNAL */

void poor_wl_priv_signal_init(struct poor_wl_priv_signal *signal)
{
	poor_wl_list_init(&signal->listener_list);
	poor_wl_list_init(&signal->emit_list);
}

void poor_wl_priv_signal_add(struct poor_wl_priv_signal *signal, struct poor_wl_listener *listener)
{
	poor_wl_list_insert(signal->listener_list.prev, &listener->link);
}

struct poor_wl_listener *poor_wl_priv_signal_get(struct poor_wl_priv_signal *signal, poor_wl_notify_func_t notify)
{
	struct poor_wl_listener *l;

	poor_wl_list_for_each(l, &signal->listener_list, link)
		if (l->notify == notify)
			return l;
	poor_wl_list_for_each(l, &signal->emit_list, link)
		if (l->notify == notify)
			return l;
	return nullptr;
}

/*
 * Listeners are moved to a temporary list, so they can remove any listener: the first one is always taken, not the
 * one after the current.
 */
void poor_wl_priv_signal_emit(struct poor_wl_priv_signal *signal, void *data)
{
	struct poor_wl_listener *l;
	struct poor_wl_list *pos;

	poor_wl_list_insert_list(&signal->emit_list, &signal->listener_list);
	poor_wl_list_init(&signal->listener_list);
	while (!poor_wl_list_empty(&signal->emit_list)) {
		pos = signal->emit_list.next;
		l = container_of(pos, struct poor_wl_listener, link);
		poor_wl_list_remove(pos);
		poor_wl_list_insert(&signal->listener_list, pos);
		l->notify(l, data);
	}
}

/*
 * Every listener is isolated before it is notified, so a listener can be freed without being removed, as much code
 * does with destroy listeners.
 */
void poor_wl_priv_signal_final_emit(struct poor_wl_priv_signal *signal, void *data)
{
	struct poor_wl_listener *l;
	struct poor_wl_list *pos;

	while (!poor_wl_list_empty(&signal->listener_list)) {
		pos = signal->listener_list.next;
		l = container_of(pos, struct poor_wl_listener, link);
		poor_wl_list_remove(pos);
		poor_wl_list_init(pos);
		l->notify(l, data);
	}
}

/* \endcond */

/*
 * Set the client's user data. 'dtor' is called after all resources have been destroyed and all destroy listeners
 * have been called, even if 'data' is NULL.
 */
POOR_WL_EXPORT void poor_wl_client_set_user_data(struct poor_wl_client *client, void *data, poor_wl_user_data_destroy_func_t dtor)
{
	client->data = data;
	client->data_dtor = dtor;
}

POOR_WL_EXPORT void *poor_wl_client_get_user_data(struct poor_wl_client *client)
{
	return client->data;
}

/*
 * The compositor calls this from its wl_fixes.ack_global_remove implementation. Dropping the registry or the client
 * acknowledges the removal too.
 */
POOR_WL_EXPORT void poor_wl_fixes_handle_ack_global_remove(struct poor_wl_fixes_resource *fixes_resource,
							   struct poor_wl_registry_resource *registry_resource,
							   uint32_t global_name)
{
	struct registry *registry = container_of(registry_resource, struct registry, resource);
	struct poor_wl_display *display = registry_resource->resource.data;
	struct poor_wl_global *global;

	poor_wl_list_for_each(global, &display->global_list, link)
		if (global->name == global_name && registry_announced(registry, global))
			break;
	if (&global->link == &display->global_list ||
	    (global->removed && registry->acked & (1ull << global->ack_slot))) {
		poor_wl_resource_post_error(&fixes_resource->resource, POOR_WL_FIXES_ERROR_INVALID_ACK_REMOVE,
				       "the given registry did not announce global %u", global_name);
		return;
	}
	if (!global->removed) {
		poor_wl_resource_post_error(&fixes_resource->resource, POOR_WL_FIXES_ERROR_INVALID_ACK_REMOVE,
				       "global %u is not removed", global_name);
		return;
	}
	registry->acked |= 1ull << global->ack_slot;
	global_acked(global);
}
