/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poor_loop_fd.h>
#include <poor_poll.h>
#include <poor_socket.h>
#include <poor_stdio.h>
#include <poor_uio.h>
#include <poor_unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "test-ring.h"
#include "test-runner.h"
#include "tests-server-protocol.h"
#include "poor_wayland_server_display.h"
#include "poor_wayland_server.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;

/* The server headers define the opcodes of events only */
enum {
	DISPLAY_SYNC = 0,
	DISPLAY_GET_REGISTRY = 1,
	REGISTRY_BIND = 0,
	SHM_CREATE_POOL = 0,
	SHM_RELEASE = 1,
	SHM_POOL_CREATE_BUFFER = 0,
	SHM_POOL_DESTROY = 1,
	SHM_POOL_RESIZE = 2,
	BUFFER_DESTROY = 0,
	FIXES_DESTROY_REGISTRY = 1,
	FIXES_ACK_GLOBAL_REMOVE = 2,
	CHILD_PING = 1,
};

/* The test speaks the protocol on one end of a socket pair, the display serves the other end */
struct peer {
	struct poor_wl_client client;
	int fd;
	uint32_t next_id;
	bool done;
	size_t len, pos;
	uint32_t buf[4096];
	char in[16384];
	char out[16384];
	union poor_wl_map_entry objects[64];
	union poor_wl_map_entry server_objects[16];
};

struct event {
	uint32_t id, opcode, size;
	const uint32_t *args;
};

struct test_socket {
	struct poor_wl_socket socket;
	unsigned accepted;
	bool ready, done, destroy_when_ready, limit_fds_when_ready;
};

/* A peer with a wl_shm, a pool and a buffer of 16x16 pixels at the start of the pool */
struct shm_client {
	struct peer peer;
	uint32_t shm, pool, buffer;
	uint32_t *data;
	size_t size;
	int fd;
};

static struct poor_loop loop;
static struct poor_wl_display display;
static struct poor_wl_global shm_global;
static bool display_done;
static int allocations, alloc_budget = -1;
static unsigned order[8], order_len;
static char log_text[65536];
static size_t log_len;
static struct rlimit fd_limit;

static void watchdog_fire(struct poor_loop *, struct poor_loop_timer *)
{
	fprintf(stderr, "loop timed out\n");
	abort();
}

static struct poor_loop_timer watchdog = POOR_LOOP_TIMER_INIT(watchdog_fire);

#define run_until(cond) \
	do { \
		while (!(cond)) \
			assert(!poor_loop_run_once(&loop)); \
	} while (0)

struct expiry {
	struct poor_loop_timer timer;
	bool expired;
};

static void expire(struct poor_loop *, struct poor_loop_timer *timer)
{
	container_of(timer, struct expiry, timer)->expired = true;
}

static void run_for(uint64_t ns)
{
	struct expiry expiry;

	poor_loop_timer_init(&expiry.timer, expire);
	expiry.expired = false;
	poor_loop_timer_arm(&loop, &expiry.timer, poor_loop_now() + ns);
	run_until(expiry.expired);
}

static void log_handler(const char *fmt, va_list ap)
{
	char line[8192];
	size_t len;

	vsnprintf_array(line, fmt, ap);
	fprintf(stderr, "server: %s", line);
	len = strlen(line);
	if (log_len + len < ARRAY_SIZE(log_text)) {
		copy_array(arrview_cfront(log_len, log_text), arrview_first(len + 1, line));
		log_len += len;
	}
}

static bool logged(const char *text)
{
	return strstr(log_text, text);
}

static void record(unsigned what)
{
	assert(order_len < ARRAY_SIZE(order));
	order[order_len++] = what;
}

static void *server_alloc(struct poor_wl_display *, struct poor_wl_client *, size_t size)
{
	if (!alloc_budget)
		return nullptr;
	if (alloc_budget > 0)
		alloc_budget--;
	allocations++;
	return malloc(size);
}

static void server_free(struct poor_wl_display *, void *ptr)
{
	allocations--;
	free(ptr);
}

static void on_display_done(struct poor_wl_display *)
{
	display_done = true;
}

static void server_start(void)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(&loop, 64, &params));
	poor_loop_timer_arm(&loop, &watchdog, poor_loop_now() + TIMEOUT_NS);
	poor_wl_log_set_handler_server(log_handler);
	poor_wl_display_init(&display, &loop, on_display_done);
	poor_wl_display_set_allocator(&display, server_alloc, server_free);
}

/* Everything that the display allocated is freed by the time it is done */
static void server_stop(void)
{
	poor_wl_display_destroy(&display);
	run_until(display_done);
	assert(!allocations);
	poor_loop_timer_disarm(&watchdog);
	assert(!poor_loop_drain(&loop, poor_loop_now() + TIMEOUT_NS));
	poor_loop_exit(&loop);
}

static void peer_done(struct poor_wl_client *client)
{
	container_of(client, struct peer, client)->done = true;
}

static void peer_init(struct peer *p, int server_fd, int fd)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(p->in, p->out, p->objects, p->server_objects);

	p->fd = fd;
	p->next_id = 2;
	p->done = false;
	p->len = p->pos = 0;
	poor_wl_client_init(&p->client, &display, server_fd, &memory, peer_done);
}

static void peer_connect(struct peer *p)
{
	int fds[2];

	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	peer_init(p, fds[0], fds[1]);
}

static void peer_close(struct peer *p)
{
	poor_loop_close_fd_or_submit(&loop, p->fd, 0);
}

static uint32_t peer_new_id(struct peer *p)
{
	return p->next_id++;
}

/* A message starts with the object and the opcode, the size of the message goes next to the opcode */
static void peer_send_impl(struct peer *p, size_t count, uint32_t (*message)[count], size_t nfds, const int (*fds)[nfds])
{
	alignas(struct cmsghdr) char control[CMSG_SPACE(32 * sizeof(int))];
	struct iovec iov = array_iovec(message);
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	struct cmsghdr *cmsg;

	assert(nfds <= 32);
	(*message)[1] |= ARRAY_SIZE_BYTES(message) << 16;
	if (nfds) {
		msg.msg_control = control;
		msg.msg_controllen = CMSG_SPACE(ARRAY_SIZE_BYTES(fds));
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(ARRAY_SIZE_BYTES(fds));
		memcpy(CMSG_DATA(cmsg), fds, ARRAY_SIZE_BYTES(fds));
	}
	assert(sendmsg(p->fd, &msg, 0) == (ssize_t)iov.iov_len);
}

#define peer_send(p, message) peer_send_impl(p, ARRAY_SIZE(message), &auto_arr(message), 0, nullptr)
#define peer_send_fds(p, message, fds) peer_send_impl(p, ARRAY_SIZE(message), &auto_arr(message), ARRAY_SIZE(fds), &auto_arr(fds))
#define SEND(p, ...) peer_send(p, ((uint32_t[]){ __VA_ARGS__ }))
#define SEND_FDS(p, fds, ...) peer_send_fds(p, ((uint32_t[]){ __VA_ARGS__ }), fds)

static const uint32_t *skip_string(const uint32_t *p)
{
	return p + 1 + (p[0] + 3) / 4;
}

static bool readable(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	return poll_array(array_ptr(&pfd), 0) == 1;
}

/*
 * Waits for the server to send something or to close the connection, then 0 is returned. Closing an fd wakes nobody
 * up, so the loop runs in short slices.
 */
static ssize_t peer_read(struct peer *p)
{
	ssize_t n;

	while (!readable(p->fd))
		run_for(1'000'000);
	n = recv_array(p->fd, arrview_cfront(p->len, arrview_bytes(p->buf)), MSG_DONTWAIT);
	assert(n >= 0);
	p->len += n;
	return n;
}

static bool peer_has_event(struct peer *p)
{
	return p->len - p->pos >= 8 && p->len - p->pos >= p->buf[p->pos / 4 + 1] >> 16;
}

static struct event peer_event(struct peer *p)
{
	struct event e;

