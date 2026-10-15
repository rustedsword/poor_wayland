/*
 * Copyright © 2008 Kristian Høgsberg
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

#ifndef WAYLAND_CLIENT_CORE_H
#define WAYLAND_CLIENT_CORE_H

#include <poor_loop.h>
#include <poor_loop_group.h>
#include <stdint.h>

#include "wayland-connection.h"
#include "wayland-util.h"
#include "wayland-version.h"

#ifdef  __cplusplus
extern "C" {
#endif

/** \class wl_proxy
 *
 * \brief Represents a protocol object on the client side.
 *
 * A wl_proxy acts as a client side proxy to an object existing in the
 * compositor. The proxy is responsible for converting requests made by the
 * clients with \ref wl_proxy_marshal_flags() into Wayland's wire format.
 * Events coming from the compositor are also handled by the proxy, which will
 * in turn call the handler set with \ref wl_proxy_add_listener().
 *
 * Proxies live in memory of the caller. A request that creates an object
 * takes the memory of its proxy, which stays in use until the proxy is
 * destroyed. Proxies of objects that the compositor creates come from the
 * function set with \ref wl_display_set_new_proxy().
 */
struct wl_proxy;

/** \class wl_display
 *
 * \brief Represents a connection to the compositor and acts as a proxy to
 * the wl_display singleton object.
 *
 * A wl_display runs on a poor_loop and can only be used from its thread.
 * Requests are queued in the display's buffer and sent when the loop
 * iteration that queued them ends, or right away with \ref wl_display_flush().
 * Events are dispatched to the listeners of their proxies as soon as they
 * arrive.
 *
 * Nothing blocks: a roundtrip is a wl_display.sync request whose callback
 * calls the code that waits for it.
 */
struct wl_display;

/** Destroy proxy after marshalling
 * \relates wl_proxy
 */
#define WL_MARSHAL_FLAG_DESTROY (1 << 0)

/* Called once, from the loop, when the connection fails. See wl_display_get_error(). */
typedef void wl_display_error_fn(struct wl_display *display);
/* Called once wl_display_disconnect() has finished, after which the display memory is not used anymore */
typedef void wl_display_done_fn(struct wl_display *display);
/* Return memory for the proxy of an object that 'factory' got from the compositor, or NULL to fail the display */
typedef struct wl_proxy *wl_display_new_proxy_fn(struct wl_display *display, struct wl_proxy *factory,
						 const struct wl_interface *interface);

struct wl_proxy {
	struct wl_object object;
	struct wl_display *display;
	void *user_data;
	wl_dispatcher_func_t dispatcher;
	const char *const *tag;
	uint32_t version;
	uint32_t flags;
};


struct wl_proxy *
wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
		       const struct wl_interface *interface,
		       uint32_t version,
		       uint32_t flags, ...);

struct wl_proxy *
wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
			     const struct wl_interface *interface,
			     uint32_t version,
			     uint32_t flags,
			     union wl_argument *args);

void
wl_proxy_destroy(struct wl_proxy *proxy);

int
wl_proxy_add_listener(struct wl_proxy *proxy,
		      void (**implementation)(void), void *data);

const void *
wl_proxy_get_listener(struct wl_proxy *proxy);

int
wl_proxy_add_dispatcher(struct wl_proxy *proxy,
			wl_dispatcher_func_t dispatcher_func,
			const void * dispatcher_data, void *data);

void
wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data);

void *
wl_proxy_get_user_data(struct wl_proxy *proxy);

uint32_t
wl_proxy_get_version(struct wl_proxy *proxy);

uint32_t
wl_proxy_get_id(struct wl_proxy *proxy);

void
wl_proxy_set_tag(struct wl_proxy *proxy,
		 const char * const *tag);

const char * const *
wl_proxy_get_tag(struct wl_proxy *proxy);

const char *
wl_proxy_get_class(struct wl_proxy *proxy);

const struct wl_interface *
wl_proxy_get_interface(struct wl_proxy *proxy);

struct wl_display *
wl_proxy_get_display(struct wl_proxy *proxy);

/*
 * Connect to the display 'name' in XDG_RUNTIME_DIR, or at the path 'name' if
 * it is absolute. Without 'name', WAYLAND_DISPLAY or "wayland-0" is used. If
 * WAYLAND_SOCKET is set, it is the fd of a connected socket to use instead.
 *
 * Requests can be made right away, they are sent once the connection is up.
 * Returns 0, or -1 with errno set if the address is invalid, then the display
 * is not initialized.
 */
int
wl_display_connect(struct wl_display *display, struct poor_loop *loop,
		   const struct wl_connection_memory *memory, const char *name,
		   wl_display_error_fn *error, wl_display_done_fn *done);

/* Use the connected socket 'fd', which the display owns */
void
wl_display_connect_to_fd(struct wl_display *display, struct poor_loop *loop,
			 const struct wl_connection_memory *memory, int fd,
			 wl_display_error_fn *error, wl_display_done_fn *done);

/* Close the connection. Proxies are left as they are and 'done' is called later from the loop. */
void
wl_display_disconnect(struct wl_display *display);

void
wl_display_set_new_proxy(struct wl_display *display,
			 wl_display_new_proxy_fn *new_proxy);

int
wl_display_get_fd(struct wl_display *display);

/* Start sending the queued requests. Returns 0, or -1 with errno set to the display error. */
int
wl_display_flush(struct wl_display *display);

int
wl_display_get_error(struct wl_display *display);

uint32_t
wl_display_get_protocol_error(struct wl_display *display,
			      const struct wl_interface **interface,
			      uint32_t *id);

void
wl_log_set_handler_client(wl_log_func_t handler);

#ifdef  __cplusplus
}
#endif

#endif
