/* SPDX-License-Identifier: MIT */
#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <poor_loop.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * A compositor for the tests, in its own translation unit since the client and server headers define different
 * struct poor_wl_display. It runs on the loop of the test and announces poor_test_global, poor_wl_fixes and poor_wl_shm.
 */
struct test_server_state {
	unsigned clients_created;
	unsigned clients_done;
	unsigned withdrawn;
	unsigned children;
	unsigned shm_buffers;
	unsigned logged_requests;
	unsigned logged_events;
	pid_t pid;
	uid_t uid;
	int socket_error;
	bool socket_ready;
	bool socket_done;
	bool display_done;
	uint32_t shm_pixel;
};

extern struct test_server_state test_server;

void test_server_start(struct poor_loop *loop);
/* Returns the fd of the client end of a new connection */
int test_server_connect(void);
/* Like test_server_connect() with a client whose buffers and object map have the smallest sizes */
int test_server_connect_small(void);
/* Like test_server_connect() with a server end that can't queue more than 'sndbuf' bytes */
int test_server_connect_sndbuf(int sndbuf);
void test_server_stop(void);
void test_server_listen(const char *name);
void test_server_listen_auto(void);
const char *test_server_socket_name(void);
void test_server_close_socket(void);
void test_server_remove_global(void);
void test_server_destroy_clients(void);
bool test_server_global_destroyed(void);
void test_server_log_messages(void);
/* Hide the poor_test_child global from clients */
void test_server_hide_extra(void);
/* Accept on 'fd', which listens already */
void test_server_listen_fd(int fd);
/* Whether a client has a send in flight */
bool test_server_sending(void);

#endif