	while (!peer_has_event(p))
		assert(peer_read(p) > 0);
	e.id = p->buf[p->pos / 4];
	e.opcode = p->buf[p->pos / 4 + 1] & 0xffff;
	e.size = p->buf[p->pos / 4 + 1] >> 16;
	e.args = p->buf + p->pos / 4 + 2;
	p->pos += e.size;
	return e;
}

static const char *error_message(const struct event *e)
{
	return (const char *)(e->args + 3);
}

/* The events before the error are skipped. The error is the last one, the server destroys the client. */
static struct event expect_error(struct peer *p, uint32_t object, uint32_t code)
{
	struct event e;

	do
		e = peer_event(p);
	while (e.id != 1 || e.opcode != POOR_WL_DISPLAY_ERROR);
	assert(e.args[0] == object && e.args[1] == code);
	run_until(p->done);
	assert(!peer_read(p));
	return e;
}

/* The events before the done event of the callback are skipped */
static void peer_roundtrip(struct peer *p)
{
	uint32_t id = peer_new_id(p);
	struct event e;

	SEND(p, 1, DISPLAY_SYNC, id);
	do
		e = peer_event(p);
	while (e.id != id);
	assert(e.opcode == POOR_WL_CALLBACK_DONE);
	e = peer_event(p);
	assert(e.id == 1 && e.opcode == POOR_WL_DISPLAY_DELETE_ID && e.args[0] == id);
}

static uint32_t peer_get_registry(struct peer *p)
{
	uint32_t id = peer_new_id(p);

	SEND(p, 1, DISPLAY_GET_REGISTRY, id);
	return id;
}

static uint32_t peer_bind(struct peer *p, uint32_t registry, uint32_t name, const char *interface, uint32_t version)
{
	uint32_t string[STRING_WORDS(interface)], id = peer_new_id(p);

	put_string(string, interface);
	make_merged_array(message, ((uint32_t[]){ registry, REGISTRY_BIND, name }), string, ((uint32_t[]){ version, id }));
	peer_send(p, message);
	return id;
}

