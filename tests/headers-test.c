/*
 * Copyright © 2015 Giulio Camuffo
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

#include "poor_wayland_client_core.h"
#include "poor_wayland_server_core.h"

#ifdef POOR_WL_DISPLAY_SYNC
#error including poor_wayland_client_core.h imported protocol symbols!
#endif
#ifdef POOR_WL_DISPLAY_ERROR
#error including poor_wayland_server_core.h imported protocol symbols!
#endif

#ifdef POOR_WAYLAND_CLIENT_H
#error including poor_wayland_client_core.h included the non-core header!
#endif
#ifdef POOR_WAYLAND_SERVER_H
#error including poor_wayland_server_core.h included the non-core header!
#endif

#include "poor_wayland_client.h"
#include "poor_wayland_server.h"

#ifndef POOR_WL_DISPLAY_SYNC
#error including poor_wayland_client.h did not import protocol symbols!
#endif
#ifndef POOR_WL_DISPLAY_ERROR
#error including poor_wayland_server.h did not import protocol symbols!
#endif

int main(int argc, char **argv) { return 0; }
