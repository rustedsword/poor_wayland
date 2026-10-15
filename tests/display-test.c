/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-ring.h"
#include "test-runner.h"
#include "test-server.h"
#include "tests-client-protocol.h"
#include "wayland-client-display.h"
#include "wayland-client.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;

struct client {
	struct wl_display display;
	struct poor_loop *loop;
	struct wl_proxy registry, callback, global, child, spawned, fixes, shm, pool, buffer;
	struct wl_registry *wl_registry;
	struct test_global *test_global;
	uint32_t global_name, extra_name, fixes_name, shm_name, removed_name;
	unsigned events, pongs;
	int error, pong_fd;
	bool error_called, done, synced, echoed;
	char in[16384];
	char out[16384];
	union wl_map_entry objects[256];
	union wl_map_entry server_objects[16];
};

static void watchdog_fire(struct poor_loop *, struct poor_loop_timer *)
{
	fprintf(stderr, "loop timed out\n");
	abort();
}

static struct poor_loop_timer watchdog = POOR_LOOP_TIMER_INIT(watchdog_fire);

static unsigned log_requests, log_events;

static void log_handler(const char *fmt, va_list ap)
{
	char line[8192];

	vsnprintf(line, sizeof(line), fmt, ap);
	if (strstr(line, " -> "))
		log_requests++;
	else if (strstr(line, "] test_global#"))
		log_events++;
	fprintf(stderr, "client: %s", line);
}

static void loop_init(struct poor_loop *loop)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(loop, 64, &params));
	poor_loop_timer_arm(loop, &watchdog, poor_loop_now() + TIMEOUT_NS);
	wl_log_set_handler_client(log_handler);
}

#define run_until(loop, cond) \
	do { \
		while (!(cond)) \
			assert(!poor_loop_run_once(loop)); \
	} while (0)

struct expiry {
	struct poor_loop_timer timer;
	bool expired;
};

static void expire(struct poor_loop *, struct poor_loop_timer *timer)
{
	container_of(timer, struct expiry, timer)->expired = true;
}

/* Let the requests that can complete do so */
static void run_for(struct poor_loop *loop, uint64_t ns)
{
	struct expiry expiry;

	poor_loop_timer_init(&expiry.timer, expire);
	expiry.expired = false;
	poor_loop_timer_arm(loop, &expiry.timer, poor_loop_now() + ns);
	run_until(loop, expiry.expired);
}

static void loop_fini(struct poor_loop *loop)
{
	poor_loop_timer_disarm(&watchdog);
	assert(!poor_loop_drain(loop, poor_loop_now() + TIMEOUT_NS));
	poor_loop_exit(loop);
}

static struct client *client_of(struct wl_display *display)
{
	return container_of(display, struct client, display);
}

static void client_error(struct wl_display *display)
{
	struct client *c = client_of(display);

	assert(!c->error_called);
	c->error_called = true;
	c->error = wl_display_get_error(display);
}

static void client_done(struct wl_display *display)
{
	client_of(display)->done = true;
}

static void registry_global(void *data, struct wl_registry *, uint32_t name, const char *interface, uint32_t)
{
	struct client *c = data;

	if (!strcmp(interface, "test_global"))
		c->global_name = name;
	else if (!strcmp(interface, "test_child"))
		c->extra_name = name;
	else if (!strcmp(interface, "wl_fixes"))
		c->fixes_name = name;
	else if (!strcmp(interface, "wl_shm"))
		c->shm_name = name;
}

static void registry_global_remove(void *data, struct wl_registry *, uint32_t name)
{
	((struct client *)data)->removed_name = name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static struct wl_connection_memory client_memory(struct client *c)
{
	return WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);
}

static void client_init(struct client *c, struct poor_loop *loop)
{
	c->loop = loop;
	c->global_name = c->extra_name = c->fixes_name = c->shm_name = c->removed_name = 0;
	c->events = c->pongs = 0;
	c->error = 0;
	c->pong_fd = -1;
	c->error_called = c->done = c->synced = c->echoed = false;
}

