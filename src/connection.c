/*
 * Copyright © 2008 Kristian Høgsberg
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

#define _GNU_SOURCE

#include "../config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wayland-private.h"

static_assert(!(POOR_WL_CONNECTION_FDS & (POOR_WL_CONNECTION_FDS - 1)), "the fd queues wrap around with their counters");

/* The cast prevents overflow when rounding values close to UINT32_MAX */
static inline uint32_t div_roundup(uint32_t n, size_t a)
{
	return (uint32_t)(((uint64_t)n + (a - 1)) / a);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t ring_used(const struct poor_wl_ring_buffer *b)
{
	return b->head - b->tail;
}

static uint32_t ring_free(const struct poor_wl_ring_buffer *b)
{
	return b->size - ring_used(b);
}

static uint32_t ring_mask(const struct poor_wl_ring_buffer *b, uint32_t i)
{
	return i & (b->size - 1);
}

static void ring_put(struct poor_wl_ring_buffer *b, const void *data, uint32_t count)
{
	uint32_t head = ring_mask(b, b->head), first = min_u32(b->size - head, count);

	memcpy(b->data + head, data, first);
	memcpy(b->data, (const char *)data + first, count - first);
	b->head += count;
}

static void ring_copy(const struct poor_wl_ring_buffer *b, void *data, uint32_t count)
{
	uint32_t tail = ring_mask(b, b->tail), first = min_u32(b->size - tail, count);

	memcpy(data, b->data + tail, first);
	memcpy((char *)data + first, b->data, count - first);
}

static int ring_iov(const struct poor_wl_ring_buffer *b, uint32_t start, uint32_t len, struct iovec *iov)
{
	uint32_t first = min_u32(b->size - ring_mask(b, start), len);

	iov[0].iov_base = b->data + ring_mask(b, start);
	iov[0].iov_len = first;
	if (first == len)
		return 1;
	iov[1].iov_base = b->data;
	iov[1].iov_len = len - first;
	return 2;
}

static uint32_t fds_used(const struct poor_wl_fd_ring *r)
{
	return r->head - r->tail;
}

static uint32_t fds_free(const struct poor_wl_fd_ring *r)
{
	return POOR_WL_CONNECTION_FDS - fds_used(r);
}

static void fds_put(struct poor_wl_fd_ring *r, int fd)
{
	r->fds[r->head++ % POOR_WL_CONNECTION_FDS] = fd;
}

static int fds_get(struct poor_wl_fd_ring *r)
{
	return r->fds[r->tail++ % POOR_WL_CONNECTION_FDS];
}

static void fds_close(struct poor_wl_fd_ring *r, struct poor_loop *loop, uint32_t count)
{
	while (count--)
		poor_loop_close_fd_or_submit(loop, fds_get(r), 0);
}

static void ring_init(struct poor_wl_ring_buffer *b, char *data, uint32_t size)
{
	assert(size >= POOR_WL_MAX_MESSAGE_SIZE && size <= UINT32_MAX / 2 + 1 && !(size & (size - 1)));
	b->data = data;
	b->size = size;
	b->head = b->tail = 0;
}

void poor_wl_connection_init(struct poor_wl_connection *connection, const struct poor_wl_connection_memory *memory)
{
	ring_init(&connection->in, memory->in, memory->in_size);
	ring_init(&connection->out, memory->out, memory->out_size);
	connection->fds_in.head = connection->fds_in.tail = 0;
	connection->fds_out.head = connection->fds_out.tail = 0;
	connection->out_fds = 0;
	connection->want_flush = false;
}

/* A recvmsg() gets the fds of one sendmsg() at most, so the queue keeps room for as many as one can send */
bool poor_wl_connection_can_recv(const struct poor_wl_connection *connection)
{
	return ring_free(&connection->in) && fds_free(&connection->fds_in) >= POOR_WL_CONNECTION_MAX_FDS;
}

void poor_wl_connection_prep_recv(struct poor_wl_connection *connection, struct io_uring_sqe *sqe, int fd)
{
	struct msghdr *msg = &connection->in_msg;

	msg->msg_name = nullptr;
	msg->msg_namelen = 0;
	msg->msg_iov = connection->in_iov;
	msg->msg_iovlen = ring_iov(&connection->in, connection->in.head, ring_free(&connection->in), connection->in_iov);
	msg->msg_control = connection->in_cmsg;
	msg->msg_controllen = sizeof(connection->in_cmsg);
	msg->msg_flags = 0;
	io_uring_prep_recvmsg(sqe, fd, msg, MSG_CMSG_CLOEXEC);
}

/* A truncated control message lost fds, so the connection can't go on */
int poor_wl_connection_recv_done(struct poor_wl_connection *connection, struct poor_loop *loop, int res)
{
	struct msghdr *msg = &connection->in_msg;
	bool overflow = msg->msg_flags & MSG_CTRUNC;

	connection->in.head += res;
	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		int fd;

		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		for (size_t i = 0; i < n; i++) {
			memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(fd), sizeof(fd));
			if (overflow || !fds_free(&connection->fds_in)) {
				overflow = true;
				poor_loop_close_fd_or_submit(loop, fd, 0);
			} else {
				fds_put(&connection->fds_in, fd);
			}
		}
	}
	if (overflow) {
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}

