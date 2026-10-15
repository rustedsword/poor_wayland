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

#ifndef POOR_WAYLAND_CLIENT_CORE_H
#define POOR_WAYLAND_CLIENT_CORE_H

#include <poor_loop.h>
#include <poor_loop_group.h>
#include <stdint.h>

#include "poor_wayland_connection.h"
#include "poor_wayland_util.h"
#include "poor_wayland_version.h"

#ifdef  __cplusplus
extern "C" {
#endif

/** \class poor_wl_proxy
 *
 * \brief Represents a protocol object on the client side.
 *
 * A poor_wl_proxy acts as a client side proxy to an object existing in the
 * compositor. The proxy is responsible for converting requests made by the
 * clients with \ref poor_wl_proxy_marshal_flags() into Wayland's wire format.
 * Events coming from the compositor are also handled by the proxy, which will
 * in turn call the handler set with \ref poor_wl_proxy_add_listener().
 *
 * Proxies live in memory of the caller. Every interface has a structure
 * that holds its proxy, such as struct poor_wl_surface, and a request that
 * creates an object takes that structure, which stays in use until the
 * proxy is destroyed. Objects that the compositor creates come from the
 * function set with \ref poor_wl_display_set_new_proxy().
 */
struct poor_wl_proxy;

/** \class poor_wl_display
 *
 * \brief Represents a connection to the compositor and acts as a proxy to
 * the poor_wl_display singleton object.
 *
 * A poor_wl_display runs on a poor_loop and can only be used from its thread.
 * Requests are queued in the display's buffer and sent when the loop
 * iteration that queued them ends, or right away with \ref poor_wl_display_flush().
 * Events are dispatched to the listeners of their proxies as soon as they
 * arrive.
 *
 * Nothing blocks: a roundtrip is a wl_display.sync request whose callback
 * calls the code that waits for it.
 */
struct poor_wl_display;

/** Destroy proxy after marshalling
 * \relates poor_wl_proxy
 */
#define POOR_WL_MARSHAL_FLAG_DESTROY (1 << 0)

/* Called once, from the loop, when the connection fails. See poor_wl_display_get_error(). */
typedef void poor_wl_display_error_fn(struct poor_wl_display *display);
/* Called once poor_wl_display_disconnect() has finished, after which the display memory is not used anymore */
typedef void poor_wl_display_done_fn(struct poor_wl_display *display);
/* Return the proxy of a new object of 'interface' that 'factory' got from the compositor, or NULL to fail the display */
typedef struct poor_wl_proxy *poor_wl_display_new_proxy_fn(struct poor_wl_display *display, struct poor_wl_proxy *factory,
						 const struct poor_wl_interface *interface);

struct poor_wl_proxy {
	struct poor_wl_object object;
	struct poor_wl_display *display;
	void *user_data;
	poor_wl_dispatcher_func_t dispatcher;
	const char *const *tag;
	uint32_t version;
	uint32_t flags;
};


struct poor_wl_proxy *
poor_wl_proxy_marshal_flags(struct poor_wl_proxy *proxy, uint32_t opcode,
		       const struct poor_wl_interface *interface,
		       uint32_t version,
		       uint32_t flags, ...);

struct poor_wl_proxy *
poor_wl_proxy_marshal_array_flags(struct poor_wl_proxy *proxy, uint32_t opcode,
			     const struct poor_wl_interface *interface,
			     uint32_t version,
			     uint32_t flags,
			     union poor_wl_argument *args);

void
poor_wl_proxy_destroy(struct poor_wl_proxy *proxy);

int
poor_wl_proxy_add_listener(struct poor_wl_proxy *proxy,
		      void (**implementation)(void), void *data);

const void *
poor_wl_proxy_get_listener(struct poor_wl_proxy *proxy);

int
poor_wl_proxy_add_dispatcher(struct poor_wl_proxy *proxy,
			poor_wl_dispatcher_func_t dispatcher_func,
			const void * dispatcher_data, void *data);

void
poor_wl_proxy_set_user_data(struct poor_wl_proxy *proxy, void *user_data);

void *
poor_wl_proxy_get_user_data(struct poor_wl_proxy *proxy);

uint32_t
poor_wl_proxy_get_version(struct poor_wl_proxy *proxy);

uint32_t
poor_wl_proxy_get_id(struct poor_wl_proxy *proxy);

void
poor_wl_proxy_set_tag(struct poor_wl_proxy *proxy,
		 const char * const *tag);

const char * const *
poor_wl_proxy_get_tag(struct poor_wl_proxy *proxy);

const char *
poor_wl_proxy_get_class(struct poor_wl_proxy *proxy);

const struct poor_wl_interface *
poor_wl_proxy_get_interface(struct poor_wl_proxy *proxy);

struct poor_wl_display *
poor_wl_proxy_get_display(struct poor_wl_proxy *proxy);

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
poor_wl_display_connect(struct poor_wl_display *display, struct poor_loop *loop,
		   const struct poor_wl_connection_memory *memory, const char *name,
		   poor_wl_display_error_fn *error, poor_wl_display_done_fn *done);

/* Use the connected socket 'fd', which the display owns */
void
poor_wl_display_connect_to_fd(struct poor_wl_display *display, struct poor_loop *loop,
			 const struct poor_wl_connection_memory *memory, int fd,
			 poor_wl_display_error_fn *error, poor_wl_display_done_fn *done);

/* Close the connection. Proxies are left as they are and 'done' is called later from the loop. */
void
poor_wl_display_disconnect(struct poor_wl_display *display);

void
poor_wl_display_set_new_proxy(struct poor_wl_display *display,
			 poor_wl_display_new_proxy_fn *new_proxy);

int
poor_wl_display_get_fd(struct poor_wl_display *display);

/* Start sending the queued requests. Returns 0, or -1 with errno set to the display error. */
int
poor_wl_display_flush(struct poor_wl_display *display);

int
poor_wl_display_get_error(struct poor_wl_display *display);

uint32_t
poor_wl_display_get_protocol_error(struct poor_wl_display *display,
			      const struct poor_wl_interface **interface,
			      uint32_t *id);

void
poor_wl_log_set_handler_client(poor_wl_log_func_t handler);

#ifdef  __cplusplus
}
#endif

#endif