static void client_get_registry(struct client *c)
{
	c->wl_registry = wl_display_get_registry(&c->display, &c->registry);
	assert(c->wl_registry);
	wl_registry_add_listener(c->wl_registry, &registry_listener, c);
}

static void client_connect_fd(struct client *c, struct poor_loop *loop, int fd)
{
	struct wl_connection_memory memory = client_memory(c);

	client_init(c, loop);
	wl_display_connect_to_fd(&c->display, loop, &memory, fd, client_error, client_done);
	client_get_registry(c);
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t)
{
	((struct client *)data)->synced = true;
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
	sync_done,
};

static void client_roundtrip(struct client *c)
{
	struct wl_callback *callback = wl_display_sync(&c->display, &c->callback);

	c->synced = false;
	wl_callback_add_listener(callback, &sync_listener, c);
	run_until(c->loop, c->synced || c->error_called);
}

static void client_bind_global(struct client *c, uint32_t version)
{
	client_roundtrip(c);
	assert(c->global_name);
	c->test_global = wl_registry_bind(c->wl_registry, c->global_name, &test_global_interface, version, &c->global);
	assert(c->test_global);
}

static void client_disconnect(struct client *c)
{
	wl_display_disconnect(&c->display);
	run_until(c->loop, c->done);
}

static void server_stop(struct poor_loop *loop)
{
	test_server_stop();
	run_until(loop, test_server.display_done);
}

static bool is_cloexec_fifo(int fd)
{
	struct stat st;

	return fcntl(fd, F_GETFD) & FD_CLOEXEC && !fstat(fd, &st) && S_ISFIFO(st.st_mode);
}

TEST(connect_to_fd_roundtrip)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_roundtrip(&c);
	assert(c.synced && !c.error_called);
	assert(c.global_name && c.extra_name && c.fixes_name && c.shm_name);
	assert(test_server.clients_created == 1);
	assert(test_server.pid == getpid() && test_server.uid == getuid());
	client_disconnect(&c);
	run_until(&loop, test_server.clients_done == 1);
	server_stop(&loop);
	loop_fini(&loop);
}

static void global_echoed(void *data, struct test_global *global, int32_t i, uint32_t u, wl_fixed_t f, const char *s,
			  const char *ns, struct test_global *o, struct wl_array *a, int32_t fd)
{
	struct client *c = data;

	assert(i == -42 && u == 0xdeadbeef && f == wl_fixed_from_double(1.5));
	assert(!strcmp(s, "hello") && !ns && o == global);
	assert(a->size == 5 && !memcmp(a->data, "array", 5));
	assert(is_cloexec_fifo(fd));
	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->echoed = true;
}

static void global_child_spawned(void *data, struct test_global *, struct test_child *child)
{
	struct client *c = data;

	assert((struct wl_proxy *)child == &c->spawned);
	c->events++;
}

static void global_data(void *data, struct test_global *, struct wl_array *payload)
{
	struct client *c = data;
	const char *p = payload->data;

	for (size_t i = 0; i < payload->size; i++)
		assert(p[i] == (char)i);
	c->events++;
}

static const struct test_global_listener global_listener = {
	global_echoed,
	global_child_spawned,
	global_data,
};