bool poor_wl_connection_has_output(const struct poor_wl_connection *connection)
{
	return ring_used(&connection->out);
}

/*
 * A send takes up to POOR_WL_CONNECTION_MAX_FDS fds. When that many are waiting, they go with a single byte, so the fds
 * of messages that need more of them are all sent before the messages are.
 */
void poor_wl_connection_prep_send(struct poor_wl_connection *connection, struct io_uring_sqe *sqe, int fd, int flags)
{
	struct msghdr *msg = &connection->out_msg;
	uint32_t nfds = min_u32(fds_used(&connection->fds_out), POOR_WL_CONNECTION_MAX_FDS);
	struct cmsghdr *cmsg = (struct cmsghdr *)connection->out_cmsg;
	uint32_t len = nfds == POOR_WL_CONNECTION_MAX_FDS ? 1 : ring_used(&connection->out);

	msg->msg_name = nullptr;
	msg->msg_namelen = 0;
	msg->msg_iov = connection->out_iov;
	msg->msg_iovlen = ring_iov(&connection->out, connection->out.tail, len, connection->out_iov);
	msg->msg_control = nullptr;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
	if (nfds) {
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(nfds * sizeof(int));
		for (uint32_t i = 0; i < nfds; i++) {
			int fd_out = connection->fds_out.fds[(connection->fds_out.tail + i) % POOR_WL_CONNECTION_FDS];

			memcpy(CMSG_DATA(cmsg) + i * sizeof(int), &fd_out, sizeof(int));
		}
		msg->msg_control = cmsg;
		msg->msg_controllen = CMSG_SPACE(nfds * sizeof(int));
	}
	connection->out_fds = nfds;
	io_uring_prep_sendmsg(sqe, fd, msg, MSG_NOSIGNAL | flags);
}

/* The fds went with the first byte, and the peer holds them now */
void poor_wl_connection_send_done(struct poor_wl_connection *connection, struct poor_loop *loop, int res)
{
	if (res > 0) {
		fds_close(&connection->fds_out, loop, connection->out_fds);
		connection->out.tail += res;
	}
	connection->out_fds = 0;
}

void poor_wl_connection_close_fds(struct poor_wl_connection *connection, struct poor_loop *loop)
{
	fds_close(&connection->fds_in, loop, fds_used(&connection->fds_in));
	fds_close(&connection->fds_out, loop, fds_used(&connection->fds_out));
}

void poor_wl_connection_close_fds_in(struct poor_wl_connection *connection, struct poor_loop *loop, int max)
{
	fds_close(&connection->fds_in, loop, min_u32(fds_used(&connection->fds_in), max));
}

uint32_t poor_wl_connection_pending_input(const struct poor_wl_connection *connection)
{
	return ring_used(&connection->in);
}

