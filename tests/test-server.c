/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test-server.h"
#include "tests-server-protocol.h"
#include "wayland-server-display.h"
#include "wayland-server.h"

constexpr unsigned MAX_CLIENTS = 8;

struct test_client {
	struct wl_client client;
	bool used;
	char in[16384];
	char out[16384];
	union wl_map_entry objects[256];
	union wl_map_entry server_objects[16];
};

struct object {
	struct wl_resource resource;
};

struct test_server_state test_server;

static struct {
	struct poor_loop *loop;
	struct wl_display display;
	struct wl_global global, extra, fixes, shm;
	struct wl_socket socket;
	struct wl_listener client_created;
	struct wl_protocol_logger logger;
	struct test_client clients[MAX_CLIENTS];
	bool extra_destroyed;
} server;

static void log_handler(const char *fmt, va_list ap)
{
	fprintf(stderr, "server: ");
	vfprintf(stderr, fmt, ap);
}

static void *server_alloc(struct wl_display *, struct wl_client *, size_t size)
{
	return malloc(size);
}

static void server_free(struct wl_display *, void *ptr)
{
	free(ptr);
}

static void object_destroy(struct wl_resource *resource)
{
	free(container_of(resource, struct object, resource));
}

static struct wl_resource *object_new(struct wl_client *client, const struct wl_interface *interface, int version, uint32_t id,
				      const void *implementation, wl_resource_destroy_func_t destroy)
{
	struct object *object = malloc(sizeof(*object));

	assert(object);
	if (wl_resource_init(&object->resource, client, interface, version, id) < 0) {
		free(object);
		wl_client_post_no_memory(client);
		return nullptr;
	}
	wl_resource_set_implementation(&object->resource, implementation, nullptr, destroy);
	return &object->resource;
}

static void destroy_request(struct wl_client *, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void child_ping(struct wl_client *, struct wl_resource *resource)
{
	int fds[2];

	assert(!pipe2(fds, O_CLOEXEC));
	test_child_send_pong(resource, fds[0]);
	poor_loop_close_fd_or_submit(server.loop, fds[0], 0);
	poor_loop_close_fd_or_submit(server.loop, fds[1], 0);
}

static const struct test_child_interface child_impl = {
	destroy_request,
	child_ping,
};

static void child_destroy(struct wl_resource *resource)
{
	test_server.children--;
	object_destroy(resource);
}

static struct wl_resource *child_new(struct wl_client *client, int version, uint32_t id)
{
	struct wl_resource *child = object_new(client, &test_child_interface, version, id, &child_impl, child_destroy);

	if (child)
		test_server.children++;
	return child;
}

static void global_echo(struct wl_client *, struct wl_resource *resource, int32_t i, uint32_t u, wl_fixed_t f, const char *s,
			const char *ns, struct wl_resource *o, struct wl_array *a, int32_t fd)
{
	test_global_send_echoed(resource, i, u, f, s, ns, o, a, fd);
	poor_loop_close_fd_or_submit(server.loop, fd, 0);
}

static void global_create_child(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	child_new(client, wl_resource_get_version(resource), id);
}

static void global_spawn_child(struct wl_client *client, struct wl_resource *resource)
{
	struct wl_resource *child = child_new(client, wl_resource_get_version(resource), 0);

	if (child)
		test_global_send_child_spawned(resource, child);
}

static void global_post_error(struct wl_client *, struct wl_resource *resource, uint32_t code)
{
	wl_resource_post_error(resource, code, "test error %u", code);
}

static void global_flood(struct wl_client *, struct wl_resource *resource, uint32_t count, uint32_t size)
{
	char payload[WL_MAX_MESSAGE_SIZE];
	struct wl_array array = WL_ARRAY_INIT(payload);
	char *p = wl_array_add(&array, size);

	assert(p);
	for (uint32_t i = 0; i < size; i++)
		p[i] = (char)i;
	for (uint32_t i = 0; i < count; i++)
		test_global_send_data(resource, &array);
}

static void global_disconnect(struct wl_client *client, struct wl_resource *)
{
	wl_client_destroy(client);
}

static void global_read_buffer(struct wl_client *, struct wl_resource *, struct wl_resource *resource)
{
	struct wl_shm_buffer *buffer = wl_shm_buffer_get(resource);

	assert(buffer);
	wl_shm_buffer_begin_access(buffer);
	test_server.shm_pixel = *(uint32_t *)wl_shm_buffer_get_data(buffer);
	wl_shm_buffer_end_access(buffer);
	test_server.shm_buffers++;
}

static void global_since_two(struct wl_client *, struct wl_resource *)
{
}

static const struct test_global_interface global_impl = {
	destroy_request,
	global_echo,
	global_create_child,
	global_spawn_child,
	global_post_error,
	global_flood,
	global_disconnect,
	global_read_buffer,
	global_since_two,
};

static void bind_global(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
	object_new(client, &test_global_interface, version, id, &global_impl, object_destroy);
}

static void bind_extra(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
	child_new(client, version, id);
}

static void extra_withdrawn(struct wl_global *global)
{
	test_server.withdrawn++;
	wl_global_destroy(global);
	server.extra_destroyed = true;
}

static void fixes_destroy_registry(struct wl_client *, struct wl_resource *, struct wl_resource *registry)
{
	wl_resource_destroy(registry);
}

static void fixes_ack_global_remove(struct wl_client *, struct wl_resource *resource, struct wl_resource *registry,
				    uint32_t name)
{
	wl_fixes_handle_ack_global_remove(resource, registry, name);
}

static const struct wl_fixes_interface fixes_impl = {
	destroy_request,
	fixes_destroy_registry,
	fixes_ack_global_remove,
};

static void bind_fixes(struct wl_client *client, void *, uint32_t version, uint32_t id)
{
	object_new(client, &wl_fixes_interface, version, id, &fixes_impl, object_destroy);
}

static void client_done(struct wl_client *client)
{
	container_of(client, struct test_client, client)->used = false;
	test_server.clients_done++;
}

static void client_created(struct wl_listener *, void *data)
{
	test_server.clients_created++;
	wl_client_get_credentials(data, &test_server.pid, &test_server.uid, nullptr);
}

static void client_start(int fd, bool small)
{
	struct test_client *c = nullptr;
	struct wl_connection_memory memory;

	for (unsigned i = 0; i < MAX_CLIENTS && !c; i++)
		if (!server.clients[i].used)
			c = &server.clients[i];
	assert(c);
	c->used = true;
	memory = WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);
	if (small) {
		memory.in_size = memory.out_size = WL_MAX_MESSAGE_SIZE;
		memory.client_objects_size = 4;
		memory.server_objects_size = 2;
	}
	wl_client_init(&c->client, &server.display, fd, &memory, client_done);
}

