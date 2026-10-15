/*
 * Copyright © 2013 Marek Chalupa
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

#include "test-runner.h"
#include "wayland-server-private.h"

static void
signal_notify(struct poor_wl_listener *listener, void *data)
{
	/* only increase counter*/
	++(*((int *) data));
}

TEST(signal_init)
{
	struct poor_wl_priv_signal signal;

	poor_wl_priv_signal_init(&signal);

	/* Test if listeners' list is initialized */
	assert(&signal.listener_list == signal.listener_list.next
		&& "Maybe wl_priv_signal implementation changed?");
	assert(signal.listener_list.next == signal.listener_list.prev
		&& "Maybe wl_priv_signal implementation changed?");
}

TEST(signal_add_get)
{
	struct poor_wl_priv_signal signal;

	/* we just need different values of notify */
	struct poor_wl_listener l1 = {.notify = (poor_wl_notify_func_t) 0x1};
	struct poor_wl_listener l2 = {.notify = (poor_wl_notify_func_t) 0x2};
	struct poor_wl_listener l3 = {.notify = (poor_wl_notify_func_t) 0x3};
	/* one real, why not */
	struct poor_wl_listener l4 = {.notify = signal_notify};

	poor_wl_priv_signal_init(&signal);

	poor_wl_priv_signal_add(&signal, &l1);
	poor_wl_priv_signal_add(&signal, &l2);
	poor_wl_priv_signal_add(&signal, &l3);
	poor_wl_priv_signal_add(&signal, &l4);

	assert(poor_wl_priv_signal_get(&signal, signal_notify) == &l4);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x3) == &l3);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x2) == &l2);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x1) == &l1);

	/* get should not be destructive */
	assert(poor_wl_priv_signal_get(&signal, signal_notify) == &l4);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x3) == &l3);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x2) == &l2);
	assert(poor_wl_priv_signal_get(&signal, (poor_wl_notify_func_t) 0x1) == &l1);
}

TEST(signal_emit_to_one_listener)
{
	int count = 0;
	int counter;

	struct poor_wl_priv_signal signal;
	struct poor_wl_listener l1 = {.notify = signal_notify};

	poor_wl_priv_signal_init(&signal);
	poor_wl_priv_signal_add(&signal, &l1);

	for (counter = 0; counter < 100; counter++)
		poor_wl_priv_signal_emit(&signal, &count);

	assert(counter == count);
}

TEST(signal_emit_to_more_listeners)
{
	int count = 0;
	int counter;

	struct poor_wl_priv_signal signal;
	struct poor_wl_listener l1 = {.notify = signal_notify};
	struct poor_wl_listener l2 = {.notify = signal_notify};
	struct poor_wl_listener l3 = {.notify = signal_notify};

	poor_wl_priv_signal_init(&signal);
	poor_wl_priv_signal_add(&signal, &l1);
	poor_wl_priv_signal_add(&signal, &l2);
	poor_wl_priv_signal_add(&signal, &l3);

	for (counter = 0; counter < 100; counter++)
		poor_wl_priv_signal_emit(&signal, &count);

	assert(3 * counter == count);
}

struct signal
{
	struct poor_wl_priv_signal signal;
	struct poor_wl_listener l1, l2, l3;
	int count;
	struct poor_wl_listener *current;
};

static void notify_remove(struct poor_wl_listener *l, void *data)
{
	struct signal *sig = data;
	poor_wl_list_remove(&sig->current->link);
	poor_wl_list_init(&sig->current->link);
	sig->count++;
}

#define INIT \
	poor_wl_priv_signal_init(&signal.signal); \
	poor_wl_list_init(&signal.l1.link); \
	poor_wl_list_init(&signal.l2.link); \
	poor_wl_list_init(&signal.l3.link);
#define CHECK_EMIT(expected) \
	signal.count = 0; \
	poor_wl_priv_signal_emit(&signal.signal, &signal); \
	assert(signal.count == expected);