void poor_wl_connection_copy(const struct poor_wl_connection *connection, void *data, size_t size)
{
	ring_copy(&connection->in, data, size);
}

void poor_wl_connection_consume(struct poor_wl_connection *connection, size_t size)
{
	connection->in.tail += size;
}

const char *get_next_argument(const char *signature, struct argument_details *details)
{
	details->nullable = 0;
	for (; *signature; ++signature) {
		switch (*signature) {
		case POOR_WL_ARG_INT:
		case POOR_WL_ARG_UINT:
		case POOR_WL_ARG_FIXED:
		case POOR_WL_ARG_STRING:
		case POOR_WL_ARG_OBJECT:
		case POOR_WL_ARG_NEW_ID:
		case POOR_WL_ARG_ARRAY:
		case POOR_WL_ARG_FD:
			details->type = *signature;
			return signature + 1;
		case '?':
			details->nullable = 1;
		}
	}
	details->type = '\0';
	return signature;
}

int arg_count_for_signature(const char *signature)
{
	int count = 0;

	for (; *signature; ++signature) {
		switch (*signature) {
		case POOR_WL_ARG_INT:
		case POOR_WL_ARG_UINT:
		case POOR_WL_ARG_FIXED:
		case POOR_WL_ARG_STRING:
		case POOR_WL_ARG_OBJECT:
		case POOR_WL_ARG_NEW_ID:
		case POOR_WL_ARG_ARRAY:
		case POOR_WL_ARG_FD:
			++count;
		}
	}
	return count;
}

int poor_wl_message_count_fds(const struct poor_wl_message *message)
{
	int fds = 0;

	for (const char *s = message->signature; *s; s++)
		fds += *s == POOR_WL_ARG_FD;
	return fds;
}

int poor_wl_message_get_since(const struct poor_wl_message *message)
{
	int since = atoi(message->signature);

	return since ? since : 1;
}

void poor_wl_argument_from_va_list(const char *signature, union poor_wl_argument *args, int count, va_list ap)
{
	struct argument_details arg;

	for (int i = 0; i < count; i++) {
		signature = get_next_argument(signature, &arg);
		switch (arg.type) {
		case POOR_WL_ARG_INT:
			args[i].i = va_arg(ap, int32_t);
			break;
		case POOR_WL_ARG_UINT:
			args[i].u = va_arg(ap, uint32_t);
			break;
		case POOR_WL_ARG_FIXED:
			args[i].f = va_arg(ap, poor_wl_fixed_t);
			break;
		case POOR_WL_ARG_STRING:
			args[i].s = va_arg(ap, const char *);
			break;
		case POOR_WL_ARG_OBJECT:
		case POOR_WL_ARG_NEW_ID:
			args[i].o = va_arg(ap, struct poor_wl_object *);
			break;
		case POOR_WL_ARG_ARRAY:
			args[i].a = va_arg(ap, struct poor_wl_array *);
			break;
		case POOR_WL_ARG_FD:
			args[i].h = va_arg(ap, int32_t);
			break;
		}
	}
}

static int closure_init(struct poor_wl_closure *closure, const struct poor_wl_message *message)
{
	int count = arg_count_for_signature(message->signature);

	if (count > POOR_WL_CLOSURE_MAX_ARGS) {
		poor_wl_log("too many args (%d) for %s (signature %s)\n", count, message->name, message->signature);
		errno = EINVAL;
		return -1;
	}
	closure->message = message;
	closure->count = count;
	return 0;
}

