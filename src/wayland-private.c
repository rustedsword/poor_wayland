/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wayland-private.h"

/*
 * In most cases the pointer equality test is sufficient. However, depending on how things are split across shared
 * objects, there can be multiple instances of the interface metadata constants.
 */
int wl_interface_equal(const struct wl_interface *a, const struct wl_interface *b)
{
	return a == b || strcmp(a->name, b->name) == 0;
}

/* Bit 0 marks a free entry that links the free list, bit 1 holds the flag */
static bool map_entry_is_free(union wl_map_entry entry)
{
	return entry.next & 0x1;
}

static void *map_entry_get_data(union wl_map_entry entry)
{
	return (void *)(entry.next & ~(uintptr_t)0x3);
}

static uint32_t map_entry_get_flags(union wl_map_entry entry)
{
	return (entry.next >> 1) & 0x1;
}

static void map_entry_set(union wl_map_entry *entry, uint32_t flags, void *data)
{
	assert(!((uintptr_t)data & 0x3));
	entry->data = data;
	entry->next |= (flags & 0x1) << 1;
}

void wl_map_init(struct wl_map *map, uint32_t side, const struct wl_connection_memory *memory)
{
	map->client_entries = memory->client_objects;
	map->server_entries = memory->server_objects;
	map->client_size = memory->client_objects_size;
	map->server_size = memory->server_objects_size;
	map->client_count = map->server_count = 0;
	map->side = side;
	map->free_list = 0;
}

/* Selects the entries of 'i' and makes it an index into them */
static union wl_map_entry *map_entries(struct wl_map *map, uint32_t *i, uint32_t **count, uint32_t *size)
{
	if (*i < WL_SERVER_ID_START) {
		*count = &map->client_count;
		*size = map->client_size;
		return map->client_entries;
	}
	*i -= WL_SERVER_ID_START;
	*count = &map->server_count;
	*size = map->server_size;
	return map->server_entries;
}

uint32_t wl_map_insert_new(struct wl_map *map, uint32_t flags, void *data)
{
	uint32_t base = map->side == WL_MAP_CLIENT_SIDE ? 0 : WL_SERVER_ID_START, i = base, *count, size;
	union wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (map->free_list) {
		i = map->free_list >> 1;
		map->free_list = entries[i].next;
	} else if (*count < size) {
		i = (*count)++;
	} else {
		errno = ENOSPC;
		return 0;
	}
	map_entry_set(&entries[i], flags, data);
	return i + base;
}

int wl_map_insert_at(struct wl_map *map, uint32_t flags, uint32_t i, void *data)
{
	uint32_t *count, size;
	union wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (i >= size) {
		errno = ENOSPC;
		return -1;
	}
	if (*count < i) {
		errno = EINVAL;
		return -1;
	}
	if (*count == i)
		(*count)++;
	map_entry_set(&entries[i], flags, data);
	return 0;
}

/* The peer allocates its ids in order, reusing the ones of destroyed objects and of zombies */
int wl_map_reserve_new(struct wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union wl_map_entry *entries;

	if ((i < WL_SERVER_ID_START) != (map->side == WL_MAP_SERVER_SIDE)) {
		errno = EINVAL;
		return -1;
	}
	entries = map_entries(map, &i, &count, &size);
	if (i >= size) {
		errno = ENOSPC;
		return -1;
	}
	if (*count < i) {
		errno = EINVAL;
		return -1;
	}
	if (*count == i) {
		(*count)++;
	} else if (map_entry_get_data(entries[i]) && !map_entry_get_flags(entries[i])) {
		errno = EINVAL;
		return -1;
	}
	entries[i].data = nullptr;
	return 0;
}

void wl_map_remove(struct wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union wl_map_entry *entries;

	if ((i < WL_SERVER_ID_START) != (map->side == WL_MAP_CLIENT_SIDE))
		return;
	entries = map_entries(map, &i, &count, &size);
	entries[i].next = map->free_list;
	map->free_list = (i << 1) | 1;
}

void *wl_map_lookup(struct wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (i < *count && !map_entry_is_free(entries[i]))
		return map_entry_get_data(entries[i]);
	return nullptr;
}

uint32_t wl_map_lookup_flags(struct wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (i < *count && !map_entry_is_free(entries[i]))
		return map_entry_get_flags(entries[i]);
	return 0;
}

/* The callback can add and remove entries */
static enum wl_iterator_result for_each_helper(union wl_map_entry *entries, const uint32_t *count, wl_iterator_func_t func,
					       void *data)
{
	enum wl_iterator_result ret = WL_ITERATOR_CONTINUE;

	for (uint32_t i = 0; i < *count && ret == WL_ITERATOR_CONTINUE; i++) {
		union wl_map_entry entry = entries[i];

		if (entry.data && !map_entry_is_free(entry))
			ret = func(map_entry_get_data(entry), data, map_entry_get_flags(entry));
	}
	return ret;
}

void wl_map_for_each(struct wl_map *map, wl_iterator_func_t func, void *data)
{
	if (for_each_helper(map->client_entries, &map->client_count, func, data) == WL_ITERATOR_CONTINUE)
		for_each_helper(map->server_entries, &map->server_count, func, data);
}

wl_log_func_t wl_log_handler;

void wl_log(const char *fmt, ...)
{
	va_list argp;

	if (!wl_log_handler)
		return;
	va_start(argp, fmt);
	wl_log_handler(fmt, argp);
	va_end(argp);
}

/* The process ends right away, so the message can't go through the loop */
void wl_abort(const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	vdprintf(STDERR_FILENO, fmt, argp);
	va_end(argp);
	abort();
}
