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

#ifndef WAYLAND_SERVER_CORE_H
#define WAYLAND_SERVER_CORE_H

#include <linux/stat.h>
#include <poor_loop.h>
#include <poor_loop_group.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/un.h>

#include "wayland-connection.h"
#include "wayland-util.h"
#include "wayland-version.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * libwayland-server runs on a poor_loop and on memory that the caller owns.
 *
 * The display, clients, sockets, globals and resources are structures of the caller. Objects that the library
 * creates on behalf of clients, registries and the objects of wl_shm, come from the allocator of the display.
 *
 * All I/O goes through the loop, so destroying a display, a client or a socket only starts closing it: its 'done'
 * function is called from the loop once nothing uses its memory anymore. A client and the display can't be used
 * from other threads.
 *
 * Events are sent when the loop iteration that queued them ends, or right away with wl_client_flush().
 */

struct wl_client;
struct wl_display;
struct wl_global;
struct wl_resource;
struct wl_socket;
struct wl_listener;

typedef void (*wl_notify_func_t)(struct wl_listener *listener, void *data);


/** \class wl_listener
 *
 * \brief A single listener for Wayland signals
 *
 * wl_listener provides the means to listen for wl_signal notifications. Many
 * Wayland objects use wl_listener for notification of significant events like
 * object destruction.
 *
 * Clients should create wl_listener objects manually and can register them as
 * listeners to signals using wl_signal_add(), assuming the signal is
 * directly accessible. For opaque structs like wl_event_loop, adding a
 * listener should be done through provided accessor methods. A listener can
 * only listen to one signal at a time.
 *
 * \code
 * struct wl_listener your_listener;
 *
 * your_listener.notify = your_callback_method;
 *
 * // Direct access
 * wl_signal_add(&some_object->destroy_signal, &your_listener);
 *
 * // Accessor access
 * wl_event_loop *loop = ...;
 * wl_event_loop_add_destroy_listener(loop, &your_listener);
 * \endcode
 *
 * If the listener is part of a larger struct, #wl_container_of can be used
 * to retrieve a pointer to it:
 *
 * \code
 * void your_listener(struct wl_listener *listener, void *data)
 * {
 * 	struct your_data *data;
 *
 * 	your_data = wl_container_of(listener, data, your_member_name);
 * }
 * \endcode
 *
 * If you need to remove a listener from a signal, use wl_list_remove().
 *
 * \code
 * wl_list_remove(&your_listener.link);
 * \endcode
 *
 * \sa wl_signal
 */
struct wl_listener {
	/** Part of wl_signal::listener_list */
	struct wl_list link;

	/** Callback function pointer */
	wl_notify_func_t notify;
};

/** \class wl_signal
 *
 * \brief A source of a type of observable event
 *
 * Signals are recognized points where significant events can be observed.
 * Compositors as well as the server can provide signals. Observers are
 * wl_listener's that are added through #wl_signal_add. Signals are emitted
 * using #wl_signal_emit, which will invoke all listeners until that
 * listener is removed by wl_list_remove() (or whenever the signal is
 * destroyed).
 *
 * \sa wl_listener for more information on using wl_signal
 */
struct wl_signal {
	struct wl_list listener_list;
};

/** Initialize a new \ref wl_signal for use.
 *
 * \param signal The signal that will be initialized
 *
 * \memberof wl_signal
 */
static inline void
wl_signal_init(struct wl_signal *signal)
{
	wl_list_init(&signal->listener_list);
}

/** Add the specified listener to this signal.
 *
 * \param signal The signal that will emit events to the listener
 * \param listener The listener to add
 *
 * \memberof wl_signal
 */
static inline void
wl_signal_add(struct wl_signal *signal, struct wl_listener *listener)
{
	wl_list_insert(signal->listener_list.prev, &listener->link);
}

/** Gets the listener struct for the specified callback.
 *
 * \param signal The signal that contains the specified listener
 * \param notify The listener that is the target of this search
 * \return the list item that corresponds to the specified listener, or NULL
 * if none was found
 *
 * \memberof wl_signal
 */
static inline struct wl_listener *
wl_signal_get(struct wl_signal *signal, wl_notify_func_t notify)
{
	struct wl_listener *l;

	wl_list_for_each(l, &signal->listener_list, link)
		if (l->notify == notify)
			return l;

	return NULL;
}

/** Emits this signal, notifying all registered listeners.
 *
 * \param signal The signal object that will emit the signal
 * \param data The data that will be emitted with the signal
 *
 * \memberof wl_signal
 */