/* The fds stay with the caller, poor_wl_closure_send() sends copies */
int poor_wl_closure_marshal(struct poor_wl_closure *closure, struct poor_wl_object *sender, uint32_t opcode, const union poor_wl_argument *args,
		       const struct poor_wl_message *message)
{
	const char *signature = message->signature;
	struct argument_details arg;
	int i;

	if (closure_init(closure, message) < 0)
		return -1;
	for (i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		closure->args[i] = args[i];
		switch (arg.type) {
		case POOR_WL_ARG_FIXED:
		case POOR_WL_ARG_UINT:
		case POOR_WL_ARG_INT:
		case POOR_WL_ARG_FD:
			break;
		case POOR_WL_ARG_STRING:
			if (!arg.nullable && !args[i].s)
				goto err_null;
			break;
		case POOR_WL_ARG_OBJECT:
			if (!arg.nullable && !args[i].o)
				goto err_null;
			break;
		case POOR_WL_ARG_NEW_ID:
			if (!args[i].o)
				goto err_null;
			closure->args[i].n = args[i].o->id;
			break;
		case POOR_WL_ARG_ARRAY:
			if (!args[i].a)
				goto err_null;
			break;
		default:
			poor_wl_abort("unhandled format code: '%c'\n", arg.type);
		}
	}
	closure->sender_id = sender->id;
	closure->opcode = opcode;
	return 0;

err_null:
	poor_wl_log("error marshalling arguments for %s (signature %s): null value passed for arg %i\n", message->name,
	       message->signature, i);
	errno = EINVAL;
	return -1;
}

/* Returns the size of the message, or -1 if it doesn't fit 'buffer' */
static int serialize_closure(const struct poor_wl_closure *closure, uint32_t *buffer, size_t buffer_count, uint32_t *nfds)
{
	const struct poor_wl_message *message = closure->message;
	const char *signature = message->signature;
	struct argument_details arg;
	uint32_t *p = buffer + 2, *end = buffer + buffer_count, size;

	*nfds = 0;
	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type == POOR_WL_ARG_FD) {
			(*nfds)++;
			continue;
		}
		if (p >= end)
			goto overflow;
		switch (arg.type) {
		case POOR_WL_ARG_UINT:
			*p++ = closure->args[i].u;
			break;
		case POOR_WL_ARG_INT:
			*p++ = closure->args[i].i;
			break;
		case POOR_WL_ARG_FIXED:
			*p++ = closure->args[i].f;
			break;
		case POOR_WL_ARG_OBJECT:
			*p++ = closure->args[i].o ? closure->args[i].o->id : 0;
			break;
		case POOR_WL_ARG_NEW_ID:
			*p++ = closure->args[i].n;
			break;
		case POOR_WL_ARG_STRING:
			if (!closure->args[i].s) {
				*p++ = 0;
				break;
			}
			size = strlen(closure->args[i].s) + 1;
			*p++ = size;
			if (div_roundup(size, sizeof(*p)) > (uint32_t)(end - p))
				goto overflow;
			p[div_roundup(size, sizeof(*p)) - 1] = 0;
			memcpy(p, closure->args[i].s, size);
			p += div_roundup(size, sizeof(*p));
			break;
		case POOR_WL_ARG_ARRAY:
			size = closure->args[i].a->size;
			*p++ = size;
			if (div_roundup(size, sizeof(*p)) > (uint32_t)(end - p))
				goto overflow;
			if (size) {
				p[div_roundup(size, sizeof(*p)) - 1] = 0;
				memcpy(p, closure->args[i].a->data, size);
			}
			p += div_roundup(size, sizeof(*p));
			break;
		case POOR_WL_ARG_FD:
			break;
		}
	}
	size = (p - buffer) * sizeof(*p);
	buffer[0] = closure->sender_id;
	buffer[1] = size << 16 | (closure->opcode & 0x0000ffff);
	return size;

overflow:
	poor_wl_log("message %s (signature %s) exceeds %d bytes\n", message->name, message->signature, POOR_WL_MAX_MESSAGE_SIZE);
	errno = E2BIG;
	return -1;
}