TEST(echo_all_argument_types)
{
	struct poor_loop loop;
	struct client c;
	char array_data[] = "array";
	struct wl_array array = WL_ARRAY_INIT(array_data);
	int fds[2];

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	assert(wl_array_add(&array, 5));
	assert(!pipe2(fds, O_CLOEXEC));
	test_global_echo(c.test_global, -42, 0xdeadbeef, wl_fixed_from_double(1.5), "hello", nullptr, c.test_global, &array,
			 fds[0]);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	run_until(&loop, c.echoed);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static void child_pong(void *data, struct test_child *, int32_t fd)
{
	struct client *c = data;

	assert(is_cloexec_fifo(fd));
	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->pongs++;
}

static const struct test_child_listener child_listener = {
	child_pong,
};

TEST(client_created_object)
{
	struct poor_loop loop;
	struct test_child *child;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	child = test_global_create_child(c.test_global, &c.child);
	assert((struct wl_proxy *)child == &c.child);
	assert(wl_proxy_get_version(&c.child) == 2);
	test_child_add_listener(child, &child_listener, &c);
	test_child_ping(child);
	run_until(&loop, c.pongs == 1);
	assert(test_server.children == 1);
	test_child_destroy(child);
	client_roundtrip(&c);
	assert(test_server.children == 0);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static struct wl_proxy *new_proxy(struct wl_display *display, struct wl_proxy *factory, const struct wl_interface *interface)
{
	struct client *c = client_of(display);

	assert(factory == &c->global && interface == &test_child_interface);
	return &c->spawned;
}

TEST(server_created_object)
{
	struct poor_loop loop;
	struct test_child *child = nullptr;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	wl_display_set_new_proxy(&c.display, new_proxy);
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	test_global_spawn_child(c.test_global);
	run_until(&loop, c.events == 1);
	child = (struct test_child *)&c.spawned;
	assert(wl_proxy_get_id(&c.spawned) >= 0xff000000);
	test_child_add_listener(child, &child_listener, &c);
	test_child_ping(child);
	run_until(&loop, c.pongs == 1);
	test_child_destroy(child);
	client_roundtrip(&c);
	assert(test_server.children == 0);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(server_created_object_without_memory)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	test_global_spawn_child(c.test_global);
	run_until(&loop, c.error_called);
	assert(c.error == ENOMEM && !c.events);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The pong goes to a zombie, which closes its fd: the fd leak check of the runner sees it */
TEST(events_of_destroyed_object_close_fds)
{
	struct poor_loop loop;
	struct test_child *child;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	child = test_global_create_child(c.test_global, &c.child);
	test_child_add_listener(child, &child_listener, &c);
	test_child_ping(child);
	test_child_destroy(child);
	client_roundtrip(&c);
	assert(!c.pongs && !c.error_called);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The display has 1, the registry 2 and the global 3, then a child and a sync callback take 4 and 5 */
TEST(ids_are_reused_after_delete_id)
{
	struct poor_loop loop;
	struct test_child *child;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	for (int i = 0; i < 10; i++) {
		child = test_global_create_child(c.test_global, &c.child);
		assert(wl_proxy_get_id(&c.child) == 4 || wl_proxy_get_id(&c.child) == 5);
		test_child_destroy(child);
		client_roundtrip(&c);
	}
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(protocol_error)
{
	const struct wl_interface *interface;
	struct poor_loop loop;
	struct client c;
	uint32_t id;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_post_error(c.test_global, 42);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO);
	assert(wl_display_get_protocol_error(&c.display, &interface, &id) == 42);
	assert(interface == &test_global_interface && id == wl_proxy_get_id(&c.global));
	run_until(&loop, test_server.clients_done == 1);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(request_of_newer_version)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 1);
	test_global_since_two(c.test_global);
	run_until(&loop, c.error_called);
	assert(c.error == EINVAL && wl_display_get_protocol_error(&c.display, nullptr, nullptr) == WL_DISPLAY_ERROR_INVALID_METHOD);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(bind_of_newer_version)
{
	const struct wl_interface *interface;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 3);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO);
	assert(wl_display_get_protocol_error(&c.display, &interface, nullptr) == WL_DISPLAY_ERROR_INVALID_OBJECT);
	assert(interface == &wl_registry_interface);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(server_destroys_client)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_disconnect(c.test_global);
	run_until(&loop, c.error_called);
	assert(c.error == EPIPE || c.error == ECONNRESET);
	assert(test_server.clients_done == 1);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(server_destroys_display_with_clients)
{
	struct poor_loop loop;
	struct client a, b;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&a, &loop, test_server_connect());
	client_connect_fd(&b, &loop, test_server_connect());
	client_roundtrip(&a);
	client_roundtrip(&b);
	test_server_stop();
	run_until(&loop, test_server.display_done && a.error_called && b.error_called);
	assert(test_server.clients_done == 2);
	client_disconnect(&a);
	client_disconnect(&b);
	loop_fini(&loop);
}

TEST(events_larger_than_client_buffer)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	for (int i = 0; i < 8; i++) {
		test_global_flood(c.test_global, 3, 4000);
		client_roundtrip(&c);
	}
	assert(c.events == 24 && !c.error_called);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The events don't fit the server's buffer for the client before the loop iteration ends */
TEST(server_buffer_overflow_disconnects_client)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_flood(c.test_global, 100, 1000);
	run_until(&loop, c.error_called);
	assert(c.error == EPIPE || c.error == ECONNRESET);
	run_until(&loop, test_server.clients_done == 1);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(client_buffer_overflow_fails_display)
{
	struct poor_loop loop;
	char array_data[4000];
	struct wl_array array = WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	memset(wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	for (int i = 0; i < 5; i++)
		test_global_echo(c.test_global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_until(&loop, c.error_called);
	assert(c.error == E2BIG);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(message_too_large_fails_display)
{
	struct poor_loop loop;
	char array_data[WL_MAX_MESSAGE_SIZE];
	struct wl_array array = WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	memset(wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	test_global_echo(c.test_global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_until(&loop, c.error_called);
	assert(c.error == E2BIG);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The server maps ids 0 to 3 only: the display, the registry and the global take 1 to 3 */
TEST(too_many_objects)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect_small());
	client_bind_global(&c, 2);
	test_global_create_child(c.test_global, &c.child);
	run_until(&loop, c.error_called);
	assert(c.error == EINVAL && wl_display_get_protocol_error(&c.display, nullptr, nullptr) == WL_DISPLAY_ERROR_INVALID_METHOD);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(client_disconnect_destroys_server_resources)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_create_child(c.test_global, &c.child);
	client_roundtrip(&c);
	assert(test_server.children == 1);
	client_disconnect(&c);
	run_until(&loop, test_server.clients_done == 1);
	assert(test_server.children == 0);
	server_stop(&loop);
	loop_fini(&loop);
}

static void connect_by_name(struct client *c, struct poor_loop *loop, const char *name)
{
	struct wl_connection_memory memory = client_memory(c);

	client_init(c, loop);
	assert(!wl_display_connect(&c->display, loop, &memory, name, client_error, client_done));
	client_get_registry(c);
}

static bool path_exists(const char *name, const char *suffix)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s%s", getenv("XDG_RUNTIME_DIR"), name, suffix);
	return !access(path, F_OK);
}

TEST(socket_listen_and_connect)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen("wayland-test");
	run_until(&loop, test_server.socket_ready);
	assert(!strcmp(test_server_socket_name(), "wayland-test"));
	assert(path_exists("wayland-test", "") && path_exists("wayland-test", ".lock"));
	connect_by_name(&c, &loop, "wayland-test");
	client_roundtrip(&c);
	assert(c.synced && c.global_name);
	client_disconnect(&c);
	test_server_close_socket();
	run_until(&loop, test_server.socket_done);
	assert(!test_server.socket_error);
	assert(!path_exists("wayland-test", "") && !path_exists("wayland-test", ".lock"));
	server_stop(&loop);
	loop_fini(&loop);
}

static int hold_lock(const char *name)
{
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "%s/%s.lock", getenv("XDG_RUNTIME_DIR"), name);
	fd = open(path, O_CREAT | O_CLOEXEC | O_RDWR, 0600);
	assert(fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB));
	return fd;
}

static void release_lock(struct poor_loop *loop, const char *name, int fd)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s.lock", getenv("XDG_RUNTIME_DIR"), name);
	assert(!unlink(path));
	poor_loop_close_fd_or_submit(loop, fd, 0);
}

TEST(socket_auto_skips_locked_names)
{
	struct poor_loop loop;
	struct client c;
	int lock;

	loop_init(&loop);
	lock = hold_lock("wayland-0");
	test_server_start(&loop);
	test_server_listen_auto();
	run_until(&loop, test_server.socket_ready);
	assert(!strcmp(test_server_socket_name(), "wayland-1"));
	connect_by_name(&c, &loop, "wayland-1");
	client_roundtrip(&c);
	assert(c.synced);
	client_disconnect(&c);
	server_stop(&loop);
	assert(test_server.socket_done && !path_exists("wayland-1", ""));
	release_lock(&loop, "wayland-0", lock);
	loop_fini(&loop);
}

TEST(socket_locked_by_other_compositor)
{
	struct poor_loop loop;
	int lock;

	loop_init(&loop);
	lock = hold_lock("wayland-busy");
	test_server_start(&loop);
	test_server_listen("wayland-busy");
	run_until(&loop, test_server.socket_done);
	assert(!test_server.socket_ready && test_server.socket_error == EWOULDBLOCK);
	assert(path_exists("wayland-busy", ".lock"));
	server_stop(&loop);
	release_lock(&loop, "wayland-busy", lock);
	loop_fini(&loop);
}

TEST(connect_without_server)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	connect_by_name(&c, &loop, "wayland-nobody");
	run_until(&loop, c.error_called);
	assert(c.error == ENOENT);
	client_disconnect(&c);
	loop_fini(&loop);
}

