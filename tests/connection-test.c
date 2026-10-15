/*
 * Copyright © 2012 Intel Corporation
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test-ring.h"
#include "test-runner.h"
#include "wayland-private.h"

struct endpoint {
	struct poor_wl_connection connection;
	struct poor_wl_map map;
	char in[POOR_WL_MAX_MESSAGE_SIZE];
	char out[POOR_WL_MAX_MESSAGE_SIZE];
	union poor_wl_map_entry objects[8];
	union poor_wl_map_entry server_objects[4];
};

static const struct poor_wl_interface test_interface;

static const struct poor_wl_interface *test_types[] = {
	nullptr, nullptr, nullptr, nullptr, &test_interface, &test_interface, nullptr, nullptr,
};

static const struct poor_wl_interface *many_types[POOR_WL_CLOSURE_MAX_ARGS + 1];

static const struct poor_wl_message test_messages[] = {
	{ "all", "iufs?onah", test_types },
	{ "string", "s", test_types },
	{ "nullable", "?s?o", test_types + 3 },
	{ "fds", "hhh", test_types },
	{ "new_id", "n", test_types + 5 },
	{ "object", "o", test_types + 4 },
	{ "array", "a", test_types },
	{ "many", "uuuuuuuuuuuuuuuuuuuuu", many_types },
	{ "string_and_uint", "su", test_types },
};

static const struct poor_wl_interface test_interface = {
	"test", 1, ARRAY_SIZE(test_messages), test_messages, 0, nullptr,
};

static struct poor_loop loop;

static void loop_init(void)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(&loop, 32, &params));
}

static void loop_fini(void)
{
	assert(!poor_loop_drain(&loop, poor_loop_now() + 1'000'000'000));
	poor_loop_exit(&loop);
}

static void endpoint_init(struct endpoint *e, uint32_t side)
{
	struct poor_wl_connection_memory memory = POOR_WL_CONNECTION_MEMORY(e->in, e->out, e->objects, e->server_objects);

	poor_wl_connection_init(&e->connection, &memory);
	poor_wl_map_init(&e->map, side, &memory);
	assert(!poor_wl_map_insert_at(&e->map, 0, 0, nullptr));
}

/* What the kernel would do: the bytes and the fds of 'from' arrive at 'to' */
static void transfer(struct endpoint *from, struct endpoint *to)
{
	struct poor_wl_ring_buffer *out = &from->connection.out, *in = &to->connection.in;
	struct poor_wl_fd_ring *fds_out = &from->connection.fds_out, *fds_in = &to->connection.fds_in;

	for (; out->tail != out->head; out->tail++, in->head++)
		in->data[in->head & (in->size - 1)] = out->data[out->tail & (out->size - 1)];
	for (; fds_out->tail != fds_out->head; fds_out->tail++, fds_in->head++)
		fds_in->fds[fds_in->head % POOR_WL_CONNECTION_FDS] = fds_out->fds[fds_out->tail % POOR_WL_CONNECTION_FDS];
}

static int send_message(struct endpoint *e, struct poor_wl_object *sender, uint32_t opcode, union poor_wl_argument *args)
{
	struct poor_wl_closure closure;

	if (poor_wl_closure_marshal(&closure, sender, opcode, args, &test_messages[opcode]) < 0)
		return -1;
	return poor_wl_closure_send(&closure, &e->connection, &loop);
}

static int receive_message(struct endpoint *e, struct poor_wl_closure *closure, struct poor_wl_closure_data *data)
{
	uint32_t header[2];

	assert(poor_wl_connection_pending_input(&e->connection) >= sizeof(header));
	poor_wl_connection_copy(&e->connection, header, sizeof(header));
	assert((header[1] >> 16) <= poor_wl_connection_pending_input(&e->connection));
	return poor_wl_connection_demarshal(&e->connection, &loop, header[1] >> 16, &e->map, &test_messages[header[1] & 0xffff],
				       closure, data);
}