static int connect_client(bool small, int sndbuf)
{
	int fds[2];

	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	if (sndbuf)
		assert(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
	client_start(fds[0], small);
	return fds[1];
}

int test_server_connect(void)
{
	return connect_client(false, 0);
}

int test_server_connect_small(void)
{
	return connect_client(true, 0);
}

int test_server_connect_sndbuf(int sndbuf)
{
	return connect_client(false, sndbuf);
}

static void socket_ready(struct wl_socket *)
{
	test_server.socket_ready = true;
}

static void socket_accept(struct wl_socket *, int fd)
{
	client_start(fd, false);
}

static void socket_done(struct wl_socket *socket)
{
	test_server.socket_done = true;
	test_server.socket_error = socket->error;
}

void test_server_listen(const char *name)
{
	wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	assert(!wl_socket_listen(&server.socket, name));
}

void test_server_listen_auto(void)
{
	wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	assert(!wl_socket_listen_auto(&server.socket));
}

const char *test_server_socket_name(void)
{
	return wl_socket_get_name(&server.socket);
}

void test_server_close_socket(void)
{
	wl_socket_destroy(&server.socket);
}

static void display_done(struct wl_display *)
{
	test_server.display_done = true;
}

void test_server_start(struct poor_loop *loop)
{
	wl_log_set_handler_server(log_handler);
	server.loop = loop;
	wl_display_init(&server.display, loop, display_done);
	wl_display_set_allocator(&server.display, server_alloc, server_free);
	server.client_created.notify = client_created;
	wl_display_add_client_created_listener(&server.display, &server.client_created);
	wl_global_init(&server.global, &server.display, &test_global_interface, 2, nullptr, bind_global);
	wl_global_init(&server.extra, &server.display, &test_child_interface, 1, nullptr, bind_extra);
	wl_global_set_withdrawn_listener(&server.extra, extra_withdrawn);
	wl_global_init(&server.fixes, &server.display, &wl_fixes_interface, 2, nullptr, bind_fixes);
	wl_display_init_shm(&server.display, &server.shm);
}

void test_server_stop(void)
{
	wl_display_destroy(&server.display);
}

void test_server_remove_global(void)
{
	wl_global_remove(&server.extra);
}

bool test_server_global_destroyed(void)
{
	return server.extra_destroyed;
}

static void logger(void *, enum wl_protocol_logger_type type, const struct wl_protocol_logger_message *message)
{
	assert(message->resource && message->message);
	if (type == WL_PROTOCOL_LOGGER_REQUEST)
		test_server.logged_requests++;
	else
		test_server.logged_events++;
}

void test_server_log_messages(void)
{
	wl_display_add_protocol_logger(&server.display, &server.logger, logger, nullptr);
}

static bool hide_extra(const struct wl_client *, const struct wl_global *global, void *)
{
	return global != &server.extra;
}

void test_server_hide_extra(void)
{
	wl_display_set_global_filter(&server.display, hide_extra, nullptr);
}

void test_server_listen_fd(int fd)
{
	wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	wl_socket_listen_fd(&server.socket, fd);
}

bool test_server_sending(void)
{
	for (unsigned i = 0; i < MAX_CLIENTS; i++)
		if (server.clients[i].used && server.clients[i].client.send_op.pending)
			return true;
	return false;
}

void test_server_destroy_clients(void)
{
	wl_display_destroy_clients(&server.display);
}