static void child_destroy_request(struct poor_wl_client *, struct poor_test_child_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static void child_ping(struct poor_wl_client *, struct poor_test_child_resource *)
{
}

static const struct poor_test_child_interface child_impl = {
	child_destroy_request,
	child_ping,
};

static void bind_nothing(struct poor_wl_client *, void *, uint32_t, uint32_t)
{
}

static void other_notify(struct poor_wl_listener *, void *)
{
}

TEST(resource_accessors)
{
	struct poor_test_child_resource child, spawned;
	struct peer p;
	int data;

	server_start();
	peer_connect(&p);
	assert(!poor_test_child_resource_init(&child, &p.client, 2, peer_new_id(&p)));
	poor_test_child_resource_set_implementation(&child, &child_impl, &data, nullptr);
	assert(poor_wl_resource_get_id(&child.resource) == 2);
	assert(poor_wl_resource_get_client(&child.resource) == &p.client);
	assert(poor_wl_resource_get_version(&child.resource) == 2);
	assert(!strcmp(poor_wl_resource_get_class(&child.resource), "test_child"));
	assert(poor_wl_resource_get_interface(&child.resource) == &poor_test_child_interface);
	assert(poor_wl_resource_get_user_data(&child.resource) == &data);
	poor_wl_resource_set_user_data(&child.resource, &p);
	assert(poor_wl_resource_get_user_data(&child.resource) == &p);
	assert(poor_wl_resource_instance_of(&child.resource, &poor_test_child_interface, &child_impl));
	assert(!poor_wl_resource_instance_of(&child.resource, &poor_test_child_interface, &data));
	assert(poor_wl_client_get_object(&p.client, 2) == &child.resource);
	assert(!poor_test_child_resource_init(&spawned, &p.client, 1, 0));
	assert(poor_wl_resource_get_id(&spawned.resource) >= 0xff000000);
	assert(poor_wl_client_get_object(&p.client, poor_wl_resource_get_id(&spawned.resource)) == &spawned.resource);
	poor_wl_resource_destroy(&spawned.resource);
	poor_wl_resource_destroy(&child.resource);
	assert(!poor_wl_client_get_object(&p.client, 2));
	peer_close(&p);
	server_stop();
}

TEST(resource_lists)
{
	struct poor_test_child_resource a, b;
	struct poor_wl_resource *resource, *tmp;
	struct poor_wl_list list;
	struct peer p, q;
	int count = 0;

	server_start();
	peer_connect(&p);
	peer_connect(&q);
	assert(!poor_test_child_resource_init(&a, &p.client, 1, 0));
	assert(!poor_test_child_resource_init(&b, &q.client, 1, 0));
	poor_wl_list_init(&list);
	poor_wl_list_insert(&list, poor_wl_resource_get_link(&a.resource));
	poor_wl_list_insert(&list, poor_wl_resource_get_link(&b.resource));
	assert(poor_wl_resource_from_link(poor_wl_resource_get_link(&a.resource)) == &a.resource);
	assert(poor_wl_resource_find_for_client(&list, &p.client) == &a.resource);
	assert(poor_wl_resource_find_for_client(&list, &q.client) == &b.resource);
	assert(!poor_wl_resource_find_for_client(&list, nullptr));
	poor_wl_resource_for_each(resource, &list)
		count++;
	assert(count == 2);
	poor_wl_resource_for_each_safe(resource, tmp, &list)
		poor_wl_list_remove(poor_wl_resource_get_link(resource));
	poor_wl_list_init(&list);
	assert(!poor_wl_resource_find_for_client(&list, &p.client));
	peer_close(&p);
	peer_close(&q);
	server_stop();
}

static void resource_destroy_notify(struct poor_wl_listener *, void *data)
{
	record(1);
	assert(poor_wl_resource_get_id(data) == 2);
}

static void resource_destructor(struct poor_wl_resource *)
{
	record(2);
}

/* The id of a destroyed resource is deleted with the next event that the client gets */
TEST(resource_destroy_notifications)
{
	struct poor_wl_listener listener = { .notify = resource_destroy_notify };
	struct poor_test_child_resource child;
	struct poor_wl_callback_resource callback;
	struct peer p;
	struct event e;

	server_start();
	peer_connect(&p);
	assert(!poor_test_child_resource_init(&child, &p.client, 1, peer_new_id(&p)));
	poor_test_child_resource_set_implementation(&child, &child_impl, nullptr, nullptr);
	poor_wl_resource_set_destructor(&child.resource, resource_destructor);
	poor_wl_resource_add_destroy_listener(&child.resource, &listener);
	assert(poor_wl_resource_get_destroy_listener(&child.resource, resource_destroy_notify) == &listener);
	assert(!poor_wl_resource_get_destroy_listener(&child.resource, other_notify));
	poor_wl_resource_destroy(&child.resource);
	assert(order_len == 2 && order[0] == 1 && order[1] == 2);
	assert(!poor_wl_client_get_object(&p.client, 2));
	assert(!poor_wl_callback_resource_init(&callback, &p.client, 1, 0));
	poor_wl_callback_send_done(&callback, 42);
	e = peer_event(&p);
	assert(e.id == 1 && e.opcode == POOR_WL_DISPLAY_DELETE_ID && e.args[0] == 2);
	e = peer_event(&p);
	assert(e.id == poor_wl_resource_get_id(&callback.resource) && e.opcode == POOR_WL_CALLBACK_DONE && e.args[0] == 42);
	poor_wl_resource_destroy(&callback.resource);
	peer_close(&p);
	server_stop();
}

/* A new id that skips ids fails the client */
TEST(resource_with_invalid_id)
{
	struct poor_test_child_resource child;
	struct event e;
	struct peer p;

	server_start();
	peer_connect(&p);
	assert(poor_test_child_resource_init(&child, &p.client, 1, 10) == -1);
	e = expect_error(&p, 1, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT);
	assert(!strcmp(error_message(&e), "invalid new id 10"));
	peer_close(&p);
	server_stop();
}

TEST(server_ids_run_out)
{
	struct poor_test_child_resource children[17];
	unsigned count = 0;
	struct peer p;

	server_start();
	peer_connect(&p);
	while (count < ARRAY_SIZE(children) && !poor_test_child_resource_init(&children[count], &p.client, 1, 0))
		count++;
	assert(count == ARRAY_SIZE(p.server_objects));
	peer_close(&p);
	server_stop();
}

static struct peer *destroyed_peer;

static void client_destroy_notify(struct poor_wl_listener *, void *data)
{
	record(1);
	assert(data == &destroyed_peer->client && poor_wl_client_get_object(data, 2));
}

static void client_destroy_late_notify(struct poor_wl_listener *, void *data)
{
	record(3);
	assert(!poor_wl_client_get_object(data, 2));
}

static void client_data_dtor(void *data)
{
	record(4);
	assert(data == &order);
}

TEST(client_destroy_order)
{
	struct poor_wl_listener destroy = { .notify = client_destroy_notify }, late = { .notify = client_destroy_late_notify };
	struct poor_test_child_resource child;
	struct peer p;

	server_start();
	peer_connect(&p);
	destroyed_peer = &p;
	assert(!poor_test_child_resource_init(&child, &p.client, 1, peer_new_id(&p)));
	poor_wl_resource_set_destructor(&child.resource, resource_destructor);
	poor_wl_client_add_destroy_listener(&p.client, &destroy);
	poor_wl_client_add_destroy_late_listener(&p.client, &late);
	assert(poor_wl_client_get_destroy_listener(&p.client, client_destroy_notify) == &destroy);
	assert(!poor_wl_client_get_destroy_listener(&p.client, client_destroy_late_notify));
	assert(poor_wl_client_get_destroy_late_listener(&p.client, client_destroy_late_notify) == &late);
	assert(!poor_wl_client_get_destroy_late_listener(&p.client, client_destroy_notify));
	poor_wl_client_set_user_data(&p.client, &order, client_data_dtor);
	assert(poor_wl_client_get_user_data(&p.client) == &order);
	poor_wl_client_destroy(&p.client);
	assert(order_len == 4 && order[0] == 1 && order[1] == 2 && order[2] == 3 && order[3] == 4);
	assert(!p.done);
	run_until(p.done);
	peer_close(&p);
	server_stop();
}

static void destroy_again(struct poor_wl_listener *, void *data)
{
	poor_wl_client_destroy(data);
}

TEST(client_destroy_from_destroy_listener)
{
	struct poor_wl_listener listener = { .notify = destroy_again };
	struct peer p;

	server_start();
	peer_connect(&p);
	poor_wl_client_add_destroy_listener(&p.client, &listener);
	poor_wl_client_destroy(&p.client);
	assert(logged("re-entrant client destruction"));
	run_until(p.done);
	peer_close(&p);
	server_stop();
}

static unsigned resources_created;

static void resource_created(struct poor_wl_listener *, void *data)
{
	assert(poor_wl_resource_get_interface(data) == &poor_test_child_interface);
	resources_created++;
}

static enum poor_wl_iterator_result count_resource(struct poor_wl_resource *, void *data)
{
	(*(unsigned *)data)++;
	return POOR_WL_ITERATOR_CONTINUE;
}

static enum poor_wl_iterator_result stop_at_second(struct poor_wl_resource *, void *data)
{
	return ++*(unsigned *)data == 2 ? POOR_WL_ITERATOR_STOP : POOR_WL_ITERATOR_CONTINUE;
}

/* The display resource comes first, then the resources with ids of the client and those of the server */
TEST(client_resources)
{
	struct poor_wl_listener listener = { .notify = resource_created };
	struct poor_test_child_resource children[3];
	unsigned count = 0;
	struct peer p;

	server_start();
	peer_connect(&p);
	poor_wl_client_add_resource_created_listener(&p.client, &listener);
	foreach_array_index(children, i)
		assert(!poor_test_child_resource_init(&children[i], &p.client, 1, i ? 0 : peer_new_id(&p)));
	assert(resources_created == 3);
	poor_wl_client_for_each_resource(&p.client, count_resource, &count);
	assert(count == 4);
	count = 0;
	poor_wl_client_for_each_resource(&p.client, stop_at_second, &count);
	assert(count == 2);
	peer_close(&p);
	server_stop();
}

TEST(client_accessors)
{
	struct poor_wl_client *client;
	struct peer p, q;
	unsigned count = 0;
	pid_t pid;
	uid_t uid;
	gid_t gid;
	int fds[2];

	server_start();
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
	peer_init(&p, fds[0], fds[1]);
	peer_connect(&q);
	assert(poor_wl_client_get_fd(&p.client) == fds[0]);
	assert(poor_wl_client_get_display(&p.client) == &display);
	run_until(p.client.ready);
	poor_wl_client_get_credentials(&p.client, &pid, &uid, &gid);
	assert(pid == getpid() && uid == getuid() && gid == getgid());
	poor_wl_client_get_credentials(&p.client, nullptr, nullptr, nullptr);
	poor_wl_client_for_each(client, poor_wl_display_get_client_list(&display))
		assert(client == (count++ ? &q.client : &p.client));
	assert(count == 2);
	assert(poor_wl_client_from_link(poor_wl_client_get_link(&q.client)) == &q.client);
	peer_close(&p);
	peer_close(&q);
	server_stop();
}

/* The first error is the one that the client gets */
TEST(client_errors)
{
	struct poor_test_child_resource child;
	struct peer a, b, c;
	struct event e;

	server_start();
	peer_connect(&a);
	peer_connect(&b);
	peer_connect(&c);
	poor_wl_client_post_no_memory(&a.client);
	e = expect_error(&a, 1, POOR_WL_DISPLAY_ERROR_NO_MEMORY);
	assert(!strcmp(error_message(&e), "no memory"));
	assert(!poor_test_child_resource_init(&child, &b.client, 1, peer_new_id(&b)));
	poor_wl_resource_post_no_memory(&child.resource);
	expect_error(&b, 1, POOR_WL_DISPLAY_ERROR_NO_MEMORY);
	poor_wl_client_post_implementation_error(&c.client, "broken %d", 7);
	poor_wl_client_post_no_memory(&c.client);
	e = expect_error(&c, 1, POOR_WL_DISPLAY_ERROR_IMPLEMENTATION);
	assert(!strcmp(error_message(&e), "broken 7"));
	peer_close(&a);
	peer_close(&b);
	peer_close(&c);
	server_stop();
}

static void client_destroyed(struct poor_wl_listener *, void *)
{
	record(1);
}

TEST(client_destroyed_before_credentials)
{
	struct poor_wl_listener listener = { .notify = client_destroyed };
	struct peer p;

	server_start();
	peer_connect(&p);
	poor_wl_client_add_destroy_listener(&p.client, &listener);
	poor_wl_client_destroy(&p.client);
	run_until(p.done);
	assert(order_len == 1 && !p.client.ready);
	peer_close(&p);
	server_stop();
}

TEST(client_hangup)
{
	struct peer p;

	server_start();
	peer_connect(&p);
	peer_roundtrip(&p);
	peer_close(&p);
	run_until(p.done);
	assert(!logged("error in client communication"));
	server_stop();
}

TEST(request_in_pieces)
{
	static const uint32_t sync[] = { 1, 12 << 16 | DISPLAY_SYNC, 2 };
	struct event e;
	struct peer p;

	server_start();
	peer_connect(&p);
	run_until(p.client.ready);
	assert(write_array(p.fd, arrview_first(10, arrview_bytes(sync))) == 10);
	run_for(20'000'000);
	assert(!readable(p.fd));
	assert(write_array(p.fd, arrview_cfront(10, arrview_bytes(sync))) == 2);
	e = peer_event(&p);
	assert(e.id == 2 && e.opcode == POOR_WL_CALLBACK_DONE);
	peer_close(&p);
	server_stop();
}

/* A client that stops reading makes the send of an event fail */
TEST(client_stops_reading)
{
	struct poor_wl_callback_resource callback;
	struct peer p;

	server_start();
	peer_connect(&p);
	assert(!shutdown(p.fd, SHUT_RD));
	assert(!poor_wl_callback_resource_init(&callback, &p.client, 1, 0));
	poor_wl_callback_send_done(&callback, 1);
	run_until(p.done);
	peer_close(&p);
	server_stop();
}

TEST(client_without_socket)
{
	struct peer p;
	int fds[2];

	server_start();
	assert(!pipe2(fds, O_CLOEXEC));
	peer_init(&p, fds[0], fds[1]);
	run_until(p.done);
	assert(logged("failed to get client credentials"));
	peer_close(&p);
	server_stop();
}

/*
 * Fds that come with requests without fd arguments wait in the connection. A recvmsg() takes the fds of one
 * sendmsg() at most, as many as fit its control buffer, so more of them fail the client, and so do sendmsg()s that
 * fill the queue.
 */
TEST(client_sends_too_many_fds)
{
	struct peer a, b;
	int fds[32];

	fill_array(fds, STDERR_FILENO);
	server_start();
	peer_connect(&a);
	peer_connect(&b);
	SEND_FDS(&a, arrview_first(POOR_WL_CONNECTION_MAX_FDS + 1, fds), 1, DISPLAY_SYNC, peer_new_id(&a));
	for (unsigned i = 0; i < POOR_WL_CONNECTION_FDS / POOR_WL_CONNECTION_MAX_FDS + 1; i++)
		SEND_FDS(&b, arrview_first(POOR_WL_CONNECTION_MAX_FDS, fds), 1, DISPLAY_SYNC, peer_new_id(&b));
	run_until(a.done && b.done);
	assert(logged("failed to read client connection") && logged("too many file descriptors from client"));
	peer_close(&a);
	peer_close(&b);
	server_stop();
}

static struct peer late_peer;

static void connect_another(struct poor_wl_listener *, void *)
{
	peer_connect(&late_peer);
}

TEST(clients_created_while_destroying_clients)
{
	struct poor_wl_listener listener = { .notify = connect_another };
	struct peer p;

	server_start();
	peer_connect(&p);
	poor_wl_client_add_destroy_listener(&p.client, &listener);
	poor_wl_display_destroy_clients(&display);
	assert(logged("cannot destroy all clients"));
	run_until(p.done);
	assert(!late_peer.done);
	peer_close(&p);
	peer_close(&late_peer);
	server_stop();
}

static void display_destroy_notify(struct poor_wl_listener *, void *data)
{
	assert(data == &display && !display_done);
	record(1);
}

TEST(display_accessors)
{
	struct poor_wl_listener listener = { .notify = display_destroy_notify };
	uint32_t serial;

	server_start();
	assert(poor_wl_display_get_loop(&display) == &loop);
	serial = poor_wl_display_get_serial(&display);
	assert(poor_wl_display_next_serial(&display) == serial + 1 && poor_wl_display_get_serial(&display) == serial + 1);
	poor_wl_display_add_destroy_listener(&display, &listener);
	assert(poor_wl_display_get_destroy_listener(&display, display_destroy_notify) == &listener);
	assert(!poor_wl_display_get_destroy_listener(&display, other_notify));
	server_stop();
	assert(order_len == 1);
}

/* A queued event waits for one that is posted, or for a flush */
TEST(queued_events)
{
	struct poor_wl_callback_resource a, b, c;
	struct event e;
	struct peer p;

	server_start();
	peer_connect(&p);
	assert(!poor_wl_callback_resource_init(&a, &p.client, 1, 0));
	assert(!poor_wl_callback_resource_init(&b, &p.client, 1, 0));
	assert(!poor_wl_callback_resource_init(&c, &p.client, 1, 0));
	poor_wl_resource_queue_event(&a.resource, POOR_WL_CALLBACK_DONE, 1);
	run_for(20'000'000);
	assert(!readable(p.fd));
	poor_wl_callback_send_done(&b, 2);
	e = peer_event(&p);
	assert(e.id == poor_wl_resource_get_id(&a.resource) && e.args[0] == 1);
	e = peer_event(&p);
	assert(e.id == poor_wl_resource_get_id(&b.resource) && e.args[0] == 2);
	poor_wl_callback_send_done(&c, 3);
	poor_wl_display_flush_clients(&display);
	e = peer_event(&p);
	assert(e.id == poor_wl_resource_get_id(&c.resource) && e.args[0] == 3);
	peer_close(&p);
	server_stop();
}

static bool hide_global(const struct poor_wl_client *, const struct poor_wl_global *global, void *data)
{
	return global != data;
}

TEST(global_accessors)
{
	struct poor_wl_global global;
	struct peer p;
	int data;

	server_start();
	peer_connect(&p);
	poor_wl_global_init(&global, &display, &poor_test_global_interface, 2, &data, bind_nothing);
	assert(poor_wl_global_get_interface(&global) == &poor_test_global_interface);
	assert(poor_wl_global_get_version(&global) == 2);
	assert(poor_wl_global_get_display(&global) == &display);
	assert(poor_wl_global_get_user_data(&global) == &data);
	poor_wl_global_set_user_data(&global, &p);
	assert(poor_wl_global_get_user_data(&global) == &p);
	assert(poor_wl_global_get_name(&global, &p.client));
	poor_wl_display_set_global_filter(&display, hide_global, &global);
	assert(!poor_wl_global_get_name(&global, &p.client));
	poor_wl_global_destroy(&global);
	peer_close(&p);
	server_stop();
}

static struct event expect_global(struct peer *p, uint32_t registry, const struct poor_wl_global *global, const char *interface)
{
	struct event e = peer_event(p);

	assert(e.id == registry && e.opcode == POOR_WL_REGISTRY_GLOBAL && e.args[0] == poor_wl_global_get_name(global, &p->client));
	assert(!strcmp((const char *)(e.args + 2), interface));
	assert(*skip_string(e.args + 1) == poor_wl_global_get_version(global));
	return e;
}

/* Registries hear about globals that come and go, unless the filter hides them */
TEST(registry_announcements)
{
	struct poor_wl_global first, second, hidden;
	uint32_t registry, first_name;
	struct peer p;
	struct event e;

	server_start();
	poor_wl_display_set_global_filter(&display, hide_global, &hidden);
	poor_wl_global_init(&first, &display, &poor_test_global_interface, 2, nullptr, bind_nothing);
	peer_connect(&p);
	first_name = poor_wl_global_get_name(&first, &p.client);
	registry = peer_get_registry(&p);
	expect_global(&p, registry, &first, "test_global");
	poor_wl_global_init(&hidden, &display, &poor_test_child_interface, 1, nullptr, bind_nothing);
	poor_wl_global_init(&second, &display, &poor_test_child_interface, 1, nullptr, bind_nothing);
	expect_global(&p, registry, &second, "test_child");
	poor_wl_global_destroy(&first);
	e = peer_event(&p);
	assert(e.id == registry && e.opcode == POOR_WL_REGISTRY_GLOBAL_REMOVE && e.args[0] == first_name);
	poor_wl_global_destroy(&hidden);
	poor_wl_global_remove(&second);
	e = peer_event(&p);
	assert(e.opcode == POOR_WL_REGISTRY_GLOBAL_REMOVE && e.args[0] == poor_wl_global_get_name(&second, &p.client));
	poor_wl_global_destroy(&second);
	peer_roundtrip(&p);
	peer_close(&p);
	server_stop();
}

static void withdrawn(struct poor_wl_global *global)
{
	record(1);
	poor_wl_global_destroy(global);
}

TEST(global_withdrawn_without_registries)
{
	struct poor_wl_global global;

	server_start();
	poor_wl_global_init(&global, &display, &poor_test_global_interface, 1, nullptr, bind_nothing);
	poor_wl_global_set_withdrawn_listener(&global, withdrawn);
	poor_wl_global_remove(&global);
	assert(order_len == 1);
	server_stop();
}

FAIL_TEST(global_removed_twice)
{
	struct poor_wl_global global;

	server_start();
	poor_wl_global_init(&global, &display, &poor_test_global_interface, 1, nullptr, bind_nothing);
	poor_wl_global_remove(&global);
	poor_wl_global_remove(&global);
}

TEST(registry_bind_errors)
{
	struct poor_wl_global global;
	uint32_t registry, name;
	struct peer p[3];
	struct event e;

	server_start();
	poor_wl_global_init(&global, &display, &poor_test_global_interface, 2, nullptr, bind_nothing);
	foreach_array_index(p, i) {
		peer_connect(&p[i]);
		name = poor_wl_global_get_name(&global, &p[i].client);
		registry = peer_get_registry(&p[i]);
		peer_bind(&p[i], registry, i == 0 ? name + 1 : name, i == 1 ? "test_child" : "test_global", i == 2 ? 0 : 1);
		expect_global(&p[i], registry, &global, "test_global");
		e = expect_error(&p[i], registry, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT);
		assert(strstr(error_message(&e), i == 0 ? "is unavailable" : i == 1 ? "invalid interface" : "0 is not a valid version"));
		peer_close(&p[i]);
	}
	poor_wl_global_destroy(&global);
	server_stop();
}

static void fixes_destroy(struct poor_wl_client *, struct poor_wl_fixes_resource *resource)
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
	fixes_destroy,
	fixes_destroy_registry,
	fixes_ack_global_remove,
};

