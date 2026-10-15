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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test-ring.h"
#include "test-runner.h"
#include "test-server.h"
#include "tests-client-protocol.h"
#include "poor_wayland_client_display.h"
#include "poor_wayland_client.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;

struct client {
	struct poor_wl_display display;
	struct poor_loop *loop;
	struct poor_wl_registry registry;
	struct poor_wl_callback callback;
	struct poor_test_global global;
	struct poor_test_child child, spawned;
	struct poor_wl_fixes fixes;
	struct poor_wl_shm shm;
	struct poor_wl_shm_pool pool;
	struct poor_wl_buffer buffer;
	uint32_t global_name, extra_name, fixes_name, shm_name, removed_name;
	unsigned events, pongs;
	int error, pong_fd;
	bool error_called, done, synced, echoed;
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[256];
	union poor_wl_map_entry server_objects[16];
};

static void watchdog_fire(struct poor_loop *, struct poor_loop_timer *)
{
	fprintf(stderr, "loop timed out\n");
	abort();
}

static struct poor_loop_timer watchdog = POOR_LOOP_TIMER_INIT(watchdog_fire);

static unsigned log_requests, log_events;
static char log_text[65536];
static size_t log_len;

static void log_handler(const char *fmt, va_list ap)
{
	char line[8192];
	size_t len;

	vsnprintf(line, sizeof(line), fmt, ap);
	if (strstr(line, " -> "))
		log_requests++;
	else if (strstr(line, "] test_global#"))
		log_events++;
	fprintf(stderr, "client: %s", line);
	len = strlen(line);
	if (log_len + len < sizeof(log_text)) {
		memcpy(log_text + log_len, line, len + 1);
		log_len += len;
	}
}

static bool logged(const char *text)
{
	return strstr(log_text, text);
}

static void loop_init(struct poor_loop *loop)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(loop, 64, &params));
	poor_loop_timer_arm(loop, &watchdog, poor_loop_now() + TIMEOUT_NS);
	poor_wl_log_set_handler_client(log_handler);
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

static struct client *client_of(struct poor_wl_display *display)
{
	return container_of(display, struct client, display);
}

static void client_error(struct poor_wl_display *display)
{
	struct client *c = client_of(display);

	assert(!c->error_called);
	c->error_called = true;
	c->error = poor_wl_display_get_error(display);
}

static void client_done(struct poor_wl_display *display)
{
	client_of(display)->done = true;
}

static void registry_global(void *data, struct poor_wl_registry *, uint32_t name, const char *interface, uint32_t)
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

static void registry_global_remove(void *data, struct poor_wl_registry *, uint32_t name)
{
	((struct client *)data)->removed_name = name;
}

static const struct poor_wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static struct poor_wl_connection_memory client_memory(struct client *c)
{
	return POOR_WL_CONNECTION_MEMORY(c->in, c->out, c->objects, c->server_objects);
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
	assert(poor_wl_display_get_registry(&c->display, &c->registry));
	poor_wl_registry_add_listener(&c->registry, &registry_listener, c);
}

static void client_connect_fd(struct client *c, struct poor_loop *loop, int fd)
{
	struct poor_wl_connection_memory memory = client_memory(c);

	client_init(c, loop);
	poor_wl_display_connect_to_fd(&c->display, loop, &memory, fd, client_error, client_done);
	client_get_registry(c);
}

static void sync_done(void *data, struct poor_wl_callback *callback, uint32_t)
{
	((struct client *)data)->synced = true;
	poor_wl_callback_destroy(callback);
}

static const struct poor_wl_callback_listener sync_listener = {
	sync_done,
};

static void client_roundtrip(struct client *c)
{
	struct poor_wl_callback *callback = poor_wl_display_sync(&c->display, &c->callback);

	c->synced = false;
	poor_wl_callback_add_listener(callback, &sync_listener, c);
	run_until(c->loop, c->synced || c->error_called);
}

static void client_bind_global(struct client *c, uint32_t version)
{
	client_roundtrip(c);
	assert(c->global_name);
	assert(poor_wl_registry_bind(&c->registry, c->global_name, &poor_test_global_interface, version, &c->global));
}