/* The connection keeps duplicates of the fds until they are sent */
int poor_wl_closure_send(const struct poor_wl_closure *closure, struct poor_wl_connection *connection, struct poor_loop *loop)
{
	uint32_t buffer[POOR_WL_MAX_MESSAGE_SIZE / sizeof(uint32_t)], nfds, head = connection->fds_out.head;
	const char *signature = closure->message->signature;
	struct argument_details arg;
	int size = serialize_closure(closure, buffer, ARRAY_SIZE(buffer), &nfds);

	if (size < 0)
		return -1;
	if ((uint32_t)size > ring_free(&connection->out) || nfds > fds_free(&connection->fds_out)) {
		poor_wl_log("no room for %s: %d bytes and %u fds queued\n", closure->message->name, ring_used(&connection->out),
		       fds_used(&connection->fds_out));
		errno = E2BIG;
		return -1;
	}
	for (int i = 0; i < closure->count; i++) {
		int fd;

		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_FD)
			continue;
		fd = fcntl(closure->args[i].h, F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			poor_wl_log("error marshalling arguments for %s: dup failed: %s\n", closure->message->name, strerror(errno));
			while (connection->fds_out.head != head)
				poor_loop_close_fd_or_submit(loop, connection->fds_out.fds[--connection->fds_out.head % POOR_WL_CONNECTION_FDS], 0);
			return -1;
		}
		fds_put(&connection->fds_out, fd);
	}
	ring_put(&connection->out, buffer, size);
	return 0;
}