TEST(ack_of_global_that_is_not_removed)
{
	struct poor_wl_fixes_resource fixes;
	struct poor_wl_global global;
	uint32_t registry, id;
	struct event e;
	struct peer p;

	server_start();
	poor_wl_global_init(&global, &display, &poor_test_global_interface, 1, nullptr, bind_nothing);
	peer_connect(&p);
	registry = peer_get_registry(&p);
	peer_roundtrip(&p);
	id = peer_new_id(&p);
	assert(!poor_wl_fixes_resource_init(&fixes, &p.client, 2, id));
	poor_wl_fixes_resource_set_implementation(&fixes, &fixes_impl, nullptr, nullptr);
	SEND(&p, id, FIXES_ACK_GLOBAL_REMOVE, registry, poor_wl_global_get_name(&global, &p.client));
	e = expect_error(&p, id, POOR_WL_FIXES_ERROR_INVALID_ACK_REMOVE);
	assert(strstr(error_message(&e), "is not removed"));
	peer_close(&p);
	poor_wl_global_destroy(&global);
	server_stop();
}

TEST(registry_without_memory)
{
	struct peer p;

	server_start();
	alloc_budget = 0;
	peer_connect(&p);
	peer_get_registry(&p);
	expect_error(&p, 1, POOR_WL_DISPLAY_ERROR_NO_MEMORY);
	peer_close(&p);
	server_stop();
}

