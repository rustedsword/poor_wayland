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

#include <string.h>

#include "poor_wayland_util.h"

POOR_WL_EXPORT void poor_wl_list_init(struct poor_wl_list *list)
{
	list->prev = list;
	list->next = list;
}

POOR_WL_EXPORT void poor_wl_list_insert(struct poor_wl_list *list, struct poor_wl_list *elm)
{
	elm->prev = list;
	elm->next = list->next;
	list->next = elm;
	elm->next->prev = elm;
}

POOR_WL_EXPORT void poor_wl_list_remove(struct poor_wl_list *elm)
{
	elm->prev->next = elm->next;
	elm->next->prev = elm->prev;
	elm->next = nullptr;
	elm->prev = nullptr;
}

POOR_WL_EXPORT int poor_wl_list_length(const struct poor_wl_list *list)
{
	int count = 0;

	for (const struct poor_wl_list *e = list->next; e != list; e = e->next)
		count++;
	return count;
}

POOR_WL_EXPORT int poor_wl_list_empty(const struct poor_wl_list *list)
{
	return list->next == list;
}

POOR_WL_EXPORT void poor_wl_list_insert_list(struct poor_wl_list *list, struct poor_wl_list *other)
{
	if (poor_wl_list_empty(other))
		return;
	other->next->prev = list;
	other->prev->next = list->next;
	list->next->prev = other->prev;
	list->next = other->next;
}

POOR_WL_EXPORT void poor_wl_array_init(struct poor_wl_array *array, void *data, size_t alloc)
{
	array->size = 0;
	array->alloc = alloc;
	array->data = data;
}

POOR_WL_EXPORT void *poor_wl_array_add(struct poor_wl_array *array, size_t size)
{
	void *p;

	if (size > array->alloc - array->size)
		return nullptr;
	p = (char *)array->data + array->size;
	array->size += size;
	return p;
}

POOR_WL_EXPORT int poor_wl_array_copy(struct poor_wl_array *array, const struct poor_wl_array *source)
{
	if (source->size > array->alloc)
		return -1;
	if (source->size)
		memcpy(array->data, source->data, source->size);
	array->size = source->size;
	return 0;
}