TEST(marshal_demarshal_all_types)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 }, new_object = { &test_interface, nullptr, 4 };
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	char bytes[] = "some bytes";
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(bytes);
	union poor_wl_argument args[8];
	struct stat a, b;

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	assert(!poor_wl_map_insert_at(&server.map, 0, 1, nullptr));
	assert(!poor_wl_map_insert_at(&server.map, 0, 2, nullptr));
	assert(!poor_wl_map_insert_at(&server.map, 0, 3, &sender));
	assert(poor_wl_array_add(&array, sizeof(bytes)));
	args[0].i = -1234;
	args[1].u = 0xdeadbeef;
	args[2].f = poor_wl_fixed_from_double(-2.5);
	args[3].s = "a string";
	args[4].o = &sender;
	args[5].o = &new_object;
	args[6].a = &array;
	args[7].h = STDERR_FILENO;
	assert(!send_message(&client, &sender, 0, args));
	transfer(&client, &server);
	assert(!receive_message(&server, &closure, &data));
	assert(closure.sender_id == 3 && closure.opcode == 0 && closure.count == 8);
	assert(closure.args[0].i == -1234 && closure.args[1].u == 0xdeadbeef);
	assert(closure.args[2].f == poor_wl_fixed_from_double(-2.5));
	assert(!strcmp(closure.args[3].s, "a string"));
	assert(closure.args[4].n == 3 && closure.args[5].n == 4);
	assert(closure.args[6].a->size == sizeof(bytes) && !memcmp(closure.args[6].a->data, bytes, sizeof(bytes)));
	assert(closure.args[7].h != STDERR_FILENO && fcntl(closure.args[7].h, F_GETFD) & FD_CLOEXEC);
	assert(!fstat(closure.args[7].h, &a) && !fstat(STDERR_FILENO, &b) && a.st_ino == b.st_ino);
	assert(!poor_wl_closure_lookup_objects(&closure, &server.map));
	assert(closure.args[4].o == &sender);
	assert(!poor_wl_connection_pending_input(&server.connection));
	poor_wl_closure_close_fds(&closure, &loop);
	assert(closure.args[7].h == -1);
	loop_fini();
}

TEST(marshal_nullable)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	union poor_wl_argument args[2];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	args[0].s = nullptr;
	args[1].o = nullptr;
	assert(!send_message(&client, &sender, 2, args));
	assert(send_message(&client, &sender, 1, args) == -1 && errno == EINVAL);
	transfer(&client, &server);
	assert(!receive_message(&server, &closure, &data));
	assert(!closure.args[0].s && !closure.args[1].n);
	loop_fini();
}

static void demarshal_fails(const uint32_t *words, size_t count, int fds)
{
	struct endpoint server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	uint32_t header[2];

	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	for (int i = 0; i < fds; i++)
		server.connection.fds_in.fds[server.connection.fds_in.head++] = dup(STDERR_FILENO);
	memcpy(server.in, words, count * sizeof(*words));
	server.connection.in.head = count * sizeof(*words);
	poor_wl_connection_copy(&server.connection, header, sizeof(header));
	assert(poor_wl_connection_demarshal(&server.connection, &loop, header[1] >> 16, &server.map, &test_messages[header[1] & 0xffff],
				       &closure, &data) == -1);
	assert(errno == EINVAL);
	poor_wl_connection_close_fds(&server.connection, &loop);
}