TEST(global_removal_acknowledged)
{
	struct poor_loop loop;
	struct wl_fixes *fixes;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_roundtrip(&c);
	fixes = wl_registry_bind(c.wl_registry, c.fixes_name, &wl_fixes_interface, 2, &c.fixes);
	test_server_remove_global();
	run_until(&loop, c.removed_name);
	assert(c.removed_name == c.extra_name && !test_server.withdrawn);
	wl_fixes_ack_global_remove(fixes, c.wl_registry, c.removed_name);
	client_roundtrip(&c);
	assert(test_server.withdrawn == 1 && test_server_global_destroyed());
	wl_fixes_ack_global_remove(fixes, c.wl_registry, c.removed_name);
	run_until(&loop, c.error_called);
	assert(wl_display_get_protocol_error(&c.display, nullptr, nullptr) == WL_FIXES_ERROR_INVALID_ACK_REMOVE);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(global_removal_acknowledged_by_registry_destruction)
{
	struct poor_loop loop;
	struct wl_fixes *fixes;
	struct client a, b;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&a, &loop, test_server_connect());
	client_connect_fd(&b, &loop, test_server_connect());
	client_roundtrip(&a);
	client_roundtrip(&b);
	fixes = wl_registry_bind(a.wl_registry, a.fixes_name, &wl_fixes_interface, 2, &a.fixes);
	test_server_remove_global();
	run_until(&loop, a.removed_name && b.removed_name);
	wl_fixes_destroy_registry(fixes, a.wl_registry);
	wl_registry_destroy(a.wl_registry);
	client_roundtrip(&a);
	assert(!test_server.withdrawn);
	client_disconnect(&b);
	run_until(&loop, test_server.withdrawn == 1);
	client_disconnect(&a);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(registry_after_removal_does_not_announce)
{
	struct poor_loop loop;
	struct client a, b;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&a, &loop, test_server_connect());
	client_roundtrip(&a);
	test_server_remove_global();
	run_until(&loop, a.removed_name);
	client_connect_fd(&b, &loop, test_server_connect());
	client_roundtrip(&b);
	assert(b.global_name && !b.extra_name);
	client_disconnect(&b);
	client_disconnect(&a);
	run_until(&loop, test_server.withdrawn == 1);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(shm_buffer)
{
	constexpr uint32_t pixel = 0x11223344;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	struct poor_loop loop;
	struct wl_shm *shm;
	struct client c;
	uint32_t *data;
	int fd;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	shm = wl_registry_bind(c.wl_registry, c.shm_name, &wl_shm_interface, 1, &c.shm);
	fd = memfd_create("shm-test", MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, 4096));
	data = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(data != MAP_FAILED);
	data[0] = pixel;
	pool = wl_shm_create_pool(shm, &c.pool, fd, 4096);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	buffer = wl_shm_pool_create_buffer(pool, &c.buffer, 0, 16, 16, 64, WL_SHM_FORMAT_XRGB8888);
	test_global_read_buffer(c.test_global, buffer);
	client_roundtrip(&c);
	assert(!c.error_called && test_server.shm_buffers == 1 && test_server.shm_pixel == pixel);
	wl_buffer_destroy(buffer);
	wl_shm_pool_destroy(pool);
	client_roundtrip(&c);
	munmap(data, 4096);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(requests_before_connect_are_sent)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen("wayland-early");
	run_until(&loop, test_server.socket_ready);
	connect_by_name(&c, &loop, "wayland-early");
	client_bind_global(&c, 2);
	client_roundtrip(&c);
	assert(!c.error_called && c.synced);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(protocol_logger)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_log_messages();
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	test_global_flood(c.test_global, 2, 8);
	client_roundtrip(&c);
	assert(c.events == 2);
	/* get_registry, sync twice, bind and flood; globals, done twice, delete_id twice and data twice */
	assert(test_server.logged_requests == 5);
	assert(test_server.logged_events >= 2 + 2 + 2 + 2);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(global_filter)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_hide_extra();
	client_connect_fd(&c, &loop, test_server_connect());
	client_roundtrip(&c);
	assert(c.global_name && !c.extra_name);
	wl_registry_bind(c.wl_registry, c.global_name + 1, &test_child_interface, 1, &c.child);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO && wl_display_get_protocol_error(&c.display, nullptr, nullptr) == WL_DISPLAY_ERROR_INVALID_OBJECT);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(debug_output)
{
	struct poor_loop loop;
	struct client c;

	setenv("WAYLAND_DEBUG", "client", 1);
	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &global_listener, &c);
	test_global_flood(c.test_global, 3, 4);
	client_roundtrip(&c);
	assert(log_requests == 5 && log_events == 3);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The checker runs after exec and fails if it inherits more fds than the test started with */
