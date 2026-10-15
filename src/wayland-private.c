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
int poor_wl_interface_equal(const struct poor_wl_interface *a, const struct poor_wl_interface *b)
{
	return a == b || strcmp(a->name, b->name) == 0;
}

/* Bit 0 marks a free entry that links the free list, bit 1 holds the flag */
static bool map_entry_is_free(union poor_wl_map_entry entry)
{
	return entry.next & 0x1;
}

static void *map_entry_get_data(union poor_wl_map_entry entry)
{
	return (void *)(entry.next & ~(uintptr_t)0x3);
}

static uint32_t map_entry_get_flags(union poor_wl_map_entry entry)
{
	return (entry.next >> 1) & 0x1;
}

static void map_entry_set(union poor_wl_map_entry *entry, uint32_t flags, void *data)
{
	assert(!((uintptr_t)data & 0x3));
	entry->data = data;
	entry->next |= (flags & 0x1) << 1;
}

void poor_wl_map_init(struct poor_wl_map *map, uint32_t side, const struct poor_wl_connection_memory *memory)
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
static union poor_wl_map_entry *map_entries(struct poor_wl_map *map, uint32_t *i, uint32_t **count, uint32_t *size)
{
	if (*i < POOR_WL_SERVER_ID_START) {
		*count = &map->client_count;
		*size = map->client_size;
		return map->client_entries;
	}
	*i -= POOR_WL_SERVER_ID_START;
	*count = &map->server_count;
	*size = map->server_size;
	return map->server_entries;
}

uint32_t poor_wl_map_insert_new(struct poor_wl_map *map, uint32_t flags, void *data)
{
	uint32_t base = map->side == POOR_WL_MAP_CLIENT_SIDE ? 0 : POOR_WL_SERVER_ID_START, i = base, *count, size;
	union poor_wl_map_entry *entries = map_entries(map, &i, &count, &size);

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

int poor_wl_map_insert_at(struct poor_wl_map *map, uint32_t flags, uint32_t i, void *data)
{
	uint32_t *count, size;
	union poor_wl_map_entry *entries = map_entries(map, &i, &count, &size);

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
int poor_wl_map_reserve_new(struct poor_wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union poor_wl_map_entry *entries;

	if ((i < POOR_WL_SERVER_ID_START) != (map->side == POOR_WL_MAP_SERVER_SIDE)) {
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

void poor_wl_map_remove(struct poor_wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union poor_wl_map_entry *entries;

	if ((i < POOR_WL_SERVER_ID_START) != (map->side == POOR_WL_MAP_CLIENT_SIDE))
		return;
	entries = map_entries(map, &i, &count, &size);
	entries[i].next = map->free_list;
	map->free_list = (i << 1) | 1;
}

void *poor_wl_map_lookup(struct poor_wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union poor_wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (i < *count && !map_entry_is_free(entries[i]))
		return map_entry_get_data(entries[i]);
	return nullptr;
}

uint32_t poor_wl_map_lookup_flags(struct poor_wl_map *map, uint32_t i)
{
	uint32_t *count, size;
	union poor_wl_map_entry *entries = map_entries(map, &i, &count, &size);

	if (i < *count && !map_entry_is_free(entries[i]))
		return map_entry_get_flags(entries[i]);
	return 0;
}

/* The callback can add and remove entries */
static enum poor_wl_iterator_result for_each_helper(union poor_wl_map_entry *entries, const uint32_t *count, poor_wl_iterator_func_t func,
					       void *data)
{
	enum poor_wl_iterator_result ret = POOR_WL_ITERATOR_CONTINUE;

	for (uint32_t i = 0; i < *count && ret == POOR_WL_ITERATOR_CONTINUE; i++) {
		union poor_wl_map_entry entry = entries[i];

		if (entry.data && !map_entry_is_free(entry))
			ret = func(map_entry_get_data(entry), data, map_entry_get_flags(entry));
	}
	return ret;
}

void poor_wl_map_for_each(struct poor_wl_map *map, poor_wl_iterator_func_t func, void *data)
{
	if (for_each_helper(map->client_entries, &map->client_count, func, data) == POOR_WL_ITERATOR_CONTINUE)
		for_each_helper(map->server_entries, &map->server_count, func, data);
}

poor_wl_log_func_t poor_wl_log_handler;

void poor_wl_log(const char *fmt, ...)
{
	va_list argp;

	if (!poor_wl_log_handler)
		return;
	va_start(argp, fmt);
	poor_wl_log_handler(fmt, argp);
	va_end(argp);
}

/* The process ends right away, so the message can't go through the loop */
void poor_wl_abort(const char *fmt, ...)
{
	va_list argp;

	va_start(argp, fmt);
	vdprintf(STDERR_FILENO, fmt, argp);
	va_end(argp);
	abort();
}