TEST(demarshal_invalid_messages)
{
	static const uint32_t short_header[] = { 3, 4 << 16 | 1 };
	static const uint32_t short_string[] = { 3, 16 << 16 | 1, 100, 0 };
	static const uint32_t not_terminated[] = { 3, 16 << 16 | 1, 4, 0x61616161 };
	static const uint32_t embedded_nul[] = { 3, 16 << 16 | 1, 4, 0x00616161 & 0xff00ffff };
	static const uint32_t null_string[] = { 3, 12 << 16 | 1, 0 };
	static const uint32_t null_new_id[] = { 3, 12 << 16 | 4, 0 };
	static const uint32_t own_new_id[] = { 3, 12 << 16 | 4, POOR_WL_SERVER_ID_START };
	static const uint32_t missing_fd[] = { 3, 8 << 16 | 3 };
	static const uint32_t missing_object[] = { 3, 8 << 16 | 5 };
	static const uint32_t null_object[] = { 3, 12 << 16 | 5, 0 };
	static const uint32_t short_array[] = { 3, 12 << 16 | 6, 100 };
	static const uint32_t too_many_args[] = { 3, 8 << 16 | 7 };

	loop_init();
	demarshal_fails(short_header, ARRAY_SIZE(short_header), 0);
	demarshal_fails(short_string, ARRAY_SIZE(short_string), 0);
	demarshal_fails(not_terminated, ARRAY_SIZE(not_terminated), 0);
	demarshal_fails(embedded_nul, ARRAY_SIZE(embedded_nul), 0);
	demarshal_fails(null_string, ARRAY_SIZE(null_string), 0);
	demarshal_fails(null_new_id, ARRAY_SIZE(null_new_id), 0);
	demarshal_fails(own_new_id, ARRAY_SIZE(own_new_id), 0);
	demarshal_fails(missing_fd, ARRAY_SIZE(missing_fd), 2);
	demarshal_fails(missing_object, ARRAY_SIZE(missing_object), 0);
	demarshal_fails(null_object, ARRAY_SIZE(null_object), 0);
	demarshal_fails(short_array, ARRAY_SIZE(short_array), 0);
	demarshal_fails(too_many_args, ARRAY_SIZE(too_many_args), 0);
	loop_fini();
}

TEST(marshal_null_arguments)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	union poor_wl_argument args[POOR_WL_CLOSURE_MAX_ARGS + 1];
	struct poor_wl_closure closure;

	args[0].o = nullptr;
	assert(poor_wl_closure_marshal(&closure, &sender, 5, args, &test_messages[5]) == -1 && errno == EINVAL);
	assert(poor_wl_closure_marshal(&closure, &sender, 4, args, &test_messages[4]) == -1 && errno == EINVAL);
	args[0].a = nullptr;
	assert(poor_wl_closure_marshal(&closure, &sender, 6, args, &test_messages[6]) == -1 && errno == EINVAL);
	for (unsigned i = 0; i < ARRAY_SIZE(args); i++)
		args[i].u = i;
	assert(poor_wl_closure_marshal(&closure, &sender, 7, args, &test_messages[7]) == -1 && errno == EINVAL);
}

TEST(lookup_unknown_object)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 }, other = { &test_interface, nullptr, 7 };
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	union poor_wl_argument args[2];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	args[0].s = nullptr;
	args[1].o = &other;
	assert(!send_message(&client, &sender, 2, args));
	transfer(&client, &server);
	assert(!receive_message(&server, &closure, &data));
	assert(poor_wl_closure_lookup_objects(&closure, &server.map) == -1 && errno == EINVAL);
	loop_fini();
}

TEST(lookup_zombie_object)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 }, zombie = { &test_interface, nullptr, 1 };
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	union poor_wl_argument args[2];

	loop_init();
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	assert(!poor_wl_map_insert_at(&client.map, POOR_WL_MAP_ENTRY_ZOMBIE, 1, (void *)&test_interface));
	args[0].s = nullptr;
	args[1].o = &zombie;
	assert(!send_message(&server, &sender, 2, args));
	transfer(&server, &client);
	assert(!receive_message(&client, &closure, &data));
	assert(!poor_wl_closure_lookup_objects(&closure, &client.map));
	assert(!closure.args[1].o);
	loop_fini();
}

TEST(message_larger_than_maximum)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	char string[POOR_WL_MAX_MESSAGE_SIZE];
	struct endpoint client;
	union poor_wl_argument args[1];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	memset(string, 'a', sizeof(string) - 1);
	string[sizeof(string) - 1] = '\0';
	args[0].s = string;
	assert(send_message(&client, &sender, 1, args) == -1 && errno == E2BIG);
	string[POOR_WL_MAX_MESSAGE_SIZE - 13] = '\0';
	assert(!send_message(&client, &sender, 1, args));
	assert(client.connection.out.head == POOR_WL_MAX_MESSAGE_SIZE);
	loop_fini();
}