static void client_disconnect(struct client *c)
{
	poor_wl_display_disconnect(&c->display);
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

static void global_echoed(void *data, struct poor_test_global *global, int32_t i, uint32_t u, poor_wl_fixed_t f, const char *s,
			  const char *ns, struct poor_test_global *o, struct poor_wl_array *a, int32_t fd)
{
	struct client *c = data;

	assert(i == -42 && u == 0xdeadbeef && f == poor_wl_fixed_from_double(1.5));
	assert(!strcmp(s, "hello") && !ns && o == global);
	assert(a->size == 5 && !memcmp(a->data, "array", 5));
	assert(is_cloexec_fifo(fd));
	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->echoed = true;
}

static void global_child_spawned(void *data, struct poor_test_global *, struct poor_test_child *child)
{
	struct client *c = data;

	assert(child == &c->spawned);
	c->events++;
}

static void global_data(void *data, struct poor_test_global *, struct poor_wl_array *payload)
{
	struct client *c = data;
	const char *p = payload->data;

	for (size_t i = 0; i < payload->size; i++)
		assert(p[i] == (char)i);
	c->events++;
}

static const struct poor_test_global_listener global_listener = {
	global_echoed,
	global_child_spawned,
	global_data,
};

TEST(echo_all_argument_types)
{
	struct poor_loop loop;
	struct client c;
	char array_data[] = "array";
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	int fds[2];

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	assert(poor_wl_array_add(&array, 5));
	assert(!pipe2(fds, O_CLOEXEC));
	poor_test_global_echo(&c.global, -42, 0xdeadbeef, poor_wl_fixed_from_double(1.5), "hello", nullptr, &c.global, &array,
			 fds[0]);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	run_until(&loop, c.echoed);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static void child_pong(void *data, struct poor_test_child *, int32_t fd)
{
	struct client *c = data;

	assert(is_cloexec_fifo(fd));
	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->pongs++;
}

static const struct poor_test_child_listener child_listener = {
	child_pong,
};

TEST(client_created_object)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	assert(poor_test_global_create_child(&c.global, &c.child) == &c.child);
	assert(poor_test_child_get_version(&c.child) == 2);
	poor_test_child_add_listener(&c.child, &child_listener, &c);
	poor_test_child_ping(&c.child);
	run_until(&loop, c.pongs == 1);
	assert(test_server.children == 1);
	poor_test_child_destroy(&c.child);
	client_roundtrip(&c);
	assert(test_server.children == 0);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static struct poor_wl_proxy *new_proxy(struct poor_wl_display *display, struct poor_wl_proxy *factory, const struct poor_wl_interface *interface)
{
	struct client *c = client_of(display);

	assert(factory == &c->global.proxy && interface == &poor_test_child_interface);
	return &c->spawned.proxy;
}

TEST(server_created_object)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	poor_wl_display_set_new_proxy(&c.display, new_proxy);
	client_bind_global(&c, 2);
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	poor_test_global_spawn_child(&c.global);
	run_until(&loop, c.events == 1);
	assert(poor_wl_proxy_get_id(&c.spawned.proxy) >= 0xff000000);
	poor_test_child_add_listener(&c.spawned, &child_listener, &c);
	poor_test_child_ping(&c.spawned);
	run_until(&loop, c.pongs == 1);
	poor_test_child_destroy(&c.spawned);
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
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	poor_test_global_spawn_child(&c.global);
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
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	assert(poor_test_global_create_child(&c.global, &c.child) == &c.child);
	poor_test_child_add_listener(&c.child, &child_listener, &c);
	poor_test_child_ping(&c.child);
	poor_test_child_destroy(&c.child);
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
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	for (int i = 0; i < 10; i++) {
		assert(poor_test_global_create_child(&c.global, &c.child) == &c.child);
		assert(poor_wl_proxy_get_id(&c.child.proxy) == 4 || poor_wl_proxy_get_id(&c.child.proxy) == 5);
		poor_test_child_destroy(&c.child);
		client_roundtrip(&c);
	}
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(protocol_error)
{
	const struct poor_wl_interface *interface;
	struct poor_loop loop;
	struct client c;
	uint32_t id;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_test_global_post_error(&c.global, 42);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO);
	assert(poor_wl_display_get_protocol_error(&c.display, &interface, &id) == 42);
	assert(interface == &poor_test_global_interface && id == poor_wl_proxy_get_id(&c.global.proxy));
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
	poor_test_global_since_two(&c.global);
	run_until(&loop, c.error_called);
	assert(c.error == EINVAL && poor_wl_display_get_protocol_error(&c.display, nullptr, nullptr) == POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(bind_of_newer_version)
{
	const struct poor_wl_interface *interface;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 3);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO);
	assert(poor_wl_display_get_protocol_error(&c.display, &interface, nullptr) == POOR_WL_DISPLAY_ERROR_INVALID_OBJECT);
	assert(interface == &poor_wl_registry_interface);
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
	poor_test_global_disconnect(&c.global);
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
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	for (int i = 0; i < 8; i++) {
		poor_test_global_flood(&c.global, 3, 4000);
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
	poor_test_global_flood(&c.global, 100, 1000);
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
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	memset(poor_wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	for (int i = 0; i < 5; i++)
		poor_test_global_echo(&c.global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_until(&loop, c.error_called);
	assert(c.error == E2BIG);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(message_too_large_fails_display)
{
	struct poor_loop loop;
	char array_data[POOR_WL_MAX_MESSAGE_SIZE];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	memset(poor_wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	poor_test_global_echo(&c.global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
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
	poor_test_global_create_child(&c.global, &c.child);
	run_until(&loop, c.error_called);
	assert(c.error == EINVAL && poor_wl_display_get_protocol_error(&c.display, nullptr, nullptr) == POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
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
	poor_test_global_create_child(&c.global, &c.child);
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
	struct poor_wl_connection_memory memory = client_memory(c);

	client_init(c, loop);
	assert(!poor_wl_display_connect(&c->display, loop, &memory, name, client_error, client_done));
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
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_roundtrip(&c);
	poor_wl_registry_bind(&c.registry, c.fixes_name, &poor_wl_fixes_interface, 2, &c.fixes);
	test_server_remove_global();
	run_until(&loop, c.removed_name);
	assert(c.removed_name == c.extra_name && !test_server.withdrawn);
	poor_wl_fixes_ack_global_remove(&c.fixes, &c.registry, c.removed_name);
	client_roundtrip(&c);
	assert(test_server.withdrawn == 1 && test_server_global_destroyed());
	poor_wl_fixes_ack_global_remove(&c.fixes, &c.registry, c.removed_name);
	run_until(&loop, c.error_called);
	assert(poor_wl_display_get_protocol_error(&c.display, nullptr, nullptr) == POOR_WL_FIXES_ERROR_INVALID_ACK_REMOVE);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(global_removal_acknowledged_by_registry_destruction)
{
	struct poor_loop loop;
	struct client a, b;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&a, &loop, test_server_connect());
	client_connect_fd(&b, &loop, test_server_connect());
	client_roundtrip(&a);
	client_roundtrip(&b);
	poor_wl_registry_bind(&a.registry, a.fixes_name, &poor_wl_fixes_interface, 2, &a.fixes);
	test_server_remove_global();
	run_until(&loop, a.removed_name && b.removed_name);
	poor_wl_fixes_destroy_registry(&a.fixes, &a.registry);
	poor_wl_registry_destroy(&a.registry);
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
	struct poor_loop loop;
	struct client c;
	uint32_t *data;
	int fd;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_wl_registry_bind(&c.registry, c.shm_name, &poor_wl_shm_interface, 1, &c.shm);
	fd = memfd_create("shm-test", MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, 4096));
	data = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(data != MAP_FAILED);
	data[0] = pixel;
	poor_wl_shm_create_pool(&c.shm, &c.pool, fd, 4096);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	poor_wl_shm_pool_create_buffer(&c.pool, &c.buffer, 0, 16, 16, 64, POOR_WL_SHM_FORMAT_XRGB8888);
	poor_test_global_read_buffer(&c.global, &c.buffer);
	client_roundtrip(&c);
	assert(!c.error_called && test_server.shm_buffers == 1 && test_server.shm_pixel == pixel);
	poor_wl_buffer_destroy(&c.buffer);
	poor_wl_shm_pool_destroy(&c.pool);
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
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	poor_test_global_flood(&c.global, 2, 8);
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
	poor_wl_registry_bind(&c.registry, c.global_name + 1, &poor_test_child_interface, 1, &c.child);
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO && poor_wl_display_get_protocol_error(&c.display, nullptr, nullptr) == POOR_WL_DISPLAY_ERROR_INVALID_OBJECT);
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
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	poor_test_global_flood(&c.global, 3, 4);
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
	int nr_fds = count_open_fds(), status;
	struct client c;
	pid_t pid;

	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen("wayland-cloexec");
	run_until(&loop, test_server.socket_ready);
	connect_by_name(&c, &loop, "wayland-cloexec");
	client_bind_global(&c, 2);
	assert(poor_test_global_create_child(&c.global, &c.child) == &c.child);
	poor_test_child_add_listener(&c.child, &child_listener, &c);
	poor_test_child_ping(&c.child);
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

static void echoed_then_disconnect(void *data, struct poor_test_global *, int32_t, uint32_t, poor_wl_fixed_t, const char *, const char *,
				   struct poor_test_global *, struct poor_wl_array *, int32_t fd)
{
	struct client *c = data;

	poor_loop_close_fd_or_submit(c->loop, fd, 0);
	c->echoed = true;
	poor_wl_display_disconnect(&c->display);
}

static const struct poor_test_global_listener disconnecting_listener = {
	echoed_then_disconnect,
	global_child_spawned,
	global_data,
};

/* The second echo is not dispatched, the display is gone */
TEST(disconnect_in_event_listener)
{
	struct poor_loop loop;
	char array_data[1];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_test_global_add_listener(&c.global, &disconnecting_listener, &c);
	poor_test_global_echo(&c.global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	poor_test_global_echo(&c.global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
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
	struct poor_wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	memset(name, 'a', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	assert(poor_wl_display_connect(&c.display, &loop, &memory, name, client_error, client_done) == -1 && errno == ENAMETOOLONG);
	unsetenv("XDG_RUNTIME_DIR");
	assert(poor_wl_display_connect(&c.display, &loop, &memory, "wayland-0", client_error, client_done) == -1 && errno == ENOENT);
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
	struct poor_wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	test_server_start(&loop);
	snprintf(fd_string, sizeof(fd_string), "%d", test_server_connect());
	setenv("WAYLAND_SOCKET", fd_string, 1);
	client_init(&c, &loop);
	assert(!poor_wl_display_connect(&c.display, &loop, &memory, "wayland-ignored", client_error, client_done));
	assert(!getenv("WAYLAND_SOCKET"));
	assert(poor_wl_display_get_fd(&c.display) == atoi(fd_string));
	client_get_registry(&c);
	client_roundtrip(&c);
	assert(c.synced && c.global_name);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static int dispatcher(const void *implementation, void *target, uint32_t opcode, const struct poor_wl_message *message,
		      union poor_wl_argument *args)
{
	struct client *c = (void *)implementation;

	assert(target == &c->global.proxy && message == &poor_test_global_interface.events[opcode] && !strcmp(message->name, "data"));
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
	assert(!poor_wl_proxy_add_dispatcher(&c.global.proxy, dispatcher, &c, nullptr));
	assert(poor_test_global_add_listener(&c.global, &global_listener, &c) == -1);
	poor_test_global_flood(&c.global, 2, 3);
	assert(!poor_wl_display_flush(&c.display));
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
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	struct client c;
	int fds[2], sndbuf = 4096;

	loop_init(&loop);
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	assert(!setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
	client_connect_fd(&c, &loop, fds[0]);
	memset(poor_wl_array_add(&array, sizeof(array_data)), 0, sizeof(array_data));
	poor_wl_registry_bind(&c.registry, 1, &poor_test_global_interface, 1, &c.global);
	for (int i = 0; i < 3; i++)
		poor_test_global_echo(&c.global, 0, 0, 0, "", nullptr, nullptr, &array, STDERR_FILENO);
	run_for(&loop, 100'000'000);
	assert(c.display.send_op.pending && !c.error_called);
	client_disconnect(&c);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	loop_fini(&loop);
}

TEST(proxy_accessors)
{
	static const char *const tag = "tag";
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	assert(poor_wl_proxy_get_listener(&c.global.proxy) == &global_listener);
	assert(poor_wl_proxy_add_dispatcher(&c.global.proxy, dispatcher, &c, nullptr) == -1);
	assert(poor_test_global_get_user_data(&c.global) == &c);
	poor_test_global_set_user_data(&c.global, &loop);
	assert(poor_test_global_get_user_data(&c.global) == &loop);
	assert(!poor_wl_proxy_get_tag(&c.global.proxy));
	poor_wl_proxy_set_tag(&c.global.proxy, &tag);
	assert(poor_wl_proxy_get_tag(&c.global.proxy) == &tag);
	assert(!strcmp(poor_wl_proxy_get_class(&c.global.proxy), "test_global"));
	assert(poor_wl_proxy_get_interface(&c.global.proxy) == &poor_test_global_interface);
	assert(poor_wl_proxy_get_display(&c.global.proxy) == &c.display);
	assert(!poor_wl_proxy_get_version(&c.display.proxy));
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* After an error requests go nowhere, but new objects still get their memory initialized */
TEST(requests_after_error)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_bind_global(&c, 2);
	poor_test_global_post_error(&c.global, 7);
	run_until(&loop, c.error_called);
	assert(poor_wl_display_flush(&c.display) == -1 && errno == EPROTO);
	assert(poor_test_global_create_child(&c.global, &c.child) == &c.child);
	assert(poor_test_child_get_version(&c.child) == 2);
	poor_test_global_flood(&c.global, 1, 1);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(invalid_request_arguments)
{
	char array_data[1];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(array_data);
	struct poor_loop loop;
	struct client a, b;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&a, &loop, test_server_connect());
	client_connect_fd(&b, &loop, test_server_connect());
	client_bind_global(&a, 2);
	client_bind_global(&b, 2);
	poor_test_global_echo(&a.global, 0, 0, 0, nullptr, nullptr, nullptr, &array, STDERR_FILENO);
	poor_test_global_echo(&b.global, 0, 0, 0, "", nullptr, nullptr, &array, -1);
	run_until(&loop, a.error_called && b.error_called);
	assert(a.error == EINVAL && logged("Error marshalling request for test_global.echo"));
	assert(b.error == EBADF && logged("Error sending request for test_global.echo"));
	client_disconnect(&a);
	client_disconnect(&b);
	server_stop(&loop);
	loop_fini(&loop);
}

/* Ids 1 to 3 go to the display, the registry and the global */
TEST(client_runs_out_of_ids)
{
	struct poor_wl_connection_memory memory;
	struct poor_test_child children[4];
	struct poor_loop loop;
	unsigned count = 0;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_init(&c, &loop);
	memory = client_memory(&c);
	memory.client_objects_size = 4;
	poor_wl_display_connect_to_fd(&c.display, &loop, &memory, test_server_connect(), client_error, client_done);
	client_get_registry(&c);
	client_bind_global(&c, 2);
	while (count < ARRAY_SIZE(children) && poor_test_global_create_child(&c.global, &children[count]))
		count++;
	assert(!count && !c.error_called);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

static struct poor_test_child spawned[2];
static unsigned spawned_count;

static struct poor_wl_proxy *new_spawned(struct poor_wl_display *, struct poor_wl_proxy *, const struct poor_wl_interface *)
{
	return &spawned[spawned_count++].proxy;
}

/* The client maps one id of the server only, the second new object is not even created */
TEST(server_created_objects_run_out)
{
	struct poor_wl_connection_memory memory;
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_init(&c, &loop);
	memory = client_memory(&c);
	memory.server_objects_size = 1;
	poor_wl_display_connect_to_fd(&c.display, &loop, &memory, test_server_connect(), client_error, client_done);
	poor_wl_display_set_new_proxy(&c.display, new_spawned);
	client_get_registry(&c);
	client_bind_global(&c, 2);
	poor_test_global_spawn_child(&c.global);
	poor_test_global_spawn_child(&c.global);
	run_until(&loop, c.error_called);
	assert(c.error == ENOSPC && spawned_count == 1);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

/* The server deletes the id of the registry before the client destroys its proxy */
TEST(proxy_destroyed_after_its_id_was_deleted)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	client_roundtrip(&c);
	poor_wl_registry_bind(&c.registry, c.fixes_name, &poor_wl_fixes_interface, 1, &c.fixes);
	poor_wl_fixes_destroy_registry(&c.fixes, &c.registry);
	client_roundtrip(&c);
	poor_wl_registry_destroy(&c.registry);
	client_roundtrip(&c);
	assert(!c.error_called);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(debug_output_of_new_objects)
{
	struct poor_loop loop;
	struct client c;

	setenv("WAYLAND_DEBUG", "1", 1);
	loop_init(&loop);
	test_server_start(&loop);
	client_connect_fd(&c, &loop, test_server_connect());
	poor_wl_display_set_new_proxy(&c.display, new_proxy);
	client_bind_global(&c, 2);
	poor_test_global_add_listener(&c.global, &global_listener, &c);
	poor_test_global_spawn_child(&c.global);
	run_until(&loop, c.events == 1);
	assert(logged("test_global#3.child_spawned(new id test_child#4278190080)"));
	poor_test_child_destroy(&c.spawned);
	client_disconnect(&c);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(connect_with_environment)
{
	char name[200];
	struct poor_loop loop;
	struct client c;
	struct poor_wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	test_server_start(&loop);
	test_server_listen("wayland-env");
	run_until(&loop, test_server.socket_ready);
	setenv("WAYLAND_DISPLAY", "wayland-env", 1);
	connect_by_name(&c, &loop, nullptr);
	client_roundtrip(&c);
	assert(c.synced);
	client_disconnect(&c);
	unsetenv("WAYLAND_DISPLAY");
	connect_by_name(&c, &loop, nullptr);
	run_until(&loop, c.error_called);
	assert(c.error == ENOENT);
	client_disconnect(&c);
	name[0] = '/';
	memset(name + 1, 'a', sizeof(name) - 2);
	name[sizeof(name) - 1] = '\0';
	assert(poor_wl_display_connect(&c.display, &loop, &memory, name, client_error, client_done) == -1 && errno == ENAMETOOLONG);
	server_stop(&loop);
	loop_fini(&loop);
}

TEST(invalid_wayland_socket)
{
	struct poor_loop loop;
	struct client c;
	struct poor_wl_connection_memory memory = client_memory(&c);

	loop_init(&loop);
	setenv("WAYLAND_SOCKET", "3x", 1);
	assert(poor_wl_display_connect(&c.display, &loop, &memory, nullptr, client_error, client_done) == -1 && errno == EINVAL);
	setenv("WAYLAND_SOCKET", "999", 1);
	assert(poor_wl_display_connect(&c.display, &loop, &memory, nullptr, client_error, client_done) == -1 && errno == EBADF);
	unsetenv("WAYLAND_SOCKET");
	loop_fini(&loop);
}

TEST(disconnect_while_connecting)
{
	struct poor_loop loop;
	struct client c;

	loop_init(&loop);
	connect_by_name(&c, &loop, "wayland-nobody");
	client_disconnect(&c);
	assert(!c.error_called);
	loop_fini(&loop);
}

/* The fd limit leaves none for the socket */
TEST(connect_without_fds)
{
	struct rlimit limit, low;
	struct poor_loop loop;
	struct client c;
	int fd = 0;

	loop_init(&loop);
	assert(!getrlimit(RLIMIT_NOFILE, &limit));
	while (fcntl(fd, F_GETFD) >= 0)
		fd++;
	low = limit;
	low.rlim_cur = fd;
	assert(!setrlimit(RLIMIT_NOFILE, &low));
	connect_by_name(&c, &loop, "wayland-nobody");
	run_until(&loop, c.error_called);
	assert(!setrlimit(RLIMIT_NOFILE, &limit));
	assert(c.error == EMFILE);
	client_disconnect(&c);
	loop_fini(&loop);
}

TEST(connection_is_not_a_socket)
{
	struct poor_loop loop;
	struct client c;
	int fds[2];

	loop_init(&loop);
	assert(!pipe2(fds, O_CLOEXEC));
	client_connect_fd(&c, &loop, fds[0]);
	run_until(&loop, c.error_called);
	assert(c.error == ENOTSOCK);
	client_disconnect(&c);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	loop_fini(&loop);
}

/* The client gets events that the test writes to the returned fd */
static int connect_raw(struct client *c, struct poor_loop *loop)
{
	int fds[2];

	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	client_connect_fd(c, loop, fds[0]);
	return fds[1];
}

enum {
	DISPLAY_ERROR = 0,
	DISPLAY_DELETE_ID = 1,
};

static void write_error(int fd, uint32_t object, uint32_t code, const char *message)
{
	size_t size = strlen(message) + 1, count = 5 + (size + 3) / 4;
	uint32_t words[32];

	assert(count <= ARRAY_SIZE(words));
	words[0] = 1;
	words[1] = count * sizeof(*words) << 16 | DISPLAY_ERROR;
	words[2] = object;
	words[3] = code;
	words[4] = size;
	words[count - 1] = 0;
	memcpy(words + 5, message, size);
	write_words(fd, words, count);
}

static void write_words_with_fds(int fd, const uint32_t *words, size_t count, unsigned nfds)
{
	alignas(struct cmsghdr) char control[CMSG_SPACE(32 * sizeof(int))];
	struct iovec iov = { (void *)words, count * sizeof(*words) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = CMSG_SPACE(nfds * sizeof(int)) };
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	int fds[32];

	assert(nfds <= ARRAY_SIZE(fds));
	for (unsigned i = 0; i < nfds; i++)
		fds[i] = STDERR_FILENO;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(nfds * sizeof(int));
	memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));
	assert(sendmsg(fd, &msg, 0) == (ssize_t)iov.iov_len);
}

/* Events of unknown objects and of destroyed ones are dropped, the debug log tells about them */
TEST(discarded_events)
{
	static const uint32_t dropped[] = {
		9, 8 << 16 | 0,
		3, 12 << 16 | 2, 0,
		1, 12 << 16 | DISPLAY_DELETE_ID, 77,
	};
	static const uint32_t delete_id[] = { 1, 12 << 16 | DISPLAY_DELETE_ID, 3 };
	static const uint32_t no_such_event[] = { 1, 8 << 16 | 5 };
	struct poor_loop loop;
	struct client c;
	int fd;

	setenv("WAYLAND_DEBUG", "client", 1);
	setenv("FORCE_COLOR", "1", 1);
	setenv("NO_COLOR", "1", 1);
	loop_init(&loop);
	fd = connect_raw(&c, &loop);
	poor_wl_registry_bind(&c.registry, 1, &poor_test_global_interface, 1, &c.global);
	poor_test_global_destroy(&c.global);
	write_words(fd, dropped, ARRAY_SIZE(dropped));
	run_until(&loop, logged("received delete_id for unknown id (77)"));
	assert(logged("discarded [unknown]#9.[event 0](0 fd, 8 byte)"));
	assert(logged("discarded [zombie]#3.[event 2](0 fd, 12 byte)"));
	write_words(fd, delete_id, 2);
	run_for(&loop, 20'000'000);
	write_words(fd, delete_id + 2, 1);
	write_words(fd, no_such_event, ARRAY_SIZE(no_such_event));
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO && logged("interface 'wl_display' has no event 5"));
	client_disconnect(&c);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop);
}

/* The code of an error of the display tells what failed, an error of another object is a protocol error */
TEST(error_events)
{
	static const uint32_t codes[] = { POOR_WL_DISPLAY_ERROR_INVALID_OBJECT, POOR_WL_DISPLAY_ERROR_NO_MEMORY,
					  POOR_WL_DISPLAY_ERROR_IMPLEMENTATION, 99 };
	static const int errors[] = { EINVAL, ENOMEM, EPROTO, EFAULT };
	const struct poor_wl_interface *interface;
	struct poor_loop loop;
	struct client c[5];
	uint32_t id;
	int fds[5];

	loop_init(&loop);
	for (unsigned i = 0; i < ARRAY_SIZE(codes); i++) {
		fds[i] = connect_raw(&c[i], &loop);
		write_error(fds[i], 1, codes[i], "error");
		run_until(&loop, c[i].error_called);
		assert(c[i].error == errors[i] && poor_wl_display_get_protocol_error(&c[i].display, &interface, &id) == codes[i]);
		assert(interface == &poor_wl_display_interface && id == 1);
	}
	fds[4] = connect_raw(&c[4], &loop);
	poor_wl_registry_bind(&c[4].registry, 1, &poor_test_global_interface, 1, &c[4].global);
	poor_test_global_destroy(&c[4].global);
	write_error(fds[4], 3, 5, "destroyed");
	run_until(&loop, c[4].error_called);
	assert(c[4].error == EPROTO && poor_wl_display_get_protocol_error(&c[4].display, &interface, &id) == 5);
	assert(!interface && !id && logged("[destroyed object]: error 5: destroyed"));
	for (unsigned i = 0; i < ARRAY_SIZE(c); i++) {
		client_disconnect(&c[i]);
		poor_loop_close_fd_or_submit(&loop, fds[i], 0);
	}
	loop_fini(&loop);
}

TEST(malformed_events)
{
	static const uint32_t too_large[] = { 1, (POOR_WL_MAX_MESSAGE_SIZE + 4) << 16 | DISPLAY_ERROR };
	static const uint32_t missing_argument[] = { 1, 8 << 16 | DISPLAY_DELETE_ID };
	struct poor_loop loop;
	struct client a, b;
	int fds[2];

	loop_init(&loop);
	fds[0] = connect_raw(&a, &loop);
	fds[1] = connect_raw(&b, &loop);
	write_words(fds[0], too_large, ARRAY_SIZE(too_large));
	write_words(fds[1], missing_argument, ARRAY_SIZE(missing_argument));
	run_until(&loop, a.error_called && b.error_called);
	assert(a.error == E2BIG && b.error == EINVAL);
	client_disconnect(&a);
	client_disconnect(&b);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	loop_fini(&loop);
}

/* More fds than one recvmsg() takes fail the display, and so do sends that fill the queue */
TEST(server_sends_too_many_fds)
{
	static const uint32_t delete_id[] = { 1, 12 << 16 | DISPLAY_DELETE_ID, 77 };
	struct poor_loop loop;
	struct client a, b;
	int fds[2];

	loop_init(&loop);
	fds[0] = connect_raw(&a, &loop);
	fds[1] = connect_raw(&b, &loop);
	write_words_with_fds(fds[0], delete_id, ARRAY_SIZE(delete_id), POOR_WL_CONNECTION_MAX_FDS + 1);
	for (unsigned i = 0; i < POOR_WL_CONNECTION_FDS / POOR_WL_CONNECTION_MAX_FDS + 1; i++)
		write_words_with_fds(fds[1], delete_id, ARRAY_SIZE(delete_id), POOR_WL_CONNECTION_MAX_FDS);
	run_until(&loop, a.error_called && b.error_called);
	assert(a.error == EOVERFLOW && b.error == EOVERFLOW);
	client_disconnect(&a);
	client_disconnect(&b);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	loop_fini(&loop);
}

/* A server that stops reading fails the sends, but the error that it sent is what the client gets */
TEST(server_stops_reading)
{
	struct poor_loop loop;
	struct client c;
	int fd;

	loop_init(&loop);
	fd = connect_raw(&c, &loop);
	assert(!shutdown(fd, SHUT_RD));
	run_until(&loop, c.display.hangup);
	assert(!c.error_called);
	write_error(fd, 1, POOR_WL_DISPLAY_ERROR_IMPLEMENTATION, "bye");
	run_until(&loop, c.error_called);
	assert(c.error == EPROTO);
	client_disconnect(&c);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	loop_fini(&loop);
}