static inline void
wl_signal_emit(struct wl_signal *signal, void *data)
{
	struct wl_listener *l, *next;

	wl_list_for_each_safe(l, next, &signal->listener_list, link)
		l->notify(l, data);
}

void
wl_signal_emit_mutable(struct wl_signal *signal, void *data);

struct wl_priv_signal {
	struct wl_list listener_list;
	struct wl_list emit_list;
};

typedef void (*wl_resource_destroy_func_t)(struct wl_resource *resource);

typedef void (*wl_global_bind_func_t)(struct wl_client *client, void *data,
				      uint32_t version, uint32_t id);

typedef void (*wl_global_withdrawn_func_t)(struct wl_global *global);

/** A filter function for wl_global objects
 *
 * \param client The client object
 * \param global The global object to show or hide
 * \param data   The user data pointer
 *
 * A filter function enables the server to decide which globals to
 * advertise to each client.
 *
 * When a wl_global filter is set, the given callback function will be
 * called during wl_global advertisement and binding.
 *
 * This function should return true if the global object should be made
 * visible to the client or false otherwise.
 */
typedef bool (*wl_display_global_filter_func_t)(const struct wl_client *client,
						const struct wl_global *global,
						void *data);

typedef void (*wl_user_data_destroy_func_t)(void *data);

typedef void wl_display_done_fn(struct wl_display *display);
typedef void *wl_display_alloc_fn(struct wl_display *display, struct wl_client *client, size_t size);
typedef void wl_display_free_fn(struct wl_display *display, void *ptr);
typedef void wl_client_done_fn(struct wl_client *client);
typedef void wl_socket_ready_fn(struct wl_socket *socket);
typedef void wl_socket_accept_fn(struct wl_socket *socket, int fd);
typedef void wl_socket_done_fn(struct wl_socket *socket);

enum wl_protocol_logger_type {
	WL_PROTOCOL_LOGGER_REQUEST,
	WL_PROTOCOL_LOGGER_EVENT,
};

struct wl_protocol_logger_message {
	struct wl_resource *resource;
	int message_opcode;
	const struct wl_message *message;
	int arguments_count;
	const union wl_argument *arguments;
};

/** Callback function type for wl_display_add_protocol_logger() */
typedef void (*wl_protocol_logger_func_t)(void *user_data,
					  enum wl_protocol_logger_type direction,
					  const struct wl_protocol_logger_message *message);

/** Callback function type for wl_client_for_each_resource() */
typedef enum wl_iterator_result (*wl_client_for_each_resource_iterator_func_t)(
						struct wl_resource *resource,
						void *user_data);

poor_loop_group_declare(wl_client_io);
poor_loop_group_declare(wl_socket_io);

struct wl_resource {
	struct wl_object object;
	wl_resource_destroy_func_t destroy;
	struct wl_list link;
	struct wl_client *client;
	void *data;
	int version;
	wl_dispatcher_func_t dispatcher;
	struct wl_priv_signal destroy_signal;
};

struct wl_client {
	wl_client_io io;
	struct wl_connection connection;
	struct poor_loop_op recv_op;
	struct poor_loop_op send_op;
	struct poor_loop_op cred_op;
	struct poor_loop_timer timer;
	struct wl_display *display;
	struct wl_resource *display_resource;
	struct wl_resource display_object;
	struct wl_list link;
	struct wl_map objects;
	struct wl_priv_signal destroy_signal;
	struct wl_priv_signal destroy_late_signal;
	struct wl_priv_signal resource_created_signal;
	struct {
		pid_t pid;
		uid_t uid;
		gid_t gid;
	} cred;
	wl_client_done_fn *done;
	void *data;
	wl_user_data_destroy_func_t data_dtor;
	int fd;
	bool ready;
	bool error;
	bool destroyed;
};

/*
 * A global with 'ack_slot' waits for the registries that announced it to acknowledge its removal. They are the
 * registries that saw it before 'removed_generation'.
 */
struct wl_global {
	struct wl_display *display;
	const struct wl_interface *interface;
	struct wl_list link;
	uint32_t name;
	uint32_t version;
	uint32_t removed_generation;
	uint32_t pending_acks;
	int ack_slot;
	void *data;
	wl_global_bind_func_t bind;
	wl_global_withdrawn_func_t withdrawn;
	bool removed;
};