/* The string fills the message, the uint after it doesn't fit */
TEST(message_full_before_last_argument)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	char string[POOR_WL_MAX_MESSAGE_SIZE - 12];
	struct endpoint client;
	union poor_wl_argument args[2];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	memset(string, 'a', sizeof(string) - 1);
	string[sizeof(string) - 1] = '\0';
	args[0].s = string;
	args[1].u = 1;
	assert(send_message(&client, &sender, 8, args) == -1 && errno == E2BIG);
	assert(!poor_wl_connection_has_output(&client.connection));
	loop_fini();
}

/* The copies of the fds before the one that can't be copied are closed */
TEST(send_invalid_fd)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	struct endpoint client;
	union poor_wl_argument args[3];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	args[0].h = args[1].h = STDERR_FILENO;
	args[2].h = -1;
	assert(send_message(&client, &sender, 3, args) == -1 && errno == EBADF);
	assert(client.connection.fds_out.head == client.connection.fds_out.tail);
	assert(!poor_wl_connection_has_output(&client.connection));
	loop_fini();
}

TEST(out_buffer_full)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	char string[1000];
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	union poor_wl_argument args[3];
	int sent = 0;

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	memset(string, 'b', sizeof(string) - 1);
	string[sizeof(string) - 1] = '\0';
	args[0].s = string;
	while (!send_message(&client, &sender, 1, args))
		sent++;
	assert(errno == E2BIG && sent == 4);
	args[0].h = args[1].h = args[2].h = STDERR_FILENO;
	client.connection.fds_out.head += POOR_WL_CONNECTION_FDS - 2;
	client.connection.out.tail = client.connection.out.head;
	assert(send_message(&client, &sender, 3, args) == -1 && errno == E2BIG);
	client.connection.fds_out.tail = client.connection.fds_out.head;
	assert(!send_message(&client, &sender, 3, args));
	transfer(&client, &server);
	assert(!receive_message(&server, &closure, &data));
	poor_wl_closure_close_fds(&closure, &loop);
	loop_fini();
}

/* Messages wrap around the end of the rings */
TEST(ring_wrap_around)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	char string[333];
	union poor_wl_argument args[1];

	loop_init();
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	for (int i = 0; i < 100; i++) {
		memset(string, 'a' + i % 26, sizeof(string) - 1);
		string[sizeof(string) - 1 - i % 4] = '\0';
		args[0].s = string;
		assert(!send_message(&client, &sender, 1, args));
		transfer(&client, &server);
		assert(!receive_message(&server, &closure, &data));
		assert(!strcmp(closure.args[0].s, string));
	}
	assert(server.connection.in.tail > sizeof(server.in) * 4);
	loop_fini();
}

struct io {
	struct poor_loop_op op;
	int res;
	bool done;
};

static void io_done(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct io *io = container_of(op, struct io, op);

	io->res = cqe->res;
	io->done = true;
}

/* More fds than one send takes go out in two sends, the first of them with a single byte */
TEST(send_and_receive_fds)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 };
	struct io send, recv;
	struct endpoint client, server;
	struct poor_wl_closure_data data;
	struct poor_wl_closure closure;
	union poor_wl_argument args[3];
	int sockets[2], received = 0, sends = 0;

	loop_init();
	poor_loop_op_init(&send.op, io_done);
	poor_loop_op_init(&recv.op, io_done);
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
	endpoint_init(&client, POOR_WL_MAP_CLIENT_SIDE);
	endpoint_init(&server, POOR_WL_MAP_SERVER_SIDE);
	args[0].h = args[1].h = args[2].h = STDERR_FILENO;
	for (int i = 0; i < 12; i++)
		assert(!send_message(&client, &sender, 3, args));
	while (poor_wl_connection_has_output(&client.connection)) {
		send.done = false;
		poor_wl_connection_prep_send(&client.connection, poor_loop_get_sqe(&loop, &send.op), sockets[0], 0);
		while (!send.done)
			assert(!poor_loop_run_once(&loop));
		assert(send.res > 0);
		assert(sends || send.res == 1);
		poor_wl_connection_send_done(&client.connection, &loop, send.res);
		sends++;
	}
	assert(sends == 2);
	while (received < 12) {
		assert(poor_wl_connection_can_recv(&server.connection));
		recv.done = false;
		poor_wl_connection_prep_recv(&server.connection, poor_loop_get_sqe(&loop, &recv.op), sockets[1]);
		while (!recv.done)
			assert(!poor_loop_run_once(&loop));
		assert(recv.res > 0);
		assert(!poor_wl_connection_recv_done(&server.connection, &loop, recv.res));
		while (poor_wl_connection_pending_input(&server.connection) >= 8) {
			assert(!receive_message(&server, &closure, &data));
			for (int i = 0; i < 3; i++)
				assert(closure.args[i].h > STDERR_FILENO && fcntl(closure.args[i].h, F_GETFD) & FD_CLOEXEC);
			poor_wl_closure_close_fds(&closure, &loop);
			received++;
		}
	}
	poor_loop_close_fd_or_submit(&loop, sockets[0], 0);
	poor_loop_close_fd_or_submit(&loop, sockets[1], 0);
	loop_fini();
}

