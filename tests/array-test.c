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
#include <string.h>

#include "test-runner.h"
#include "poor_wayland_util.h"

TEST(array_init)
{
	struct poor_wl_array array;
	char storage[16];

	poor_wl_array_init(&array, storage, sizeof(storage));
	assert(array.size == 0);
	assert(array.alloc == sizeof(storage));
	assert(array.data == storage);
}

TEST(array_init_macro)
{
	int storage[4];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(storage);

	assert(array.size == 0);
	assert(array.alloc == sizeof(storage));
	assert(array.data == storage);
}

TEST(array_add)
{
	struct mydata {
		unsigned int a;
		unsigned int b;
		double c;
		double d;
	};
	constexpr unsigned iterations = 1321;
	static struct mydata storage[iterations];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(storage);

	for (unsigned i = 0; i < iterations; i++) {
		struct mydata *ptr = poor_wl_array_add(&array, sizeof(*ptr));

		assert(ptr == &storage[i]);
		assert((i + 1) * sizeof(*ptr) == array.size);
		ptr->a = i * 3;
		ptr->b = 20000 - i;
		ptr->c = (double)i;
		ptr->d = (double)(i / 2.);
	}
	for (unsigned i = 0; i < iterations; ++i) {
		struct mydata *check = (struct mydata *)array.data + i;

		assert(check->a == i * 3);
		assert(check->b == 20000 - i);
		assert(check->c == (double)i);
		assert(check->d == (double)(i / 2.));
	}
}

TEST(array_add_beyond_storage)
{
	char storage[10];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(storage);

	assert(poor_wl_array_add(&array, 6) == storage);
	assert(!poor_wl_array_add(&array, 5));
	assert(array.size == 6);
	assert(poor_wl_array_add(&array, 4) == storage + 6);
	assert(!poor_wl_array_add(&array, 1));
	assert(poor_wl_array_add(&array, 0) == storage + 10);
}

TEST(array_copy)
{
	constexpr int iterations = 1529;
	static int source_storage[iterations], copy_storage[iterations], small_storage[iterations - 1];
	struct poor_wl_array source = POOR_WL_ARRAY_INIT(source_storage);
	struct poor_wl_array copy = POOR_WL_ARRAY_INIT(copy_storage);
	struct poor_wl_array small = POOR_WL_ARRAY_INIT(small_storage);

	for (int i = 0; i < iterations; i++) {
		int *p = poor_wl_array_add(&source, sizeof(int));

		assert(p);
		*p = i * 2 + i;
	}
	assert(poor_wl_array_copy(&copy, &source) == 0);
	assert(copy.size == source.size);
	for (int i = 0; i < iterations; i++) {
		int *s = (int *)source.data + i;
		int *c = (int *)copy.data + i;

		assert(*s == *c);
		assert(s != c);
		assert(*s == i * 2 + i);
	}
	assert(poor_wl_array_copy(&small, &source) == -1);
	assert(small.size == 0);
}

TEST(array_for_each)
{
	static const int elements[] = { 77, 12, 45192, 53280, 334455 };
	int storage[5];
	struct poor_wl_array array = POOR_WL_ARRAY_INIT(storage);
	int *p;
	int i;

	for (i = 0; i < 5; i++) {
		p = poor_wl_array_add(&array, sizeof(*p));
		assert(p);
		*p = elements[i];
	}
	i = 0;
	poor_wl_array_for_each(p, &array) {
		assert(*p == elements[i]);
		i++;
	}
	assert(i == 5);
}