struct wl_socket {
	wl_socket_io io;
	struct wl_display *display;
	struct wl_list link;
	struct poor_loop_op lock_op;
	struct poor_loop_op stat_op;
	struct poor_loop_op unlink_op;
	struct poor_loop_op socket_op;
	struct poor_loop_op bind_op;
	struct poor_loop_op listen_op;
	struct poor_loop_op accept_op;
	struct poor_loop_op cleanup_op;
	struct poor_loop_timer timer;
	wl_socket_ready_fn *ready;
	wl_socket_accept_fn *accept;
	wl_socket_done_fn *done;
	struct statx stx;
	struct sockaddr_un addr;
	const char *display_name;
	int fd;
	int fd_lock;
	int displayno;
	int error;
	int bind_error;
	int listen_error;
	bool auto_name;
	bool closing;
	bool cleaned;
	char lock_addr[sizeof(((struct sockaddr_un *)nullptr)->sun_path) + 5];
};

struct wl_protocol_logger {
	struct wl_list link;
	wl_protocol_logger_func_t func;
	void *user_data;
};

/*
 * Initialize a display that runs on 'loop'. 'done' is called once wl_display_destroy() has finished, after the
 * done functions of all its clients and sockets.
 */
void
wl_display_init(struct wl_display *display, struct poor_loop *loop,
		wl_display_done_fn *done);

/*
 * Set the allocator for the objects that the library creates for clients:
 * registries and the objects of wl_shm. Without it clients get no_memory
 * errors when they ask for them.
 */
void
wl_display_set_allocator(struct wl_display *display,
			 wl_display_alloc_fn *alloc, wl_display_free_fn *free);

/* Destroy all sockets and clients, then call 'done' */
void
wl_display_destroy(struct wl_display *display);

struct poor_loop *
wl_display_get_loop(struct wl_display *display);

void
wl_display_flush_clients(struct wl_display *display);

void
wl_display_destroy_clients(struct wl_display *display);

uint32_t
wl_display_get_serial(const struct wl_display *display);

uint32_t
wl_display_next_serial(struct wl_display *display);

void
wl_display_add_destroy_listener(struct wl_display *display,
				struct wl_listener *listener);

/* The listeners are called once the client credentials are known */
void
wl_display_add_client_created_listener(struct wl_display *display,
					struct wl_listener *listener);

struct wl_listener *
wl_display_get_destroy_listener(struct wl_display *display,
				wl_notify_func_t notify);

void
wl_display_set_global_filter(struct wl_display *display,
			     wl_display_global_filter_func_t filter,
			     void *data);

struct wl_list *
wl_display_get_client_list(struct wl_display *display);

/*
 * Prepare a listening socket. 'ready' is called once it listens, 'accept' with
 * every connection, whose fd the callee owns, and 'done' once it is closed.
 */
void
wl_socket_init(struct wl_socket *socket, struct wl_display *display,
	       wl_socket_ready_fn *ready, wl_socket_accept_fn *accept,
	       wl_socket_done_fn *done);

/*
 * Listen on the socket 'name' in XDG_RUNTIME_DIR, or on the path 'name' if it
 * is absolute. Without 'name', WAYLAND_DISPLAY or "wayland-0" is used.
 *
 * Returns 0, or -1 with errno set if the path is invalid, then 'done' isn't
 * called. If listening fails later, 'done' is called with the socket's 'error'
 * set to an errno value.
 */
int
wl_socket_listen(struct wl_socket *socket, const char *name);

/* Like wl_socket_listen() with the first free name of "wayland-0" to "wayland-32" */
int
wl_socket_listen_auto(struct wl_socket *socket);

/* Accept on a socket that already listens, without calling 'ready'. The socket owns 'fd'. */
void
wl_socket_listen_fd(struct wl_socket *socket, int fd);

/* Stop accepting and remove the socket and its lock file */
void
wl_socket_destroy(struct wl_socket *socket);

const char *
wl_socket_get_name(const struct wl_socket *socket);

/*
 * Make the global visible to clients. The version is between 1 and the version
 * of the interface. The global must stay valid until wl_global_destroy().
 */
void
wl_global_init(struct wl_global *global, struct wl_display *display,
	       const struct wl_interface *interface, int version,
	       void *data, wl_global_bind_func_t bind);

void
wl_global_remove(struct wl_global *global);

void
wl_global_set_withdrawn_listener(struct wl_global *global,
				 wl_global_withdrawn_func_t func);

void
wl_global_destroy(struct wl_global *global);

const struct wl_interface *
wl_global_get_interface(const struct wl_global *global);