TEST(all_fds_are_cloexec)
{
	struct poor_loop loop;
	struct test_child *child;
	int nr_fds = count_open_fds(), status;
	struct client c;
	pid_t pid;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen("wayland-cloexec");
	run_until(&loop, test_server.socket_ready);
	connect_by_name(&c, &loop, "wayland-cloexec");
	client_bind_global(&c, 2);
	child = test_global_create_child(c.test_global, &c.child);
	test_child_add_listener(child, &child_listener, &c);
	test_child_ping(child);
	run_until(&loop, c.pongs == 1);
	pid = fork();
	assert(pid >= 0);
	if (!pid)
		exec_fd_leak_check(nr_fds);
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static void echoed_then_disconnect(void *data, struct test_global *, int32_t, uint32_t, wl_fixed_t, const char *, const char *,
				   struct test_global *, struct wl_array *, int32_t fd)
{
	struct client *c = data;

	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->echoed = true;
	wl_display_disconnect(&c->display);
}

static const struct test_global_listener disconnecting_listener = {
	echoed_then_disconnect,
	global_child_spawned,
	global_data,
};

/* The second echo is not dispatched, the display is gone */
TEST(disconnect_in_event_listener)
{
	struct poor_loop loop;
	char array_data[1];
	struct wl_array array = WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	test_global_add_listener(c.test_global, &disconnecting_listener, &c);
	test_global_echo(c.test_global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	test_global_echo(c.test_global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_until(&loop, c.done);
	assert(c.echoed && !c.error_called);
	run_until(&loop, test_server.clients_done == 1);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(socket_invalid_names)
{
	struct poor_loop loop;
	char name[200];
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	struct client c;
	struct wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	memset(name, 'a', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	assert(wl_display_connect(&c.display, &loop, &memory, name, client_error, client_done) == -1 && errno == ENAMETOOLONG);
	unsetenv("XDG_RUNTIME_DIR");
	assert(wl_display_connect(&c.display, &loop, &memory, "wayland-0", client_error, client_done) == -1 && errno == ENOENT);
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	loop_fini(&loop);
}

TEST(socket_absolute_path)
{
	struct poor_loop loop;
	char path[256];
	struct client c;

	snprintf(path, sizeof(path), "%s/absolute-socket", getenv("XDG_RUNTIME_DIR"));
	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen(path);
	run_until(&loop, test_server.socket_ready);
	assert(!strcmp(test_server_socket_name(), path));
	connect_by_name(&c, &loop, path);
	client_roundtrip(&c);
	assert(c.synced);
	client_disconnect(&c);
	server_stop(&loop);
	assert(access(path, F_OK));
	loop_fini(&loop);
}

TEST(socket_adopted_fd)
{
	struct sockaddr_un addr = { .sun_family = AF_LOCAL };
	struct poor_loop loop;
	struct client c;
	int fd;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/adopted", getenv("XDG_RUNTIME_DIR"));
	fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	assert(fd >= 0 && !bind(fd, (struct sockaddr *)&addr, sizeof(addr)) && !listen(fd, 1));
	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen_fd(fd);
	connect_by_name(&c, &loop, addr.sun_path);
	client_roundtrip(&c);
	assert(c.synced);
	client_disconnect(&c);
	server_stop(&loop);
	assert(test_server.socket_done && !test_server.socket_ready);
	assert(!unlink(addr.sun_path));
	loop_fini(&loop);
}

TEST(wayland_socket_environment)
{
	struct poor_loop loop;
	char fd_string[16];
	struct client c;
	struct wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	test_server_start(&loop);
	snprintf(fd_string, sizeof(fd_string), "%d", test_server_connect());
	setenv("WAYLAND_SOCKET", fd_string, 1);
	client_init(&c, &loop);
	assert(!wl_display_connect(&c.display, &loop, &memory, "wayland-ignored", client_error, client_done));
	assert(!getenv("WAYLAND_SOCKET"));
	assert(wl_display_get_fd(&c.display) == atoi(fd_string));
	client_get_registry(&c);
	client_roundtrip(&c);
	assert(c.synced && c.global_name);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static int dispatcher(const void *implementation, void *target, uint32_t opcode, const struct wl_message *message,
		      union wl_argument *args)
{
	struct client *c = (void *)implementation;

	assert(target == &c->global && message == &test_global_interface.events[opcode] && !strcmp(message->name, "data"));
	assert(args[0].a->size == 3);
	c->events++;
	return 0;
}

TEST(proxy_dispatcher)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	assert(!wl_proxy_add_dispatcher(&c.global, dispatcher, &c, nullptr));
	assert(wl_proxy_add_listener(&c.global, (void (**)(void))&global_listener, &c) == -1);
	test_global_flood(c.test_global, 2, 3);
	assert(!wl_display_flush(&c.display));
	client_roundtrip(&c);
	assert(c.events == 2);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static void write_words(int fd, const uint32_t *words, size_t count)
{
	assert(write(fd, words, count * sizeof(*words)) == (ssize_t)(count * sizeof(*words)));
}

/* The client never reads, so the server's send waits for room until the destruction cancels it */
TEST(destroy_client_with_blocked_send)
{
	static const uint32_t requests[] = {
		1, 12 << 16 | 1, 2,
		2, 36 << 16 | 0, 1, 12, 0x74736574, 0x6f6c675f, 0x006c6162, 1, 3,
		3, 16 << 16 | 5, 10, 1000,
	};
	struct poor_loop loop;
	int fd;

	loop_init(&loop);
	test_server_start(&loop);
	fd = test_server_connect_sndbuf(4096);
	write_words(fd, requests, ARRAY_SIZE(requests));
	run_for(&loop, 100'000'000);
	assert(!test_server.clients_done && test_server_sending());
	test_server_destroy_clients();
	run_until(&loop, test_server.clients_done == 1);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	server_stop(&loop);
	loop_fini(&loop);
}

/* Nobody reads the requests, so the display's send waits for room until the disconnect cancels it */
TEST(disconnect_with_blocked_send)
{
	struct poor_loop loop;
	char array_data[4000];
	struct wl_array array = WL_ARRAY_INIT(array_data);
	struct client c;
	int fds[2], sndbuf = 4096;

	loop_init(&loop);
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	assert(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
	client_connect_fd(&c, &loop, fds[0]);
	memset(wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	c.test_global = wl_registry_bind(c.wl_registry, 1, &test_global_interface, 1, &c.global);
	for (int i = 0; i < 3; i++)
		test_global_echo(c.test_global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_for(&loop, 100'000'000);
	assert(c.display.send_op.pending && !c.error_called);
	client_disconnect(&c);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	loop_fini(&loop);
}
