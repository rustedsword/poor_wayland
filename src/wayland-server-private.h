/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
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

#ifndef POOR_WAYLAND_SERVER_PRIVATE_H
#define POOR_WAYLAND_SERVER_PRIVATE_H

#include "poor_wayland_server_core.h"

void
poor_wl_priv_signal_init(struct poor_wl_priv_signal *signal);

void
poor_wl_priv_signal_add(struct poor_wl_priv_signal *signal, struct poor_wl_listener *listener);

struct poor_wl_listener *
poor_wl_priv_signal_get(struct poor_wl_priv_signal *signal, poor_wl_notify_func_t notify);

void
poor_wl_priv_signal_emit(struct poor_wl_priv_signal *signal, void *data);

void
poor_wl_priv_signal_final_emit(struct poor_wl_priv_signal *signal, void *data);

#endif