TEST(invalid_requests)
{
	static const uint32_t too_large[] = { 1, (POOR_WL_MAX_MESSAGE_SIZE + 4) << 16 | DISPLAY_SYNC };
	struct poor_wl_fixes_resource fixes;
	struct peer p[5];
	struct event e;

	server_start();
	foreach_array_ref(p, peer)
		peer_connect(peer);
	assert(write_array(p[0].fd, too_large) == (ssize_t)ARRAY_SIZE_BYTES(too_large));
	SEND(&p[1], 7, 0);
	SEND(&p[2], 1, 5);
	SEND(&p[3], 1, DISPLAY_SYNC);
	assert(!poor_wl_fixes_resource_init(&fixes, &p[4].client, 1, peer_new_id(&p[4])));
	SEND(&p[4], 2, FIXES_DESTROY_REGISTRY, 1);
	e = expect_error(&p[0], 1, POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
	assert(!strcmp(error_message(&e), "message length 4100 exceeds 4096"));
	e = expect_error(&p[1], 1, POOR_WL_DISPLAY_ERROR_INVALID_OBJECT);
	assert(!strcmp(error_message(&e), "invalid object 7"));
	e = expect_error(&p[2], 1, POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
	assert(!strcmp(error_message(&e), "invalid method 5, object wl_display#1"));
	e = expect_error(&p[3], 1, POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
	assert(!strcmp(error_message(&e), "invalid arguments for wl_display#1.sync"));
	e = expect_error(&p[4], 1, POOR_WL_DISPLAY_ERROR_INVALID_METHOD);
	assert(!strcmp(error_message(&e), "invalid arguments for wl_fixes#2.destroy_registry"));
	foreach_array_ref(p, peer)
		peer_close(peer);
	server_stop();
}

static int dispatched_opcode = -1;

static int dispatch(const void *implementation, void *target, uint32_t opcode, const struct poor_wl_message *message,
		    union poor_wl_argument *)
{
	struct poor_test_child_resource *child = container_of((struct poor_wl_object *)target, struct poor_test_child_resource,
							      resource.object);

	assert(implementation == &child_impl && poor_wl_resource_get_id(&child->resource) == 2);
	assert(!strcmp(message->name, "ping"));
	dispatched_opcode = opcode;
	return 0;
}

TEST(resource_dispatcher)
{
	struct poor_test_child_resource child;
	struct peer p;
	int data;

	server_start();
	peer_connect(&p);
	assert(!poor_test_child_resource_init(&child, &p.client, 1, peer_new_id(&p)));
	poor_wl_resource_set_dispatcher(&child.resource, dispatch, &child_impl, &data, nullptr);
	assert(poor_wl_resource_get_user_data(&child.resource) == &data);
	SEND(&p, 2, CHILD_PING);
	run_until(dispatched_opcode == CHILD_PING);
	peer_close(&p);
	server_stop();
}

FAIL_TEST(request_to_resource_without_implementation)
{
	struct poor_test_child_resource child;
	struct peer p;

	server_start();
	peer_connect(&p);
	assert(!poor_test_child_resource_init(&child, &p.client, 1, peer_new_id(&p)));
	SEND(&p, 2, CHILD_PING);
	run_until(p.done);
}

FAIL_TEST(request_without_handler)
{
	static const struct poor_test_child_interface partial = { child_destroy_request, nullptr };
	struct poor_test_child_resource child;
	struct peer p;

	server_start();
	peer_connect(&p);
	assert(!poor_test_child_resource_init(&child, &p.client, 1, peer_new_id(&p)));
	poor_test_child_resource_set_implementation(&child, &partial, nullptr, nullptr);
	SEND(&p, 2, CHILD_PING);
	run_until(p.done);
}

/* An object of another client in an event is a bug of the compositor, the client is dropped */
TEST(event_with_object_of_other_client)
{
	struct poor_wl_surface_resource surface;
	struct poor_wl_output_resource output;
	struct peer p, q;

	server_start();
	peer_connect(&p);
	peer_connect(&q);
	assert(!poor_wl_surface_resource_init(&surface, &p.client, 1, 0));
	assert(!poor_wl_output_resource_init(&output, &q.client, 1, 0));
	poor_wl_surface_send_enter(&surface, &output);
	assert(logged("compositor bug"));
	run_until(p.done);
	assert(!q.done && !readable(q.fd));
	peer_close(&p);
	peer_close(&q);
	server_stop();
}

static unsigned logged_messages;

static void count_message(void *, enum poor_wl_protocol_logger_type, const struct poor_wl_protocol_logger_message *)
{
	logged_messages++;
}

/* A roundtrip is the sync request, the done event and the delete_id event */
TEST(protocol_logger_destroy)
{
	struct poor_wl_protocol_logger logger;
	struct peer p;

	server_start();
	poor_wl_display_add_protocol_logger(&display, &logger, count_message, nullptr);
	peer_connect(&p);
	peer_roundtrip(&p);
	assert(logged_messages == 3);
	poor_wl_protocol_logger_destroy(&logger);
	peer_roundtrip(&p);
	assert(logged_messages == 3);
	peer_close(&p);
	server_stop();
}

/* NO_COLOR wins over FORCE_COLOR */
TEST(debug_log)
{
	struct poor_test_global_resource global;
	char bytes[4], line[128];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(bytes);
	struct peer p;
	int fd = stderr_copy();

	setenv("WAYLAND_DEBUG", "thread_id,server", 1);
	setenv("FORCE_COLOR", "1", 1);
	setenv("NO_COLOR", "1", 1);
	server_start();
	peer_connect(&p);
	peer_roundtrip(&p);
	assert(logged("wl_display#1.sync(new id wl_callback#2)") && logged(" -> wl_callback#2.done(0)"));
	assert(logged(" -> wl_display#1.delete_id(2)") && logged("TID#") && !logged("\033["));
	assert(!poor_test_global_resource_init(&global, &p.client, 1, peer_new_id(&p)));
	assert(poor_wl_array_add(&array, 3));
	poor_test_global_send_echoed(&global, -1, 1, poor_wl_fixed_from_double(-1.5), "s", nullptr, &global, &array, fd);
	peer_event(&p);
	snprintf_array(line, " -> test_global#3.echoed(-1, 1, -1.50000000, \"s\", nil, test_global#3, array[3], fd %d)", fd);
	assert(logged(line));
	peer_close(&p);
	server_stop();
}

static void socket_ready(struct poor_wl_socket *socket)
{
	struct test_socket *s = container_of(socket, struct test_socket, socket);
	struct rlimit limit = fd_limit;
	int fd = 0;

	s->ready = true;
	if (s->destroy_when_ready)
		poor_wl_socket_destroy(socket);
	if (s->limit_fds_when_ready) {
		while (fcntl(fd, F_GETFD) >= 0)
			fd++;
		limit.rlim_cur = fd;
		assert(!setrlimit(RLIMIT_NOFILE, &limit));
	}
}

static void socket_accept(struct poor_wl_socket *socket, int fd)
{
	container_of(socket, struct test_socket, socket)->accepted++;
	poor_loop_close_fd_or_submit(&loop, fd, 0);
}

static void socket_done(struct poor_wl_socket *socket)
{
	container_of(socket, struct test_socket, socket)->done = true;
}

static void socket_init(struct test_socket *s)
{
	s->accepted = 0;
	s->ready = s->done = s->destroy_when_ready = s->limit_fds_when_ready = false;
	poor_wl_socket_init(&s->socket, &display, socket_ready, socket_accept, socket_done);
}

static void socket_close(struct test_socket *s)
{
	poor_wl_socket_destroy(&s->socket);
	poor_wl_socket_destroy(&s->socket);
	run_until(s->done);
}

static void runtime_path_impl(size_t size, char (*path)[size], const char *name)
{
	snprintf_array(path, "%s/%s", getenv("XDG_RUNTIME_DIR"), name);
}

#define runtime_path(path, name) runtime_path_impl(ARRAY_SIZE(path), &auto_arr(path), name)

static void connect_socket(int fd, const char *name)
{
	struct sockaddr_un addr = { .sun_family = AF_LOCAL };

	runtime_path(addr.sun_path, name);
	assert(!connect(fd, (struct sockaddr *)&addr, sizeof(addr)));
}

static int connect_to(const char *name)
{
	int fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);

	assert(fd >= 0);
	connect_socket(fd, name);
	return fd;
}

TEST(socket_names)
{
	char name[200], *runtime_dir = strdup(getenv("XDG_RUNTIME_DIR"));
	struct test_socket s;

	server_start();
	memset_array(arrview_str(name), 'a');
	*array_last_ref(name) = '\0';
	socket_init(&s);
	assert(poor_wl_socket_listen(&s.socket, name) == -1 && errno == ENAMETOOLONG);
	setenv("WAYLAND_DISPLAY", "wayland-env", 1);
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, nullptr));
	run_until(s.ready);
	assert(!strcmp(poor_wl_socket_get_name(&s.socket), "wayland-env"));
	socket_close(&s);
	unsetenv("WAYLAND_DISPLAY");
	socket_init(&s);
	s.destroy_when_ready = true;
	assert(!poor_wl_socket_listen(&s.socket, nullptr));
	run_until(s.done);
	assert(s.ready && !strcmp(poor_wl_socket_get_name(&s.socket), "wayland-0"));
	unsetenv("XDG_RUNTIME_DIR");
	socket_init(&s);
	assert(poor_wl_socket_listen(&s.socket, "wayland-1") == -1 && errno == ENOENT);
	assert(poor_wl_socket_listen_auto(&s.socket) == -1 && errno == ENOENT);
	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	free(runtime_dir);
	server_stop();
}