uint32_t
wl_global_get_name(const struct wl_global *global,
                   const struct wl_client *client);

uint32_t
wl_global_get_version(const struct wl_global *global);

struct wl_display *
wl_global_get_display(const struct wl_global *global);

void *
wl_global_get_user_data(const struct wl_global *global);

void
wl_global_set_user_data(struct wl_global *global, void *data);

/*
 * Serve a client on the connected socket 'fd', which the client owns. The
 * client uses 'memory' and the client structure until 'done' is called.
 */
void
wl_client_init(struct wl_client *client, struct wl_display *display, int fd,
	       const struct wl_connection_memory *memory,
	       wl_client_done_fn *done);

struct wl_list *
wl_client_get_link(struct wl_client *client);

struct wl_client *
wl_client_from_link(struct wl_list *link);

/** Iterate over a list of clients. */
#define wl_client_for_each(client, list)				\
	for (client = wl_client_from_link((list)->next);	\
	     wl_client_get_link(client) != (list);			\
	     client = wl_client_from_link(wl_client_get_link(client)->next))

/*
 * Destroy the client's resources and start closing it. Queued events, like a
 * protocol error, are sent if the socket takes them right away.
 */
void
wl_client_destroy(struct wl_client *client);

void
wl_client_flush(struct wl_client *client);

struct wl_display *
wl_client_get_display(struct wl_client *client);

void
wl_client_get_credentials(const struct wl_client *client,
			  pid_t *pid, uid_t *uid, gid_t *gid);

int
wl_client_get_fd(struct wl_client *client);

void
wl_client_add_destroy_listener(struct wl_client *client,
			       struct wl_listener *listener);

struct wl_listener *
wl_client_get_destroy_listener(struct wl_client *client,
			       wl_notify_func_t notify);

void
wl_client_add_destroy_late_listener(struct wl_client *client,
				    struct wl_listener *listener);

struct wl_listener *
wl_client_get_destroy_late_listener(struct wl_client *client,
				    wl_notify_func_t notify);

struct wl_resource *
wl_client_get_object(struct wl_client *client, uint32_t id);

void
wl_client_post_no_memory(struct wl_client *client);

void
wl_client_post_implementation_error(struct wl_client *client,
                                    const char* msg, ...) WL_PRINTF(2,3);

void
wl_client_add_resource_created_listener(struct wl_client *client,
                                        struct wl_listener *listener);

void
wl_client_for_each_resource(struct wl_client *client,
                            wl_client_for_each_resource_iterator_func_t iterator,
                            void *user_data);

void
wl_client_set_user_data(struct wl_client *client,
			void *data,
			wl_user_data_destroy_func_t dtor);

void *
wl_client_get_user_data(struct wl_client *client);

/*
 * Post an event to the client's object referred to by 'resource'.
 * 'opcode' is the event number generated from the protocol XML
 * description (the event name). The variable arguments are the event
 * parameters, in the order they appear in the protocol XML specification.
 *
 * The variable arguments' types are:
 * - type=uint:	uint32_t
 * - type=int:		int32_t
 * - type=fixed:	wl_fixed_t
 * - type=string:	(const char *) to a nil-terminated string
 * - type=array:	(struct wl_array *)
 * - type=fd:		int, that is an open file descriptor, the caller keeps it
 * - type=new_id:	(struct wl_object *) or (struct wl_resource *)
 * - type=object:	(struct wl_object *) or (struct wl_resource *)
 */
void
wl_resource_post_event(struct wl_resource *resource,
		       uint32_t opcode, ...);

void
wl_resource_post_event_array(struct wl_resource *resource,
			     uint32_t opcode, union wl_argument *args);

void
wl_resource_queue_event(struct wl_resource *resource,
			uint32_t opcode, ...);

void
wl_resource_queue_event_array(struct wl_resource *resource,
			      uint32_t opcode, union wl_argument *args);

void
wl_resource_post_error_vargs(struct wl_resource *resource,
			     uint32_t code, const char *msg, va_list argp);

void
wl_resource_post_error(struct wl_resource *resource,
		       uint32_t code, const char *msg, ...) WL_PRINTF(3, 4);

void
wl_resource_post_no_memory(struct wl_resource *resource);

/*
 * Initialize a resource with the new id 'id', or with an id of the server if
 * 'id' is 0. Returns 0, or -1 when the client can't have it.
 *
 * The resource stays in use until its destructor is called, which is the last
 * time the library touches it.
 */
