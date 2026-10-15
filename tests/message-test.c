/*
 * Copyright © 2014 Jonas Ådahl
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

#include "poor_wayland_client.h"
#include "wayland-private.h"
#include "poor_wayland_server.h"
#include "test-runner.h"

TEST(message_version)
{
	unsigned int i;
	const struct {
		const struct poor_wl_message *message;
		int expected_version;
	} messages[] = {
		{ &poor_wl_pointer_interface.events[POOR_WL_POINTER_ENTER], 1 },
		{ &poor_wl_surface_interface.events[POOR_WL_SURFACE_ENTER], 1 },
		{ &poor_wl_pointer_interface.methods[POOR_WL_POINTER_SET_CURSOR], 1 },
		{ &poor_wl_pointer_interface.methods[POOR_WL_POINTER_RELEASE], 3 },
		{ &poor_wl_surface_interface.methods[POOR_WL_SURFACE_DESTROY], 1 },
		{ &poor_wl_surface_interface.methods[POOR_WL_SURFACE_SET_BUFFER_TRANSFORM], 2 },
		{ &poor_wl_surface_interface.methods[POOR_WL_SURFACE_SET_BUFFER_SCALE], 3 },
	};

	for (i = 0; i < ARRAY_SIZE(messages); ++i) {
		assert(poor_wl_message_get_since(messages[i].message) ==
		       messages[i].expected_version);
	}
}

TEST(message_count_fds)
{
	const struct {
		struct poor_wl_message message;
		int expected_fd_count;
	} messages[] = {
		{ { "empty", "", nullptr }, 0 },
		{ { "none", "iufsona", nullptr }, 0 },
		{ { "one", "iufsonah", nullptr }, 1 },
		{ { "versioned", "2hh?s", nullptr }, 2 },
	};

	for (unsigned i = 0; i < ARRAY_SIZE(messages); ++i)
		assert(poor_wl_message_count_fds(&messages[i].message) == messages[i].expected_fd_count);
	assert(poor_wl_message_count_fds(&poor_wl_keyboard_interface.events[POOR_WL_KEYBOARD_KEYMAP]) == 1);
}