/* A writable file in the way is left over by a compositor that held the lock, a read-only one is not removed */
TEST(socket_files_in_the_way)
{
	char stale[256], other[256];
	struct test_socket s;

	runtime_path(stale, "wayland-stale");
	runtime_path(other, "wayland-other");
	server_start();
	poor_loop_close_fd_or_submit(&loop, open(stale, O_CREAT | O_CLOEXEC | O_WRONLY, 0600), 0);
	poor_loop_close_fd_or_submit(&loop, open(other, O_CREAT | O_CLOEXEC | O_WRONLY, 0400), 0);
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, "wayland-stale"));
	run_until(s.ready);
	socket_close(&s);
	assert(!s.socket.error && access(stale, F_OK));
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, "wayland-other"));
	run_until(s.done);
	assert(!s.ready && s.socket.error == EADDRINUSE && logged("bind() failed"));
	server_stop();
}

TEST(socket_without_lock_file)
{
	struct test_socket s;

	server_start();
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, "/nonexistent/wayland-0"));
	run_until(s.done);
	assert(!s.ready && s.socket.error == ENOENT && logged("unable to open lockfile"));
	server_stop();
}

static int hold_lock(const char *name)
{
	char path[256];
	int fd;

	snprintf_array(path, "%s/%s.lock", getenv("XDG_RUNTIME_DIR"), name);
	fd = open(path, O_CREAT | O_CLOEXEC | O_RDWR, 0600);
	assert(fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB));
	return fd;
}

static void release_lock(const char *name, int fd)
{
	char path[256];

	snprintf_array(path, "%s/%s.lock", getenv("XDG_RUNTIME_DIR"), name);
	assert(!unlink(path));
	poor_loop_close_fd_or_submit(&loop, fd, 0);
}

TEST(socket_auto_without_free_names)
{
	struct test_socket s;
	char name[32];
	int locks[33];

	server_start();
	foreach_array_index(locks, i) {
		snprintf_array(name, "wayland-%zu", i);
		locks[i] = hold_lock(name);
	}
	socket_init(&s);
	assert(!poor_wl_socket_listen_auto(&s.socket));
	run_until(s.done);
	assert(!s.ready && s.socket.error == EINVAL);
	foreach_array_index(locks, i) {
		snprintf_array(name, "wayland-%zu", i);
		release_lock(name, locks[i]);
	}
	server_stop();
}

/* The lock file may already exist when the socket is destroyed */
TEST(socket_destroyed_while_starting)
{
	char lock[256];
	struct test_socket s;

	runtime_path(lock, "wayland-early.lock");
	server_start();
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, "wayland-early"));
	socket_close(&s);
	assert(!s.ready);
	unlink(lock);
	server_stop();
}

/* The accept fails while there are no fds, and is tried again later */
TEST(socket_accept_without_fds)
{
	struct test_socket s;
	int fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);

	assert(fd >= 0 && !getrlimit(RLIMIT_NOFILE, &fd_limit));
	server_start();
	socket_init(&s);
	s.limit_fds_when_ready = true;
	assert(!poor_wl_socket_listen(&s.socket, "wayland-emfile"));
	run_until(s.ready);
	connect_socket(fd, "wayland-emfile");
	run_until(logged("failed to accept: Too many open files"));
	assert(!setrlimit(RLIMIT_NOFILE, &fd_limit));
	run_until(s.accepted == 1);
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	socket_close(&s);
	server_stop();
}

/* The lock file takes the last fd, so there is none left for the socket */
TEST(socket_without_fds)
{
	struct rlimit limit;
	struct test_socket s;
	int fd = 0;

	assert(!getrlimit(RLIMIT_NOFILE, &fd_limit));
	server_start();
	socket_init(&s);
	while (fcntl(fd, F_GETFD) >= 0)
		fd++;
	limit = fd_limit;
	limit.rlim_cur = fd + 1;
	assert(!setrlimit(RLIMIT_NOFILE, &limit));
	assert(!poor_wl_socket_listen(&s.socket, "wayland-nofd"));
	run_until(s.done);
	assert(!setrlimit(RLIMIT_NOFILE, &fd_limit));
	assert(!s.ready && s.socket.error == EMFILE);
	server_stop();
}