int
wl_resource_init(struct wl_resource *resource, struct wl_client *client,
		 const struct wl_interface *interface,
		 int version, uint32_t id);

void
wl_resource_set_implementation(struct wl_resource *resource,
			       const void *implementation,
			       void *data,
			       wl_resource_destroy_func_t destroy);

void
wl_resource_set_dispatcher(struct wl_resource *resource,
			   wl_dispatcher_func_t dispatcher,
			   const void *implementation,
			   void *data,
			   wl_resource_destroy_func_t destroy);

void
wl_resource_destroy(struct wl_resource *resource);

uint32_t
wl_resource_get_id(const struct wl_resource *resource);

struct wl_list *
wl_resource_get_link(struct wl_resource *resource);

struct wl_resource *
wl_resource_from_link(struct wl_list *resource);

struct wl_resource *
wl_resource_find_for_client(struct wl_list *list, struct wl_client *client);

struct wl_client *
wl_resource_get_client(struct wl_resource *resource);

void
wl_resource_set_user_data(struct wl_resource *resource, void *data);

void *
wl_resource_get_user_data(struct wl_resource *resource);

int
wl_resource_get_version(const struct wl_resource *resource);

void
wl_resource_set_destructor(struct wl_resource *resource,
			   wl_resource_destroy_func_t destroy);

int
wl_resource_instance_of(struct wl_resource *resource,
			const struct wl_interface *interface,
			const void *implementation);

const char *
wl_resource_get_class(const struct wl_resource *resource);

const struct wl_interface *
wl_resource_get_interface(struct wl_resource *resource);

void
wl_resource_add_destroy_listener(struct wl_resource *resource,
				 struct wl_listener *listener);

struct wl_listener *
wl_resource_get_destroy_listener(struct wl_resource *resource,
				 wl_notify_func_t notify);

#define wl_resource_for_each(resource, list)					\
	for (resource = 0, resource = wl_resource_from_link((list)->next);	\
	     wl_resource_get_link(resource) != (list);				\
	     resource = wl_resource_from_link(wl_resource_get_link(resource)->next))

#define wl_resource_for_each_safe(resource, tmp, list)					\
	for (resource = 0, tmp = 0,							\
	     resource = wl_resource_from_link((list)->next),	\
	     tmp = wl_resource_from_link((list)->next->next);	\
	     wl_resource_get_link(resource) != (list);				\
	     resource = tmp,							\
	     tmp = wl_resource_from_link(wl_resource_get_link(resource)->next))

struct wl_shm_buffer *
wl_shm_buffer_get(struct wl_resource *resource);

void
wl_shm_buffer_begin_access(struct wl_shm_buffer *buffer);

void
wl_shm_buffer_end_access(struct wl_shm_buffer *buffer);

void *
wl_shm_buffer_get_data(struct wl_shm_buffer *buffer);

int32_t
wl_shm_buffer_get_stride(const struct wl_shm_buffer *buffer);

uint32_t
wl_shm_buffer_get_format(const struct wl_shm_buffer *buffer);

int32_t
wl_shm_buffer_get_width(const struct wl_shm_buffer *buffer);

int32_t
wl_shm_buffer_get_height(const struct wl_shm_buffer *buffer);

struct wl_shm_buffer *
wl_shm_buffer_ref(struct wl_shm_buffer *buffer);

void
wl_shm_buffer_unref(struct wl_shm_buffer *buffer);

struct wl_shm_pool *
wl_shm_buffer_ref_pool(struct wl_shm_buffer *buffer);

void
wl_shm_pool_unref(struct wl_shm_pool *pool);

/* Announce wl_shm with 'global'. Pools and buffers come from the allocator of the display. */
void
wl_display_init_shm(struct wl_display *display, struct wl_global *global);

/* Advertise 'formats' besides ARGB8888 and XRGB8888. The caller keeps them valid. */
void
wl_display_set_shm_formats(struct wl_display *display,
			   const uint32_t *formats, size_t count);

void
wl_log_set_handler_server(wl_log_func_t handler);

void
wl_display_add_protocol_logger(struct wl_display *display,
			       struct wl_protocol_logger *logger,
			       wl_protocol_logger_func_t func, void *user_data);

void
wl_protocol_logger_destroy(struct wl_protocol_logger *logger);

void
wl_fixes_handle_ack_global_remove(struct wl_resource *fixes_resource,
				  struct wl_resource *registry_resource,
				  uint32_t global_name);

#ifdef  __cplusplus
}
#endif

#endif