static char log_buffer[8192];

static void log_to_buffer(const char *fmt, va_list ap)
{
	vsnprintf(log_buffer, sizeof(log_buffer), fmt, ap);
}

TEST(closure_print)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 }, new_object = { &test_interface, nullptr, 4 };
	struct poor_wl_closure closure;
	char bytes[4];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(bytes);
	union poor_wl_argument args[8];

	assert(poor_wl_array_add(&array, 3));
	args[0].i = -5;
	args[1].u = 7;
	args[2].f = poor_wl_fixed_from_double(1.25);
	args[3].s = "str";
	args[4].o = nullptr;
	args[5].o = &new_object;
	args[6].a = &array;
	args[7].h = 2;
	assert(!poor_wl_closure_marshal(&closure, &sender, 0, args, &test_messages[0]));
	poor_wl_log_handler = log_to_buffer;
	poor_wl_closure_print(&closure, &sender, true, false, nullptr, false);
	poor_wl_log_handler = nullptr;
	assert(strstr(log_buffer, " -> test#3.all(-5, 7, 1.25000000, \"str\", nil, new id test#4, array[3], fd 2)\n"));
}

TEST(env_tokens)
{
	assert(poor_wl_check_env_token("client", "client"));
	assert(poor_wl_check_env_token(",,server,,client", "client"));
	assert(poor_wl_check_env_token("server,client,", "server"));
	assert(!poor_wl_check_env_token("server,clients", "client"));
	assert(!poor_wl_check_env_token("cli,server", "client"));
	assert(!poor_wl_check_env_token(",,,", "client"));
	assert(!poor_wl_check_env_token(nullptr, "client"));
}

static uint32_t no_id(const union poor_wl_argument *)
{
	return 0;
}

TEST(closure_print_details)
{
	struct poor_wl_object sender = { &test_interface, nullptr, 3 }, new_object = { &test_interface, nullptr, 4 };
	struct poor_wl_closure closure;
	char bytes[1];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(bytes);
	union poor_wl_argument args[8];

	setenv("WAYLAND_DEBUG", "thread_id", 1);
	args[0].s = nullptr;
	args[1].o = &sender;
	assert(!poor_wl_closure_marshal(&closure, &sender, 2, args, &test_messages[2]));
	poor_wl_log_handler = log_to_buffer;
	poor_wl_closure_print(&closure, &sender, false, true, nullptr, false);
	assert(strstr(log_buffer, "TID#") && strstr(log_buffer, "discarded test#3.nullable(nil, test#3)\n"));
	poor_wl_closure_print(&closure, &sender, false, false, nullptr, true);
	assert(strstr(log_buffer, "\033["));
	args[0].i = 0;
	args[1].u = 0;
	args[2].f = poor_wl_fixed_from_double(-0.5);
	args[3].s = "s";
	args[4].o = nullptr;
	args[5].o = &new_object;
	args[6].a = &array;
	args[7].h = 1;
	assert(!poor_wl_closure_marshal(&closure, &sender, 0, args, &test_messages[0]));
	poor_wl_closure_print(&closure, &sender, false, false, no_id, false);
	poor_wl_log_handler = nullptr;
	assert(strstr(log_buffer, "test#3.all(0, 0, -0.50000000, \"s\", nil, new id test#nil, array[0], fd 1)\n"));
}