int poor_wl_connection_demarshal(struct poor_wl_connection *connection, struct poor_loop *loop, uint32_t size, struct poor_wl_map *objects,
			    const struct poor_wl_message *message, struct poor_wl_closure *closure, struct poor_wl_closure_data *data)
{
	uint32_t *p = data->words, *next, *end, length, length_in_u32, id;
	struct poor_wl_array *array_extra = data->arrays;
	const char *signature = message->signature;
	struct argument_details arg;
	char *s;

	assert(size <= sizeof(data->words));
	if (size < 2 * sizeof(*p)) {
		poor_wl_log("message too short, invalid header\n");
		poor_wl_connection_consume(connection, size);
		errno = EINVAL;
		return -1;
	}
	if (closure_init(closure, message) < 0) {
		poor_wl_connection_consume(connection, size);
		return -1;
	}
	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type == POOR_WL_ARG_FD)
			closure->args[i].h = -1;
	}

	end = p + size / sizeof(*p);
	poor_wl_connection_copy(connection, p, size);
	closure->sender_id = *p++;
	closure->opcode = *p++ & 0x0000ffff;

	signature = message->signature;
	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_FD && p >= end) {
			poor_wl_log("message too short, object (%d), message %s(%s)\n", closure->sender_id, message->name,
			       message->signature);
			errno = EINVAL;
			goto err;
		}
		switch (arg.type) {
		case POOR_WL_ARG_UINT:
			closure->args[i].u = *p++;
			break;
		case POOR_WL_ARG_INT:
			closure->args[i].i = *p++;
			break;
		case POOR_WL_ARG_FIXED:
			closure->args[i].f = *p++;
			break;
		case POOR_WL_ARG_STRING:
			length = *p++;
			if (length == 0 && !arg.nullable) {
				poor_wl_log("NULL string received on non-nullable type, message %s(%s)\n", message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			if (length == 0) {
				closure->args[i].s = nullptr;
				break;
			}
			length_in_u32 = div_roundup(length, sizeof(*p));
			if ((uint32_t)(end - p) < length_in_u32) {
				poor_wl_log("message too short, object (%d), message %s(%s)\n", closure->sender_id, message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			next = p + length_in_u32;
			s = (char *)p;
			if (s[length - 1] != '\0') {
				poor_wl_log("string not nul-terminated, message %s(%s)\n", message->name, message->signature);
				errno = EINVAL;
				goto err;
			}
			if (strlen(s) != length - 1) {
				poor_wl_log("string has embedded nul at offset %zu, message %s(%s)\n", strlen(s), message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			closure->args[i].s = s;
			p = next;
			break;
		case POOR_WL_ARG_OBJECT:
			id = *p++;
			closure->args[i].n = id;
			if (id == 0 && !arg.nullable) {
				poor_wl_log("NULL object received on non-nullable type, message %s(%s)\n", message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			break;
		case POOR_WL_ARG_NEW_ID:
			id = *p++;
			closure->args[i].n = id;
			if (id == 0) {
				poor_wl_log("NULL new ID received on non-nullable type, message %s(%s)\n", message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			if (poor_wl_map_reserve_new(objects, id) < 0) {
				poor_wl_log("not a valid new object id (%u), message %s(%s): %s\n", id, message->name, message->signature,
				       strerror(errno));
				goto err;
			}
			break;
		case POOR_WL_ARG_ARRAY:
			length = *p++;
			length_in_u32 = div_roundup(length, sizeof(*p));
			if ((uint32_t)(end - p) < length_in_u32) {
				poor_wl_log("message too short, object (%d), message %s(%s)\n", closure->sender_id, message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			next = p + length_in_u32;
			array_extra->size = length;
			array_extra->alloc = 0;
			array_extra->data = p;
			closure->args[i].a = array_extra++;
			p = next;
			break;
		case POOR_WL_ARG_FD:
			if (!fds_used(&connection->fds_in)) {
				poor_wl_log("file descriptor expected, object (%d), message %s(%s)\n", closure->sender_id, message->name,
				       message->signature);
				errno = EINVAL;
				goto err;
			}
			closure->args[i].h = fds_get(&connection->fds_in);
			break;
		default:
			poor_wl_abort("unknown type\n");
		}
	}
	poor_wl_connection_consume(connection, size);
	return 0;

err:
	poor_wl_closure_close_fds(closure, loop);
	poor_wl_connection_consume(connection, size);
	return -1;
}

bool poor_wl_object_is_zombie(struct poor_wl_map *map, uint32_t id)
{
	return map->side == POOR_WL_MAP_CLIENT_SIDE && (poor_wl_map_lookup_flags(map, id) & POOR_WL_MAP_ENTRY_ZOMBIE);
}

int poor_wl_closure_lookup_objects(struct poor_wl_closure *closure, struct poor_wl_map *objects)
{
	const struct poor_wl_message *message = closure->message;
	const char *signature = message->signature;
	struct argument_details arg;
	struct poor_wl_object *object;
	uint32_t id;

	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type != POOR_WL_ARG_OBJECT)
			continue;
		id = closure->args[i].n;
		closure->args[i].o = nullptr;
		object = poor_wl_map_lookup(objects, id);
		if (poor_wl_object_is_zombie(objects, id)) {
			object = nullptr;
		} else if (!object && id) {
			poor_wl_log("unknown object (%u), message %s(%s)\n", id, message->name, message->signature);
			errno = EINVAL;
			return -1;
		}
		if (object && message->types[i] && !poor_wl_interface_equal(object->interface, message->types[i])) {
			poor_wl_log("invalid object (%u), type (%s), message %s(%s)\n", id, object->interface->name, message->name,
			       message->signature);
			errno = EINVAL;
			return -1;
		}
		closure->args[i].o = object;
	}
	return 0;
}

static void convert_arguments_to_ffi(const char *signature, uint32_t flags, union poor_wl_argument *args, int count,
				     ffi_type **ffi_types, void **ffi_args)
{
	struct argument_details arg;

	for (int i = 0; i < count; i++) {
		signature = get_next_argument(signature, &arg);
		switch (arg.type) {
		case POOR_WL_ARG_INT:
			ffi_types[i] = &ffi_type_sint32;
			ffi_args[i] = &args[i].i;
			break;
		case POOR_WL_ARG_UINT:
			ffi_types[i] = &ffi_type_uint32;
			ffi_args[i] = &args[i].u;
			break;
		case POOR_WL_ARG_FIXED:
			ffi_types[i] = &ffi_type_sint32;
			ffi_args[i] = &args[i].f;
			break;
		case POOR_WL_ARG_STRING:
			ffi_types[i] = &ffi_type_pointer;
			ffi_args[i] = &args[i].s;
			break;
		case POOR_WL_ARG_OBJECT:
			ffi_types[i] = &ffi_type_pointer;
			ffi_args[i] = &args[i].o;
			break;
		case POOR_WL_ARG_NEW_ID:
			if (flags & POOR_WL_CLOSURE_INVOKE_CLIENT) {
				ffi_types[i] = &ffi_type_pointer;
				ffi_args[i] = &args[i].o;
			} else {
				ffi_types[i] = &ffi_type_uint32;
				ffi_args[i] = &args[i].n;
			}
			break;
		case POOR_WL_ARG_ARRAY:
			ffi_types[i] = &ffi_type_pointer;
			ffi_args[i] = &args[i].a;
			break;
		case POOR_WL_ARG_FD:
			ffi_types[i] = &ffi_type_sint32;
			ffi_args[i] = &args[i].h;
			break;
		default:
			poor_wl_abort("unknown type\n");
		}
	}
}

/* The implementation owns the fds now */
static void closure_clear_fds(struct poor_wl_closure *closure)
{
	const char *signature = closure->message->signature;
	struct argument_details arg;

	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type == POOR_WL_ARG_FD)
			closure->args[i].h = -1;
	}
}

void poor_wl_closure_invoke(struct poor_wl_closure *closure, uint32_t flags, struct poor_wl_object *target, uint32_t opcode, void *data)
{
	ffi_type *ffi_types[POOR_WL_CLOSURE_MAX_ARGS + 2];
	void *ffi_args[POOR_WL_CLOSURE_MAX_ARGS + 2];
	void (*const *implementation)(void) = target->implementation;
	ffi_cif cif;

	ffi_types[0] = &ffi_type_pointer;
	ffi_args[0] = &data;
	ffi_types[1] = &ffi_type_pointer;
	ffi_args[1] = &target;
	convert_arguments_to_ffi(closure->message->signature, flags, closure->args, closure->count, ffi_types + 2, ffi_args + 2);
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, closure->count + 2, &ffi_type_void, ffi_types);
	if (!implementation)
		poor_wl_abort("Implementation of resource %d of %s is NULL\n", target->id, target->interface->name);
	if (!implementation[opcode])
		poor_wl_abort("listener function for opcode %u of %s is NULL\n", opcode, target->interface->name);
	ffi_call(&cif, implementation[opcode], nullptr, ffi_args);
	closure_clear_fds(closure);
}

void poor_wl_closure_dispatch(struct poor_wl_closure *closure, poor_wl_dispatcher_func_t dispatcher, struct poor_wl_object *target, uint32_t opcode)
{
	dispatcher(target->implementation, target, opcode, closure->message, closure->args);
	closure_clear_fds(closure);
}

void poor_wl_closure_close_fds(struct poor_wl_closure *closure, struct poor_loop *loop)
{
	const char *signature = closure->message->signature;
	struct argument_details arg;

	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (arg.type == POOR_WL_ARG_FD && closure->args[i].h >= 0) {
			poor_loop_close_fd_or_submit(loop, closure->args[i].h, 0);
			closure->args[i].h = -1;
		}
	}
}

bool poor_wl_check_env_token(const char *env, const char *token)
{
	const char *ptr = env;
	size_t token_len;

	if (!env)
		return false;
	token_len = strlen(token);
	for (;;) {
		const char *end;
		size_t len;

		while (*ptr == ',')
			ptr++;
		if (*ptr == '\x00')
			return false;
		end = strchr(ptr + 1, ',');
		if (!end)
			return strcmp(ptr, token) == 0;
		len = end - ptr;
		if (len == token_len && memcmp(ptr, token, len) == 0)
			return true;
		ptr += len;
	}
}

struct print_buffer {
	size_t len;
	char data[8192];
};

[[gnu::format(printf, 2, 3)]] static void buffer_print(struct print_buffer *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->len >= sizeof(b->data))
		return;
	va_start(ap, fmt);
	n = vsnprintf(b->data + b->len, sizeof(b->data) - b->len, fmt, ap);
	va_end(ap);
	if (n > 0)
		b->len += n;
}

void poor_wl_closure_print(const struct poor_wl_closure *closure, const struct poor_wl_object *target, bool send, bool discarded,
		      uint32_t (*n_parse)(const union poor_wl_argument *arg), bool color)
{
#if defined(HAVE_GETTID)
	static int include_tid = -1;
#endif
	const char *signature = closure->message->signature;
	struct argument_details arg;
	struct print_buffer b;
	struct timespec tp;
	uint32_t nval;

	b.len = 0;
	clock_gettime(CLOCK_REALTIME, &tp);
	buffer_print(&b, "%s[%02u:%02u:%02u.%06u] ", color ? POOR_WL_DEBUG_COLOR_GREEN : "", (unsigned)((tp.tv_sec / 3600) % 24),
	      (unsigned)((tp.tv_sec / 60) % 60), (unsigned)(tp.tv_sec % 60), (unsigned)(tp.tv_nsec / 1000));
#if defined(HAVE_GETTID)
	if (include_tid < 0)
		include_tid = poor_wl_check_env_token(getenv("WAYLAND_DEBUG"), "thread_id");
	if (include_tid)
		buffer_print(&b, "%sTID#%d ", color ? POOR_WL_DEBUG_COLOR_CYAN : "", (int)gettid());
#endif
	buffer_print(&b, "%s%s%s%s%s%s%s#%u%s.%s%s(", color ? POOR_WL_DEBUG_COLOR_RED : "", discarded ? "discarded " : "",
	      color ? POOR_WL_DEBUG_COLOR_RESET : "", send ? " -> " : "", color ? POOR_WL_DEBUG_COLOR_BLUE : "", target->interface->name,
	      color ? POOR_WL_DEBUG_COLOR_MAGENTA : "", target->id, color ? POOR_WL_DEBUG_COLOR_CYAN : "", closure->message->name,
	      color ? POOR_WL_DEBUG_COLOR_RESET : "");
	for (int i = 0; i < closure->count; i++) {
		signature = get_next_argument(signature, &arg);
		if (i > 0)
			buffer_print(&b, ", ");
		switch (arg.type) {
		case POOR_WL_ARG_UINT:
			buffer_print(&b, "%u", closure->args[i].u);
			break;
		case POOR_WL_ARG_INT:
			buffer_print(&b, "%d", closure->args[i].i);
			break;
		case POOR_WL_ARG_FIXED:
			/* The magic number 390625 is 1e8 / 256 */
			if (closure->args[i].f >= 0)
				buffer_print(&b, "%d.%08d", closure->args[i].f / 256, 390625 * (closure->args[i].f % 256));
			else
				buffer_print(&b, "-%d.%08d", closure->args[i].f / -256, -390625 * (closure->args[i].f % 256));
			break;
		case POOR_WL_ARG_STRING:
			if (closure->args[i].s)
				buffer_print(&b, "\"%s\"", closure->args[i].s);
			else
				buffer_print(&b, "nil");
			break;
		case POOR_WL_ARG_OBJECT:
			if (closure->args[i].o)
				buffer_print(&b, "%s#%u", closure->args[i].o->interface->name, closure->args[i].o->id);
			else
				buffer_print(&b, "nil");
			break;
		case POOR_WL_ARG_NEW_ID:
			nval = n_parse ? n_parse(&closure->args[i]) : closure->args[i].n;
			buffer_print(&b, "new id %s#", closure->message->types[i] ? closure->message->types[i]->name : "[unknown]");
			if (nval)
				buffer_print(&b, "%u", nval);
			else
				buffer_print(&b, "nil");
			break;
		case POOR_WL_ARG_ARRAY:
			buffer_print(&b, "array[%zu]", closure->args[i].a->size);
			break;
		case POOR_WL_ARG_FD:
			buffer_print(&b, "fd %d", closure->args[i].h);
			break;
		}
	}
	buffer_print(&b, ")%s\n", color ? POOR_WL_DEBUG_COLOR_RESET : "");
	poor_wl_log("%s", b.data);
}