TEST(signal_remove_listener)
{
	test_set_timeout(4);

	struct signal signal;

	signal.l1.notify = notify_remove;
	signal.l2.notify = notify_remove;
	signal.l3.notify = notify_remove;

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);

	signal.current = &signal.l1;
	CHECK_EMIT(1)
	CHECK_EMIT(0)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	CHECK_EMIT(2)
	CHECK_EMIT(1)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.current = &signal.l2;
	CHECK_EMIT(1)
	CHECK_EMIT(1)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);
	poor_wl_priv_signal_add(&signal.signal, &signal.l3);

	signal.current = &signal.l1;
	CHECK_EMIT(3)
	CHECK_EMIT(2)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);
	poor_wl_priv_signal_add(&signal.signal, &signal.l3);

	signal.current = &signal.l2;
	CHECK_EMIT(2)
	CHECK_EMIT(2)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);
	poor_wl_priv_signal_add(&signal.signal, &signal.l3);

	signal.current = &signal.l3;
	CHECK_EMIT(2)
	CHECK_EMIT(2)
}

static void notify_readd(struct poor_wl_listener *l, void *data)
{
	struct signal *signal = data;
	if (signal->current) {
		poor_wl_list_remove(&signal->current->link);
		poor_wl_list_init(&signal->current->link);
		poor_wl_priv_signal_add(&signal->signal, signal->current);
	}
	signal->count++;
}

static void notify_empty(struct poor_wl_listener *l, void *data)
{
	struct signal *signal = data;
	signal->count++;
}

TEST(signal_readd_listener)
{
	/* Readding a listener is supported, that is it doesn't trigger an
	 * infinite loop or other weird things, but if in a listener you
	 * re-add another listener, that will not be fired in the current
	 * signal emission. */

	test_set_timeout(4);

	struct signal signal;

	signal.l1.notify = notify_readd;
	signal.l2.notify = notify_readd;

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);

	signal.current = &signal.l1;
	CHECK_EMIT(1)
	CHECK_EMIT(1)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);

	signal.current = &signal.l2;
	CHECK_EMIT(1)
	signal.current = NULL;
	CHECK_EMIT(2)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.current = &signal.l1;
	CHECK_EMIT(1)
	/* l2 was added before l1, so l2 is fired first, which by readding l1
	 * removes it from the current list that is being fired, so 1 is correct */
	CHECK_EMIT(1)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.l1.notify = notify_empty;
	signal.current = &signal.l2;
	CHECK_EMIT(2)
	CHECK_EMIT(2)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.l1.notify = notify_empty;
	signal.current = &signal.l1;
	CHECK_EMIT(2)
	/* same as before, by readding l1 in the first emit, it now is fired
	 * after l2, so on the second emit it is not fired at all. */
	CHECK_EMIT(1)
}

static void notify_addandget(struct poor_wl_listener *l, void *data)
{
	struct signal *signal = data;
	poor_wl_list_remove(&signal->current->link);
	poor_wl_list_init(&signal->current->link);
	poor_wl_priv_signal_add(&signal->signal, signal->current);

	assert(poor_wl_priv_signal_get(&signal->signal, signal->current->notify) != NULL);

	signal->count++;
}

static void notify_get(struct poor_wl_listener *l, void *data)
{
	struct signal *signal = data;
	assert(poor_wl_priv_signal_get(&signal->signal, signal->current->notify) == signal->current);
	signal->count++;
}

TEST(signal_get_listener)
{
	test_set_timeout(4);

	struct signal signal;

	signal.l1.notify = notify_addandget;
	signal.l2.notify = notify_get;

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);

	signal.current = &signal.l2;
	CHECK_EMIT(1)

	INIT
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.current = &signal.l2;
	CHECK_EMIT(1)

	INIT
	signal.l1.notify = notify_get;
	signal.l2.notify = notify_empty;
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	CHECK_EMIT(2)

	INIT
	signal.l1.notify = notify_empty;
	signal.l2.notify = notify_get;
	poor_wl_priv_signal_add(&signal.signal, &signal.l1);
	poor_wl_priv_signal_add(&signal.signal, &signal.l2);

	signal.current = &signal.l1;
	CHECK_EMIT(2)
}