TEST(socket_accept_fails)
{
	struct test_socket s;
	int fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);

	assert(fd >= 0);
	server_start();
	socket_init(&s);
	poor_wl_socket_listen_fd(&s.socket, fd);
	run_until(s.done);
	assert(s.socket.error == EINVAL && !s.accepted);
	server_stop();
}

TEST(socket_accepts_clients)
{
	struct test_socket s;
	int fds[3];

	server_start();
	socket_init(&s);
	assert(!poor_wl_socket_listen(&s.socket, "wayland-accept"));
	run_until(s.ready);
	foreach_array_ref(fds, fd)
		*fd = connect_to("wayland-accept");
	run_until(s.accepted == ARRAY_SIZE(fds));
	foreach_array_const_ref(fds, fd)
		poor_loop_close_fd_or_submit(&loop, *fd, 0);
	server_stop();
	assert(s.done);
}

static int memfd(size_t size)
{
	int fd = memfd_create("server-test", MFD_CLOEXEC);

	assert(fd >= 0 && !ftruncate(fd, size));
	return fd;
}

static uint32_t peer_create_pool(struct peer *p, uint32_t shm, int fd, int32_t size)
{
	uint32_t id = peer_new_id(p);

	SEND_FDS(p, array_ptr(&fd), shm, SHM_CREATE_POOL, id, (uint32_t)size);
	return id;
}

static uint32_t peer_create_buffer(struct peer *p, uint32_t pool, int32_t offset, int32_t height, uint32_t format)
{
	uint32_t id = peer_new_id(p);

	SEND(p, pool, SHM_POOL_CREATE_BUFFER, id, (uint32_t)offset, 16, (uint32_t)height, 64, format);
	return id;
}

static struct poor_wl_shm_buffer *server_buffer(struct peer *p, uint32_t id)
{
	struct poor_wl_resource *resource = poor_wl_client_get_object(&p->client, id);

	assert(resource);
	return poor_wl_shm_buffer_get(container_of(resource, struct poor_wl_buffer_resource, resource));
}

static void shm_start(void)
{
	server_start();
	poor_wl_display_init_shm(&display, &shm_global);
}

static uint32_t peer_bind_shm(struct peer *p, uint32_t version)
{
	uint32_t registry = peer_get_registry(p);

	return peer_bind(p, registry, poor_wl_global_get_name(&shm_global, &p->client), "wl_shm", version);
}

/* The pool has 'size' bytes, its first word is 1 and the word at 4096 is 2 */
static struct poor_wl_shm_buffer *shm_client_init(struct shm_client *c, size_t size)
{
	c->size = size;
	c->fd = memfd(size);
	c->data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	assert(c->data != MAP_FAILED);
	c->data[0] = 1;
	if (size > 4096)
		c->data[1024] = 2;
	peer_connect(&c->peer);
	c->shm = peer_bind_shm(&c->peer, 2);
	c->pool = peer_create_pool(&c->peer, c->shm, c->fd, size);
	c->buffer = peer_create_buffer(&c->peer, c->pool, 0, 16, POOR_WL_SHM_FORMAT_ARGB8888);
	peer_roundtrip(&c->peer);
	return server_buffer(&c->peer, c->buffer);
}

static void shm_client_fini(struct shm_client *c)
{
	munmap(c->data, c->size);
	poor_loop_close_fd_or_submit(&loop, c->fd, 0);
	peer_close(&c->peer);
}

static uint32_t read_pixel(struct poor_wl_shm_buffer *buffer)
{
	uint32_t pixel;

	poor_wl_shm_buffer_begin_access(buffer);
	pixel = *(volatile uint32_t *)poor_wl_shm_buffer_get_data(buffer);
	poor_wl_shm_buffer_end_access(buffer);
	return pixel;
}

TEST(shm_formats)
{
	static const uint32_t formats[] = { POOR_WL_SHM_FORMAT_RGB565, POOR_WL_SHM_FORMAT_XBGR8888 };
	static const uint32_t expected[] = { POOR_WL_SHM_FORMAT_ARGB8888, POOR_WL_SHM_FORMAT_XRGB8888, POOR_WL_SHM_FORMAT_RGB565,
					     POOR_WL_SHM_FORMAT_XBGR8888 };
	uint32_t shm;
	struct peer p;
	struct event e;

	shm_start();
	poor_wl_display_set_shm_formats(&display, formats);
	peer_connect(&p);
	shm = peer_bind_shm(&p, 1);
	e = peer_event(&p);
	assert(e.opcode == POOR_WL_REGISTRY_GLOBAL);
	foreach_array_const_ref(expected, format) {
		e = peer_event(&p);
		assert(e.id == shm && e.opcode == POOR_WL_SHM_FORMAT && e.args[0] == *format);
	}
	peer_close(&p);
	server_stop();
}

TEST(shm_buffers)
{
	static const uint32_t formats[] = { POOR_WL_SHM_FORMAT_RGB565 };
	struct poor_wl_buffer_resource other;
	struct poor_wl_shm_buffer *buffer;
	struct shm_client c;
	uint32_t id;

	shm_start();
	poor_wl_display_set_shm_formats(&display, formats);
	buffer = shm_client_init(&c, 8192);
	assert(buffer && poor_wl_shm_buffer_get_width(buffer) == 16 && poor_wl_shm_buffer_get_height(buffer) == 16);
	assert(poor_wl_shm_buffer_get_stride(buffer) == 64 && poor_wl_shm_buffer_get_format(buffer) == POOR_WL_SHM_FORMAT_ARGB8888);
	poor_wl_shm_buffer_begin_access(buffer);
	assert(read_pixel(buffer) == 1);
	poor_wl_shm_buffer_end_access(buffer);
	id = peer_create_buffer(&c.peer, c.pool, 4096, 16, POOR_WL_SHM_FORMAT_RGB565);
	peer_roundtrip(&c.peer);
	buffer = server_buffer(&c.peer, id);
	assert(read_pixel(buffer) == 2 && poor_wl_shm_buffer_get_format(buffer) == POOR_WL_SHM_FORMAT_RGB565);
	assert(!poor_wl_shm_buffer_get(nullptr));
	assert(!poor_wl_buffer_resource_init(&other, &c.peer.client, 1, 0));
	assert(!poor_wl_shm_buffer_get(&other));
	poor_wl_resource_destroy(&other.resource);
	SEND(&c.peer, id, BUFFER_DESTROY);
	SEND(&c.peer, c.buffer, BUFFER_DESTROY);
	SEND(&c.peer, c.pool, SHM_POOL_DESTROY);
	SEND(&c.peer, c.shm, SHM_RELEASE);
	peer_roundtrip(&c.peer);
	assert(!poor_wl_client_get_object(&c.peer.client, c.pool) && !poor_wl_client_get_object(&c.peer.client, c.shm));
	shm_client_fini(&c);
	server_stop();
}

TEST(shm_pool_resize)
{
	struct shm_client c;
	struct event e;
	uint32_t id;

	shm_start();
	shm_client_init(&c, 4096);
	assert(!ftruncate(c.fd, 8192));
	SEND(&c.peer, c.pool, SHM_POOL_RESIZE, 8192);
	id = peer_create_buffer(&c.peer, c.pool, 4096, 16, POOR_WL_SHM_FORMAT_XRGB8888);
	peer_roundtrip(&c.peer);
	assert(server_buffer(&c.peer, id));
	SEND(&c.peer, c.pool, SHM_POOL_RESIZE, 4096);
	e = expect_error(&c.peer, c.pool, POOR_WL_SHM_ERROR_INVALID_FD);
	assert(!strcmp(error_message(&e), "shrinking pool invalid"));
	shm_client_fini(&c);
	server_stop();
}

