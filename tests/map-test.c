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
#include <stdint.h>

#include "test-runner.h"
#include "wayland-private.h"

struct map_memory {
	union wl_map_entry client[8];
	union wl_map_entry server[4];
};

static void map_init(struct wl_map *map, uint32_t side, struct map_memory *m)
{
	struct wl_connection_memory memory = {
		.client_objects = m->client,
		.server_objects = m->server,
		.client_objects_size = 8,
		.server_objects_size = 4,
	};

	wl_map_init(map, side, &memory);
}

TEST(map_insert_new)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t i, j, k, a, b, c;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	i = wl_map_insert_new(&map, 0, &a);
	j = wl_map_insert_new(&map, 0, &b);
	k = wl_map_insert_new(&map, 0, &c);
	assert(i == WL_SERVER_ID_START);
	assert(j == WL_SERVER_ID_START + 1);
	assert(k == WL_SERVER_ID_START + 2);
	assert(wl_map_lookup(&map, i) == &a);
	assert(wl_map_lookup(&map, j) == &b);
	assert(wl_map_lookup(&map, k) == &c);

	map_init(&map, WL_MAP_CLIENT_SIDE, &memory);
	i = wl_map_insert_new(&map, 0, &a);
	assert(i == 0);
	assert(wl_map_lookup(&map, i) == &a);
}

TEST(map_insert_new_full)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t a;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	for (int i = 0; i < 4; i++)
		assert(wl_map_insert_new(&map, 0, &a) == WL_SERVER_ID_START + i);
	errno = 0;
	assert(wl_map_insert_new(&map, 0, &a) == 0);
	assert(errno == ENOSPC);
	wl_map_remove(&map, WL_SERVER_ID_START + 2);
	assert(wl_map_insert_new(&map, 0, &a) == WL_SERVER_ID_START + 2);
}

TEST(map_insert_at)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t a, b, c;

	map_init(&map, WL_MAP_CLIENT_SIDE, &memory);
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START, &a) == 0);
	errno = 0;
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START + 3, &b) == -1);
	assert(errno == EINVAL);
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START + 1, &c) == 0);
	assert(wl_map_lookup(&map, WL_SERVER_ID_START) == &a);
	assert(wl_map_lookup(&map, WL_SERVER_ID_START + 1) == &c);
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START + 2, &b) == 0);
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START + 3, &b) == 0);
	errno = 0;
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START + 4, &b) == -1);
	assert(errno == ENOSPC);
}

TEST(map_remove)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t i, j, k, l, a, b, c, d;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	i = wl_map_insert_new(&map, 0, &a);
	j = wl_map_insert_new(&map, 0, &b);
	k = wl_map_insert_new(&map, 0, &c);
	assert(wl_map_lookup(&map, i) == &a);
	assert(wl_map_lookup(&map, j) == &b);
	assert(wl_map_lookup(&map, k) == &c);
	wl_map_remove(&map, j);
	assert(wl_map_lookup(&map, j) == nullptr);
	l = wl_map_insert_new(&map, 0, &d);
	assert(l == WL_SERVER_ID_START + 1);
	assert(wl_map_lookup(&map, l) == &d);
}

TEST(map_flags)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t i, j, a, b;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	i = wl_map_insert_new(&map, 0, &a);
	j = wl_map_insert_new(&map, 1, &b);
	assert(wl_map_lookup(&map, i) == &a);
	assert(wl_map_lookup(&map, j) == &b);
	assert(wl_map_lookup_flags(&map, i) == 0);
	assert(wl_map_lookup_flags(&map, j) == 1);
}

TEST(map_reserve_new)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t a;

	map_init(&map, WL_MAP_CLIENT_SIDE, &memory);
	assert(wl_map_reserve_new(&map, 1) == -1 && errno == EINVAL);
	assert(wl_map_reserve_new(&map, WL_SERVER_ID_START + 1) == -1 && errno == EINVAL);
	assert(wl_map_reserve_new(&map, WL_SERVER_ID_START) == 0);
	assert(wl_map_insert_at(&map, 0, WL_SERVER_ID_START, &a) == 0);
	assert(wl_map_reserve_new(&map, WL_SERVER_ID_START) == -1 && errno == EINVAL);
	assert(wl_map_insert_at(&map, WL_MAP_ENTRY_ZOMBIE, WL_SERVER_ID_START, &a) == 0);
	assert(wl_object_is_zombie(&map, WL_SERVER_ID_START));
	assert(wl_map_reserve_new(&map, WL_SERVER_ID_START) == 0);
	assert(!wl_object_is_zombie(&map, WL_SERVER_ID_START));
	assert(wl_map_lookup(&map, WL_SERVER_ID_START) == nullptr);

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	assert(wl_map_reserve_new(&map, WL_SERVER_ID_START) == -1 && errno == EINVAL);
	for (uint32_t i = 0; i < 8; i++)
		assert(wl_map_reserve_new(&map, i) == 0);
	assert(wl_map_reserve_new(&map, 8) == -1 && errno == ENOSPC);
}

static enum wl_iterator_result never_run(void *, void *, uint32_t)
{
	assert(0);
}

TEST(map_iter_empty)
{
	struct map_memory memory;
	struct wl_map map;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	wl_map_for_each(&map, never_run, nullptr);
}

static enum wl_iterator_result count(void *, void *data, uint32_t)
{
	(*(int *)data)++;
	return WL_ITERATOR_CONTINUE;
}

TEST(map_iter_skips_empty_entries)
{
	struct map_memory memory;
	struct wl_map map;
	uint32_t a, b;
	int n = 0;

	map_init(&map, WL_MAP_SERVER_SIDE, &memory);
	assert(wl_map_insert_at(&map, 0, 0, nullptr) == 0);
	assert(wl_map_insert_at(&map, 0, 1, &a) == 0);
	assert(wl_map_insert_at(&map, 0, 2, nullptr) == 0);
	assert(wl_map_insert_new(&map, 0, &b) == WL_SERVER_ID_START);
	wl_map_for_each(&map, count, &n);
	assert(n == 2);
}
