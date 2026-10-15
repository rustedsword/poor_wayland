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
#include "poor_wayland_server_display.h"
#include "poor_wayland_server.h"

constexpr unsigned MAX_CLIENTS = 8;

struct test_client {
	struct poor_wl_client client;
	bool used;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[256];
	union poor_wl_map_entry server_objects[16];
};

struct test_server_state test_server;

static struct {
	struct poor_loop *loop;
	struct poor_wl_display display;
	struct poor_wl_global global, extra, fixes, shm;
	struct poor_wl_socket socket;
	struct poor_wl_listener client_created;
	struct poor_wl_protocol_logger logger;
	struct test_client clients[MAX_CLIENTS];
	bool extra_destroyed;
} server;

static void log_handler(const char *fmt, va_list ap)
{
	fprintf(stderr, "server: ");
	vfprintf(stderr, fmt, ap);
}

static void *server_alloc(struct poor_wl_display *, struct poor_wl_client *, size_t size)
{
	return malloc(size);
}

static void server_free(struct poor_wl_display *, void *ptr)
{
	free(ptr);
}

static void child_destroy_request(struct poor_wl_client *, struct poor_test_child_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void child_ping(struct poor_wl_client *, struct poor_test_child_resource *resource)
{
	int fds[2];

	assert(!pipe2(fds, O_CLOEXEC));
	poor_test_child_send_pong(resource, fds[0]);
	poor_loop_close_fd_or_submit(server.loop, fds[0], 0);
	poor_loop_close_fd_or_submit(server.loop, fds[1], 0);
}

static const struct poor_test_child_interface child_impl = {
	child_destroy_request,
	child_ping,
};

static void child_destroy(struct poor_wl_resource *resource)
{
	test_server.children--;
	free(container_of(resource, struct poor_test_child_resource, resource));
}

static struct poor_test_child_resource *child_new(struct poor_wl_client *client, int version, uint32_t id)
{
	struct poor_test_child_resource *child = malloc(sizeof(*child));

	assert(child);
	if (poor_test_child_resource_init(child, client, version, id) < 0) {
		free(child);
		poor_wl_client_post_no_memory(client);
		return nullptr;
	}
	poor_test_child_resource_set_implementation(child, &child_impl, nullptr, child_destroy);
	test_server.children++;
	return child;
}

static void global_destroy_request(struct poor_wl_client *, struct poor_test_global_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void global_echo(struct poor_wl_client *, struct poor_test_global_resource *resource, int32_t i, uint32_t u,
			poor_wl_fixed_t f, const char *s, const char *ns, struct poor_test_global_resource *o,
			struct poor_wl_array *a, int32_t fd)
{
	poor_test_global_send_echoed(resource, i, u, f, s, ns, o, a, fd);
	poor_loop_close_fd_or_submit(server.loop, fd, 0);
}

static void global_create_child(struct poor_wl_client *client, struct poor_test_global_resource *resource, uint32_t id)
{
	child_new(client, poor_wl_resource_get_version(&resource->resource), id);
}

static void global_spawn_child(struct poor_wl_client *client, struct poor_test_global_resource *resource)
{
	struct poor_test_child_resource *child = child_new(client, poor_wl_resource_get_version(&resource->resource), 0);

	if (child)
		poor_test_global_send_child_spawned(resource, child);
}

static void global_post_error(struct poor_wl_client *, struct poor_test_global_resource *resource, uint32_t code)
{
	poor_wl_resource_post_error(&resource->resource, code, "test error %u", code);
}

static void global_flood(struct poor_wl_client *, struct poor_test_global_resource *resource, uint32_t count, uint32_t size)
{
	char payload[POOR_WL_MAX_MESSAGE_SIZE];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(payload);
	char *p = poor_wl_array_add(&array, size);

	assert(p);
	for (uint32_t i = 0; i < size; i++)
		p[i] = (char)i;
	for (uint32_t i = 0; i < count; i++)
		poor_test_global_send_data(resource, &array);
}

static void global_disconnect(struct poor_wl_client *client, struct poor_test_global_resource *)
{
	poor_wl_client_destroy(client);
}

static void global_read_buffer(struct poor_wl_client *, struct poor_test_global_resource *,
			       struct poor_wl_buffer_resource *resource)
{
	struct poor_wl_shm_buffer *buffer = poor_wl_shm_buffer_get(resource);

	assert(buffer);
	poor_wl_shm_buffer_begin_access(buffer);
	test_server.shm_pixel = *(uint32_t *)poor_wl_shm_buffer_get_data(buffer);
	poor_wl_shm_buffer_end_access(buffer);
	test_server.shm_buffers++;
}

static void global_since_two(struct poor_wl_client *, struct poor_test_global_resource *)
{
}

static const struct poor_test_global_interface global_impl = {
	global_destroy_request,
	global_echo,
	global_create_child,
	global_spawn_child,
	global_post_error,
	global_flood,
	global_disconnect,
	global_read_buffer,
	global_since_two,
};

static void global_destroy(struct poor_wl_resource *resource)
{
	free(container_of(resource, struct poor_test_global_resource, resource));
}

static void bind_global(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	struct poor_test_global_resource *global = malloc(sizeof(*global));

	assert(global);
	if (poor_test_global_resource_init(global, client, version, id) < 0) {
		free(global);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_test_global_resource_set_implementation(global, &global_impl, nullptr, global_destroy);
}

static void bind_extra(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	child_new(client, version, id);
}

static void extra_withdrawn(struct poor_wl_global *global)
{
	test_server.withdrawn++;
	poor_wl_global_destroy(global);
	server.extra_destroyed = true;
}

static void fixes_destroy_request(struct poor_wl_client *, struct poor_wl_fixes_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void fixes_destroy_registry(struct poor_wl_client *, struct poor_wl_fixes_resource *,
				   struct poor_wl_registry_resource *registry)
{
	poor_wl_resource_destroy(&registry->resource);
}

static void fixes_ack_global_remove(struct poor_wl_client *, struct poor_wl_fixes_resource *resource,
				    struct poor_wl_registry_resource *registry, uint32_t name)
{
	poor_wl_fixes_handle_ack_global_remove(resource, registry, name);
}

static const struct poor_wl_fixes_interface fixes_impl = {
	fixes_destroy_request,
	fixes_destroy_registry,
	fixes_ack_global_remove,
};

static void fixes_destroy(struct poor_wl_resource *resource)
{
	free(container_of(resource, struct poor_wl_fixes_resource, resource));
}

static void bind_fixes(struct poor_wl_client *client, void *, uint32_t version, uint32_t id)
{
	struct poor_wl_fixes_resource *fixes = malloc(sizeof(*fixes));

	assert(fixes);
	if (poor_wl_fixes_resource_init(fixes, client, version, id) < 0) {
		free(fixes);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_fixes_resource_set_implementation(fixes, &fixes_impl, nullptr, fixes_destroy);
}

static void client_done(struct poor_wl_client *client)
{
	container_of(client, struct test_client, client)->used = false;
	test_server.clients_done++;
}

static void client_created(struct poor_wl_listener *, void *data)
{
	test_server.clients_created++;
	poor_wl_client_get_credentials(data, &test_server.pid, &test_server.uid, nullptr);
}

static void client_start(int fd, bool small)
{
	struct test_client *c = nullptr;
	struct poor_wl_connection_memory memory;

	for (unsigned i = 0; i < MAX_CLIENTS && !c; i++)
		if (!server.clients[i].used)
			c = &server.clients[i];
	assert(c);
	c->used = true;
	memory = POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);
	if (small) {
		memory.in_size = memory.out_size = POOR_WL_MAX_MESSAGE_SIZE;
		memory.client_objects_size = 4;
		memory.server_objects_size = 2;
	}
	poor_wl_client_init(&c->client, &server.display, fd, &memory, client_done);
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

static void socket_ready(struct poor_wl_socket *)
{
	test_server.socket_ready = true;
}

static void socket_accept(struct poor_wl_socket *, int fd)
{
	client_start(fd, false);
}

static void socket_done(struct poor_wl_socket *socket)
{
	test_server.socket_done = true;
	test_server.socket_error = socket->error;
}

void test_server_listen(const char *name)
{
	poor_wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	assert(!poor_wl_socket_listen(&server.socket, name));
}

void test_server_listen_auto(void)
{
	poor_wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	assert(!poor_wl_socket_listen_auto(&server.socket));
}

const char *test_server_socket_name(void)
{
	return poor_wl_socket_get_name(&server.socket);
}

void test_server_close_socket(void)
{
	poor_wl_socket_destroy(&server.socket);
}

static void display_done(struct poor_wl_display *)
{
	test_server.display_done = true;
}

void test_server_start(struct poor_loop *loop)
{
	poor_wl_log_set_handler_server(log_handler);
	server.loop = loop;
	poor_wl_display_init(&server.display, loop, display_done);
	poor_wl_display_set_allocator(&server.display, server_alloc, server_free);
	server.client_created.notify = client_created;
	poor_wl_display_add_client_created_listener(&server.display, &server.client_created);
	poor_wl_global_init(&server.global, &server.display, &poor_test_global_interface, 2, nullptr, bind_global);
	poor_wl_global_init(&server.extra, &server.display, &poor_test_child_interface, 1, nullptr, bind_extra);
	poor_wl_global_set_withdrawn_listener(&server.extra, extra_withdrawn);
	poor_wl_global_init(&server.fixes, &server.display, &poor_wl_fixes_interface, 2, nullptr, bind_fixes);
	poor_wl_display_init_shm(&server.display, &server.shm);
}

void test_server_stop(void)
{
	poor_wl_display_destroy(&server.display);
}

void test_server_remove_global(void)
{
	poor_wl_global_remove(&server.extra);
}

bool test_server_global_destroyed(void)
{
	return server.extra_destroyed;
}

static void logger(void *, enum poor_wl_protocol_logger_type type, const struct poor_wl_protocol_logger_message *message)
{
	assert(message->resource && message->message);
	if (type == POOR_WL_PROTOCOL_LOGGER_REQUEST)
		test_server.logged_requests++;
	else
		test_server.logged_events++;
}

void test_server_log_messages(void)
{
	poor_wl_display_add_protocol_logger(&server.display, &server.logger, logger, nullptr);
}

static bool hide_extra(const struct poor_wl_client *, const struct poor_wl_global *global, void *)
{
	return global != &server.extra;
}

void test_server_hide_extra(void)
{
	poor_wl_display_set_global_filter(&server.display, hide_extra, nullptr);
}

void test_server_listen_fd(int fd)
{
	poor_wl_socket_init(&server.socket, &server.display, socket_ready, socket_accept, socket_done);
	poor_wl_socket_listen_fd(&server.socket, fd);
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
	poor_wl_display_destroy_clients(&server.display);
}