/* While the compositor holds the mapping, a resize waits: pointers into the pool stay valid */
TEST(shm_resize_waits_for_mapping)
{
	struct poor_wl_shm_mapping *mapping;
	struct poor_wl_shm_buffer *buffer;
	struct shm_client c;
	void *data;
	uint32_t id;

	shm_start();
	buffer = shm_client_init(&c, 4096);
	mapping = poor_wl_shm_buffer_ref_mapping(buffer);
	assert(poor_wl_shm_buffer_ref_mapping(buffer) == mapping);
	data = poor_wl_shm_buffer_get_data(buffer);
	assert(!ftruncate(c.fd, 8192));
	SEND(&c.peer, c.pool, SHM_POOL_RESIZE, 8192);
	peer_roundtrip(&c.peer);
	assert(poor_wl_shm_buffer_get_data(buffer) == data && logged("deferred resize pending"));
	poor_wl_shm_mapping_unref(mapping);
	poor_wl_shm_mapping_unref(mapping);
	id = peer_create_buffer(&c.peer, c.pool, 4096, 16, POOR_WL_SHM_FORMAT_XRGB8888);
	peer_roundtrip(&c.peer);
	assert(server_buffer(&c.peer, id));
	shm_client_fini(&c);
	server_stop();
}

/* References keep a buffer and its pool after their resources and their client are gone */
TEST(shm_references)
{
	struct poor_wl_shm_mapping *mapping;
	struct poor_wl_shm_buffer *buffer;
	struct shm_client c;

	shm_start();
	buffer = shm_client_init(&c, 4096);
	assert(poor_wl_shm_buffer_ref(buffer) == buffer && poor_wl_shm_buffer_ref(buffer) == buffer);
	mapping = poor_wl_shm_buffer_ref_mapping(buffer);
	SEND(&c.peer, c.buffer, BUFFER_DESTROY);
	SEND(&c.peer, c.pool, SHM_POOL_DESTROY);
	peer_roundtrip(&c.peer);
	assert(!poor_wl_client_get_object(&c.peer.client, c.buffer));
	poor_wl_shm_buffer_unref(buffer);
	assert(poor_wl_shm_buffer_get_width(buffer) == 16);
	poor_wl_client_destroy(&c.peer.client);
	run_until(c.peer.done);
	assert(read_pixel(buffer) == 1);
	poor_wl_shm_buffer_unref(buffer);
	poor_wl_shm_mapping_unref(mapping);
	shm_client_fini(&c);
	server_stop();
}

/*
 * A pool that the client truncated makes accesses fault. The fault maps zeroes over the pool and the client gets an
 * error, if it still exists.
 */
TEST(shm_truncated_pool)
{
	struct poor_wl_shm_buffer *buffers[3];
	struct shm_client c[3];
	struct event e;

	shm_start();
	foreach_array_index(c, i) {
		buffers[i] = shm_client_init(&c[i], 4096);
		poor_wl_shm_buffer_ref(buffers[i]);
		assert(!ftruncate(c[i].fd, 0));
	}
	assert(read_pixel(buffers[0]) == 0);
	expect_error(&c[0].peer, c[0].buffer, POOR_WL_SHM_ERROR_INVALID_FD);
	SEND(&c[1].peer, c[1].buffer, BUFFER_DESTROY);
	peer_roundtrip(&c[1].peer);
	assert(read_pixel(buffers[1]) == 0);
	e = expect_error(&c[1].peer, 1, POOR_WL_DISPLAY_ERROR_IMPLEMENTATION);
	assert(strstr(error_message(&e), "already been destroyed"));
	poor_wl_client_destroy(&c[2].peer.client);
	run_until(c[2].peer.done);
	assert(read_pixel(buffers[2]) == 0);
	foreach_array_index(c, i) {
		poor_wl_shm_buffer_unref(buffers[i]);
		shm_client_fini(&c[i]);
	}
	server_stop();
}

TEST(shm_errors)
{
	struct shm_client c[3];
	char message[64];
	uint32_t shm[2];
	struct event e;
	struct peer p[2];
	int fds[2];

	shm_start();
	peer_connect(&p[0]);
	shm[0] = peer_bind_shm(&p[0], 1);
	peer_create_pool(&p[0], shm[0], STDERR_FILENO, 0);
	e = expect_error(&p[0], shm[0], POOR_WL_SHM_ERROR_INVALID_STRIDE);
	assert(!strcmp(error_message(&e), "invalid size (0)"));
	assert(!pipe2(fds, O_CLOEXEC));
	peer_connect(&p[1]);
	shm[1] = peer_bind_shm(&p[1], 1);
	peer_create_pool(&p[1], shm[1], fds[0], 4096);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	poor_loop_close_fd_or_submit(&loop, fds[1], 0);
	expect_error(&p[1], shm[1], POOR_WL_SHM_ERROR_INVALID_FD);
	foreach_array_ref(c, client)
		shm_client_init(client, 4096);
	peer_create_buffer(&c[0].peer, c[0].pool, 0, 16, POOR_WL_SHM_FORMAT_RGB565);
	e = expect_error(&c[0].peer, c[0].pool, POOR_WL_SHM_ERROR_INVALID_FORMAT);
	snprintf_array(message, "invalid format 0x%x", POOR_WL_SHM_FORMAT_RGB565);
	assert(!strcmp(error_message(&e), message));
	peer_create_buffer(&c[1].peer, c[1].pool, 0, 65, POOR_WL_SHM_FORMAT_ARGB8888);
	expect_error(&c[1].peer, c[1].pool, POOR_WL_SHM_ERROR_INVALID_STRIDE);
	peer_create_buffer(&c[2].peer, c[2].pool, -4, 16, POOR_WL_SHM_FORMAT_ARGB8888);
	expect_error(&c[2].peer, c[2].pool, POOR_WL_SHM_ERROR_INVALID_STRIDE);
	foreach_array_ref(c, client)
		shm_client_fini(client);
	peer_close(&p[0]);
	peer_close(&p[1]);
	server_stop();
}

/* The allocator fails for the wl_shm, the pool and the buffer in turn, after the registry got its memory */
TEST(shm_without_memory)
{
	struct peer p[3];
	uint32_t shm, pool;
	int fd = memfd(4096);

	shm_start();
	foreach_array_index(p, i) {
		peer_connect(&p[i]);
		alloc_budget = i + 1;
		shm = peer_bind_shm(&p[i], 1);
		if (i > 0) {
			pool = peer_create_pool(&p[i], shm, fd, 4096);
			if (i > 1)
				peer_create_buffer(&p[i], pool, 0, 16, POOR_WL_SHM_FORMAT_ARGB8888);
		}
		expect_error(&p[i], 1, POOR_WL_DISPLAY_ERROR_NO_MEMORY);
		peer_close(&p[i]);
	}
	alloc_budget = -1;
	poor_loop_close_fd_or_submit(&loop, fd, 0);
	server_stop();
}

FAIL_TEST(shm_access_of_two_pools)
{
	struct poor_wl_shm_buffer *a, *b;
	struct shm_client c[2];

	shm_start();
	a = shm_client_init(&c[0], 4096);
	b = shm_client_init(&c[1], 4096);
	poor_wl_shm_buffer_begin_access(a);
	poor_wl_shm_buffer_begin_access(b);
}

FAIL_TEST(shm_end_access_without_begin)
{
	struct shm_client c;

	shm_start();
	poor_wl_shm_buffer_end_access(shm_client_init(&c, 4096));
}

FAIL_TEST(shm_buffer_unref_without_ref)
{
	struct shm_client c;

	shm_start();
	poor_wl_shm_buffer_unref(shm_client_init(&c, 4096));
}

FAIL_TEST(shm_mapping_unref_without_ref)
{
	struct poor_wl_shm_mapping *mapping;
	struct shm_client c;

	shm_start();
	mapping = poor_wl_shm_buffer_ref_mapping(shm_client_init(&c, 4096));
	poor_wl_shm_mapping_unref(mapping);
	poor_wl_shm_mapping_unref(mapping);
}

/* A SIGBUS that has nothing to do with a pool kills the process */
FAIL_TEST(shm_unrelated_sigbus)
{
	struct shm_client c;

	shm_start();
	read_pixel(shm_client_init(&c, 4096));
	raise(SIGBUS);
}
