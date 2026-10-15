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

#ifndef POOR_WAYLAND_SERVER_CORE_H
#define POOR_WAYLAND_SERVER_CORE_H

#include <linux/stat.h>
#include <poor_loop.h>
#include <poor_loop_group.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/un.h>

#include "poor_wayland_connection.h"
#include "poor_wayland_util.h"
#include "poor_wayland_version.h"

#ifdef  __cplusplus
extern "C" {
#endif

/*
 * libpoor_wayland_server runs on a poor_loop and on memory that the caller owns.
 *
 * The display, clients, sockets, globals and resources are structures of the caller. Objects that the library
 * creates on behalf of clients, registries and the objects of poor_wl_shm, come from the allocator of the display.
 *
 * All I/O goes through the loop, so destroying a display, a client or a socket only starts closing it: its 'done'
 * function is called from the loop once nothing uses its memory anymore. A client and the display can't be used
 * from other threads.
 *
 * Every interface has a structure that holds its resource, such as struct poor_wl_surface_resource. It is what
 * poor_wl_surface_resource_init() initializes and what the request handlers and the event functions of the
 * interface take.
 *
 * Events are sent when the loop iteration that queued them ends, or right away with poor_wl_client_flush().
 */

struct poor_wl_client;
struct poor_wl_display;
struct poor_wl_global;
struct poor_wl_resource;
struct poor_wl_socket;
struct poor_wl_listener;
struct poor_wl_buffer_resource;
struct poor_wl_fixes_resource;
struct poor_wl_registry_resource;

typedef void (*poor_wl_notify_func_t)(struct poor_wl_listener *listener, void *data);


/** \class poor_wl_listener
 *
 * \brief A single listener for Wayland signals
 *
 * poor_wl_listener provides the means to listen for poor_wl_signal notifications. Many
 * Wayland objects use poor_wl_listener for notification of significant events like
 * object destruction.
 *
 * Clients should create poor_wl_listener objects manually and can register them as
 * listeners to signals using poor_wl_signal_add(), assuming the signal is
 * directly accessible. For opaque structs like poor_wl_event_loop, adding a
 * listener should be done through provided accessor methods. A listener can
 * only listen to one signal at a time.
 *
 * \code
 * struct poor_wl_listener your_listener;
 *
 * your_listener.notify = your_callback_method;
 *
 * // Direct access
 * poor_wl_signal_add(&some_object->destroy_signal, &your_listener);
 *
 * // Accessor access
 * poor_wl_event_loop *loop = ...;
 * poor_wl_event_loop_add_destroy_listener(loop, &your_listener);
 * \endcode
 *
 * If the listener is part of a larger struct, #poor_wl_container_of can be used
 * to retrieve a pointer to it:
 *
 * \code
 * void your_listener(struct poor_wl_listener *listener, void *data)
 * {
 * 	struct your_data *data;
 *
 * 	your_data = poor_wl_container_of(listener, data, your_member_name);
 * }
 * \endcode
 *
 * If you need to remove a listener from a signal, use poor_wl_list_remove().
 *
 * \code
 * poor_wl_list_remove(&your_listener.link);
 * \endcode
 *
 * \sa poor_wl_signal
 */
struct poor_wl_listener {
	/** Part of poor_wl_signal::listener_list */
	struct poor_wl_list link;

	/** Callback function pointer */
	poor_wl_notify_func_t notify;
};

/** \class poor_wl_signal
 *
 * \brief A source of a type of observable event
 *
 * Signals are recognized points where significant events can be observed.
 * Compositors as well as the server can provide signals. Observers are
 * poor_wl_listener's that are added through #poor_wl_signal_add. Signals are emitted
 * using #poor_wl_signal_emit, which will invoke all listeners until that
 * listener is removed by poor_wl_list_remove() (or whenever the signal is
 * destroyed).
 *
 * \sa poor_wl_listener for more information on using poor_wl_signal
 */
struct poor_wl_signal {
	struct poor_wl_list listener_list;
};

/** Initialize a new \ref poor_wl_signal for use.
 *
 * \param signal The signal that will be initialized
 *
 * \memberof poor_wl_signal
 */
static inline void
poor_wl_signal_init(struct poor_wl_signal *signal)
{
	poor_wl_list_init(&signal->listener_list);
}

/** Add the specified listener to this signal.
 *
 * \param signal The signal that will emit events to the listener
 * \param listener The listener to add
 *
 * \memberof poor_wl_signal
 */
static inline void
poor_wl_signal_add(struct poor_wl_signal *signal, struct poor_wl_listener *listener)
{
	poor_wl_list_insert(signal->listener_list.prev, &listener->link);
}

/** Gets the listener struct for the specified callback.
 *
 * \param signal The signal that contains the specified listener
 * \param notify The listener that is the target of this search
 * \return the list item that corresponds to the specified listener, or NULL
 * if none was found
 *
 * \memberof poor_wl_signal
 */
static inline struct poor_wl_listener *
poor_wl_signal_get(struct poor_wl_signal *signal, poor_wl_notify_func_t notify)
{
	struct poor_wl_listener *l;

	poor_wl_list_for_each(l, &signal->listener_list, link)
		if (l->notify == notify)
			return l;

	return NULL;
}

/** Emits this signal, notifying all registered listeners.
 *
 * \param signal The signal object that will emit the signal
 * \param data The data that will be emitted with the signal
 *
 * \memberof poor_wl_signal
 */
static inline void
poor_wl_signal_emit(struct poor_wl_signal *signal, void *data)
{
	struct poor_wl_listener *l, *next;

	poor_wl_list_for_each_safe(l, next, &signal->listener_list, link)
		l->notify(l, data);
}

void
poor_wl_signal_emit_mutable(struct poor_wl_signal *signal, void *data);

struct poor_wl_priv_signal {
	struct poor_wl_list listener_list;
	struct poor_wl_list emit_list;
};

typedef void (*poor_wl_resource_destroy_func_t)(struct poor_wl_resource *resource);

typedef void (*poor_wl_global_bind_func_t)(struct poor_wl_client *client, void *data,
				      uint32_t version, uint32_t id);

typedef void (*poor_wl_global_withdrawn_func_t)(struct poor_wl_global *global);

/** A filter function for poor_wl_global objects
 *
 * \param client The client object
 * \param global The global object to show or hide
 * \param data   The user data pointer
 *
 * A filter function enables the server to decide which globals to
 * advertise to each client.
 *
 * When a poor_wl_global filter is set, the given callback function will be
 * called during poor_wl_global advertisement and binding.
 *
 * This function should return true if the global object should be made
 * visible to the client or false otherwise.
 */
typedef bool (*poor_wl_display_global_filter_func_t)(const struct poor_wl_client *client,
						const struct poor_wl_global *global,
						void *data);

typedef void (*poor_wl_user_data_destroy_func_t)(void *data);

typedef void poor_wl_display_done_fn(struct poor_wl_display *display);
typedef void *poor_wl_display_alloc_fn(struct poor_wl_display *display, struct poor_wl_client *client, size_t size);
typedef void poor_wl_display_free_fn(struct poor_wl_display *display, void *ptr);
typedef void poor_wl_client_done_fn(struct poor_wl_client *client);
typedef void poor_wl_socket_ready_fn(struct poor_wl_socket *socket);
typedef void poor_wl_socket_accept_fn(struct poor_wl_socket *socket, int fd);
typedef void poor_wl_socket_done_fn(struct poor_wl_socket *socket);

enum poor_wl_protocol_logger_type {
	POOR_WL_PROTOCOL_LOGGER_REQUEST,
	POOR_WL_PROTOCOL_LOGGER_EVENT,
};

struct poor_wl_protocol_logger_message {
	struct poor_wl_resource *resource;
	int message_opcode;
	const struct poor_wl_message *message;
	int arguments_count;
	const union poor_wl_argument *arguments;
};

/** Callback function type for poor_wl_display_add_protocol_logger() */
typedef void (*poor_wl_protocol_logger_func_t)(void *user_data,
					  enum poor_wl_protocol_logger_type direction,
					  const struct poor_wl_protocol_logger_message *message);

/** Callback function type for poor_wl_client_for_each_resource() */
typedef enum poor_wl_iterator_result (*poor_wl_client_for_each_resource_iterator_func_t)(
						struct poor_wl_resource *resource,
						void *user_data);

poor_loop_group_declare(poor_wl_client_io);
poor_loop_group_declare(poor_wl_socket_io);

struct poor_wl_resource {
	struct poor_wl_object object;
	poor_wl_resource_destroy_func_t destroy;
	struct poor_wl_list link;
	struct poor_wl_client *client;
	void *data;
	int version;
	poor_wl_dispatcher_func_t dispatcher;
	struct poor_wl_priv_signal destroy_signal;
};

/* The library implements the wl_display of every client */
struct poor_wl_display_resource {
	struct poor_wl_resource resource;
};

struct poor_wl_client {
	poor_wl_client_io io;
	struct poor_wl_connection connection;
	struct poor_loop_op recv_op;
	struct poor_loop_op send_op;
	struct poor_loop_op cred_op;
	struct poor_loop_timer timer;
	struct poor_wl_display *display;
	struct poor_wl_resource *display_resource;
	struct poor_wl_display_resource display_object;
	struct poor_wl_list link;
	struct poor_wl_map objects;
	struct poor_wl_priv_signal destroy_signal;
	struct poor_wl_priv_signal destroy_late_signal;
	struct poor_wl_priv_signal resource_created_signal;
	struct {
		pid_t pid;
		uid_t uid;
		gid_t gid;
	} cred;
	poor_wl_client_done_fn *done;
	void *data;
	poor_wl_user_data_destroy_func_t data_dtor;
	int fd;
	bool ready;
	bool error;
	bool destroyed;
};

/*
 * A global with 'ack_slot' waits for the registries that announced it to acknowledge its removal. They are the
 * registries that saw it before 'removed_generation'.
 */
struct poor_wl_global {
	struct poor_wl_display *display;
	const struct poor_wl_interface *interface;
	struct poor_wl_list link;
	uint32_t name;
	uint32_t version;
	uint32_t removed_generation;
	uint32_t pending_acks;
	int ack_slot;
	void *data;
	poor_wl_global_bind_func_t bind;
	poor_wl_global_withdrawn_func_t withdrawn;
	bool removed;
};

struct poor_wl_socket {
	poor_wl_socket_io io;
	struct poor_wl_display *display;
	struct poor_wl_list link;
	struct poor_loop_op lock_op;
	struct poor_loop_op stat_op;
	struct poor_loop_op unlink_op;
	struct poor_loop_op socket_op;
	struct poor_loop_op bind_op;
	struct poor_loop_op listen_op;
	struct poor_loop_op accept_op;
	struct poor_loop_op cleanup_op;
	struct poor_loop_timer timer;
	poor_wl_socket_ready_fn *ready;
	poor_wl_socket_accept_fn *accept;
	poor_wl_socket_done_fn *done;
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
	char lock_addr[ARRAY_SIZE(((struct sockaddr_un *)nullptr)->sun_path) + 5];
};

struct poor_wl_protocol_logger {
	struct poor_wl_list link;
	poor_wl_protocol_logger_func_t func;
	void *user_data;
};

/*
 * Initialize a display that runs on 'loop'. 'done' is called once poor_wl_display_destroy() has finished, after the
 * done functions of all its clients and sockets.
 */
void
poor_wl_display_init(struct poor_wl_display *display, struct poor_loop *loop,
		poor_wl_display_done_fn *done);

/*
 * Set the allocator for the objects that the library creates for clients:
 * registries and the objects of poor_wl_shm. Without it clients get no_memory
 * errors when they ask for them.
 */
void
poor_wl_display_set_allocator(struct poor_wl_display *display,
			 poor_wl_display_alloc_fn *alloc, poor_wl_display_free_fn *free);

/* Destroy all sockets and clients, then call 'done' */
void
poor_wl_display_destroy(struct poor_wl_display *display);

struct poor_loop *
poor_wl_display_get_loop(struct poor_wl_display *display);

void
poor_wl_display_flush_clients(struct poor_wl_display *display);

void
poor_wl_display_destroy_clients(struct poor_wl_display *display);

uint32_t
poor_wl_display_get_serial(const struct poor_wl_display *display);

uint32_t
poor_wl_display_next_serial(struct poor_wl_display *display);

void
poor_wl_display_add_destroy_listener(struct poor_wl_display *display,
				struct poor_wl_listener *listener);

/* The listeners are called once the client credentials are known */
void
poor_wl_display_add_client_created_listener(struct poor_wl_display *display,
					struct poor_wl_listener *listener);

struct poor_wl_listener *
poor_wl_display_get_destroy_listener(struct poor_wl_display *display,
				poor_wl_notify_func_t notify);

void
poor_wl_display_set_global_filter(struct poor_wl_display *display,
			     poor_wl_display_global_filter_func_t filter,
			     void *data);

struct poor_wl_list *
poor_wl_display_get_client_list(struct poor_wl_display *display);

/*
 * Prepare a listening socket. 'ready' is called once it listens, 'accept' with
 * every connection, whose fd the callee owns, and 'done' once it is closed.
 */
void
poor_wl_socket_init(struct poor_wl_socket *socket, struct poor_wl_display *display,
	       poor_wl_socket_ready_fn *ready, poor_wl_socket_accept_fn *accept,
	       poor_wl_socket_done_fn *done);

/*
 * Listen on the socket 'name' in XDG_RUNTIME_DIR, or on the path 'name' if it
 * is absolute. Without 'name', WAYLAND_DISPLAY or "wayland-0" is used.
 *
 * Returns 0, or -1 with errno set if the path is invalid, then 'done' isn't
 * called. If listening fails later, 'done' is called with the socket's 'error'
 * set to an errno value.
 */
int
poor_wl_socket_listen(struct poor_wl_socket *socket, const char *name);

/* Like poor_wl_socket_listen() with the first free name of "wayland-0" to "wayland-32" */
int
poor_wl_socket_listen_auto(struct poor_wl_socket *socket);

/* Accept on a socket that already listens, without calling 'ready'. The socket owns 'fd'. */
void
poor_wl_socket_listen_fd(struct poor_wl_socket *socket, int fd);

/* Stop accepting and remove the socket and its lock file */
void
poor_wl_socket_destroy(struct poor_wl_socket *socket);

const char *
poor_wl_socket_get_name(const struct poor_wl_socket *socket);

/*
 * Make the global visible to clients. The version is between 1 and the version
 * of the interface. The global must stay valid until poor_wl_global_destroy().
 */
void
poor_wl_global_init(struct poor_wl_global *global, struct poor_wl_display *display,
	       const struct poor_wl_interface *interface, int version,
	       void *data, poor_wl_global_bind_func_t bind);

void
poor_wl_global_remove(struct poor_wl_global *global);

void
poor_wl_global_set_withdrawn_listener(struct poor_wl_global *global,
				 poor_wl_global_withdrawn_func_t func);

void
poor_wl_global_destroy(struct poor_wl_global *global);

const struct poor_wl_interface *
poor_wl_global_get_interface(const struct poor_wl_global *global);

uint32_t
poor_wl_global_get_name(const struct poor_wl_global *global,
                   const struct poor_wl_client *client);

uint32_t
poor_wl_global_get_version(const struct poor_wl_global *global);

struct poor_wl_display *
poor_wl_global_get_display(const struct poor_wl_global *global);

void *
poor_wl_global_get_user_data(const struct poor_wl_global *global);

void
poor_wl_global_set_user_data(struct poor_wl_global *global, void *data);

/*
 * Serve a client on the connected socket 'fd', which the client owns. The
 * client uses 'memory' and the client structure until 'done' is called.
 */
void
poor_wl_client_init(struct poor_wl_client *client, struct poor_wl_display *display, int fd,
	       const struct poor_wl_connection_memory *memory,
	       poor_wl_client_done_fn *done);

struct poor_wl_list *
poor_wl_client_get_link(struct poor_wl_client *client);

struct poor_wl_client *
poor_wl_client_from_link(struct poor_wl_list *link);

/** Iterate over a list of clients. */
#define poor_wl_client_for_each(client, list)				\
	for (client = poor_wl_client_from_link((list)->next);	\
	     poor_wl_client_get_link(client) != (list);			\
	     client = poor_wl_client_from_link(poor_wl_client_get_link(client)->next))

/*
 * Destroy the client's resources and start closing it. Queued events, like a
 * protocol error, are sent if the socket takes them right away.
 */
void
poor_wl_client_destroy(struct poor_wl_client *client);

void
poor_wl_client_flush(struct poor_wl_client *client);

struct poor_wl_display *
poor_wl_client_get_display(struct poor_wl_client *client);

void
poor_wl_client_get_credentials(const struct poor_wl_client *client,
			  pid_t *pid, uid_t *uid, gid_t *gid);

int
poor_wl_client_get_fd(struct poor_wl_client *client);

void
poor_wl_client_add_destroy_listener(struct poor_wl_client *client,
			       struct poor_wl_listener *listener);

struct poor_wl_listener *
poor_wl_client_get_destroy_listener(struct poor_wl_client *client,
			       poor_wl_notify_func_t notify);

void
poor_wl_client_add_destroy_late_listener(struct poor_wl_client *client,
				    struct poor_wl_listener *listener);

struct poor_wl_listener *
poor_wl_client_get_destroy_late_listener(struct poor_wl_client *client,
				    poor_wl_notify_func_t notify);

struct poor_wl_resource *
poor_wl_client_get_object(struct poor_wl_client *client, uint32_t id);

void
poor_wl_client_post_no_memory(struct poor_wl_client *client);

void
poor_wl_client_post_implementation_error(struct poor_wl_client *client,
                                    const char* msg, ...) POOR_WL_PRINTF(2,3);

void
poor_wl_client_add_resource_created_listener(struct poor_wl_client *client,
                                        struct poor_wl_listener *listener);

void
poor_wl_client_for_each_resource(struct poor_wl_client *client,
                            poor_wl_client_for_each_resource_iterator_func_t iterator,
                            void *user_data);

void
poor_wl_client_set_user_data(struct poor_wl_client *client,
			void *data,
			poor_wl_user_data_destroy_func_t dtor);

void *
poor_wl_client_get_user_data(struct poor_wl_client *client);

/*
 * Post an event to the client's object referred to by 'resource'.
 * 'opcode' is the event number generated from the protocol XML
 * description (the event name). The variable arguments are the event
 * parameters, in the order they appear in the protocol XML specification.
 *
 * The variable arguments' types are:
 * - type=uint:	uint32_t
 * - type=int:		int32_t
 * - type=fixed:	poor_wl_fixed_t
 * - type=string:	(const char *) to a nil-terminated string
 * - type=array:	(struct poor_wl_array *)
 * - type=fd:		int, an open file descriptor that the connection takes and closes once it is sent
 * - type=new_id:	(struct poor_wl_object *) or (struct poor_wl_resource *)
 * - type=object:	(struct poor_wl_object *) or (struct poor_wl_resource *)
 */
void
poor_wl_resource_post_event(struct poor_wl_resource *resource,
		       uint32_t opcode, ...);

void
poor_wl_resource_post_event_array(struct poor_wl_resource *resource,
			     uint32_t opcode, union poor_wl_argument *args);

void
poor_wl_resource_queue_event(struct poor_wl_resource *resource,
			uint32_t opcode, ...);

void
poor_wl_resource_queue_event_array(struct poor_wl_resource *resource,
			      uint32_t opcode, union poor_wl_argument *args);

void
poor_wl_resource_post_error_vargs(struct poor_wl_resource *resource,
			     uint32_t code, const char *msg, va_list argp);

void
poor_wl_resource_post_error(struct poor_wl_resource *resource,
		       uint32_t code, const char *msg, ...) POOR_WL_PRINTF(3, 4);

void
poor_wl_resource_post_no_memory(struct poor_wl_resource *resource);

/*
 * Initialize a resource with the new id 'id', or with an id of the server if
 * 'id' is 0. Returns 0, or -1 when the client can't have it.
 *
 * The resource stays in use until its destructor is called, which is the last
 * time the library touches it.
 */
int
poor_wl_resource_init(struct poor_wl_resource *resource, struct poor_wl_client *client,
		 const struct poor_wl_interface *interface,
		 int version, uint32_t id);

void
poor_wl_resource_set_implementation(struct poor_wl_resource *resource,
			       const void *implementation,
			       void *data,
			       poor_wl_resource_destroy_func_t destroy);

void
poor_wl_resource_set_dispatcher(struct poor_wl_resource *resource,
			   poor_wl_dispatcher_func_t dispatcher,
			   const void *implementation,
			   void *data,
			   poor_wl_resource_destroy_func_t destroy);

void
poor_wl_resource_destroy(struct poor_wl_resource *resource);

uint32_t
poor_wl_resource_get_id(const struct poor_wl_resource *resource);

struct poor_wl_list *
poor_wl_resource_get_link(struct poor_wl_resource *resource);

struct poor_wl_resource *
poor_wl_resource_from_link(struct poor_wl_list *resource);

struct poor_wl_resource *
poor_wl_resource_find_for_client(struct poor_wl_list *list, struct poor_wl_client *client);

struct poor_wl_client *
poor_wl_resource_get_client(struct poor_wl_resource *resource);

void
poor_wl_resource_set_user_data(struct poor_wl_resource *resource, void *data);

void *
poor_wl_resource_get_user_data(struct poor_wl_resource *resource);

int
poor_wl_resource_get_version(const struct poor_wl_resource *resource);

void
poor_wl_resource_set_destructor(struct poor_wl_resource *resource,
			   poor_wl_resource_destroy_func_t destroy);

int
poor_wl_resource_instance_of(struct poor_wl_resource *resource,
			const struct poor_wl_interface *interface,
			const void *implementation);

const char *
poor_wl_resource_get_class(const struct poor_wl_resource *resource);

const struct poor_wl_interface *
poor_wl_resource_get_interface(struct poor_wl_resource *resource);

void
poor_wl_resource_add_destroy_listener(struct poor_wl_resource *resource,
				 struct poor_wl_listener *listener);

struct poor_wl_listener *
poor_wl_resource_get_destroy_listener(struct poor_wl_resource *resource,
				 poor_wl_notify_func_t notify);

#define poor_wl_resource_for_each(resource, list)					\
	for (resource = 0, resource = poor_wl_resource_from_link((list)->next);	\
	     poor_wl_resource_get_link(resource) != (list);				\
	     resource = poor_wl_resource_from_link(poor_wl_resource_get_link(resource)->next))

#define poor_wl_resource_for_each_safe(resource, tmp, list)					\
	for (resource = 0, tmp = 0,							\
	     resource = poor_wl_resource_from_link((list)->next),	\
	     tmp = poor_wl_resource_from_link((list)->next->next);	\
	     poor_wl_resource_get_link(resource) != (list);				\
	     resource = tmp,							\
	     tmp = poor_wl_resource_from_link(poor_wl_resource_get_link(resource)->next))

struct poor_wl_shm_buffer *
poor_wl_shm_buffer_get(struct poor_wl_buffer_resource *resource);

void
poor_wl_shm_buffer_begin_access(struct poor_wl_shm_buffer *buffer);

void
poor_wl_shm_buffer_end_access(struct poor_wl_shm_buffer *buffer);

void *
poor_wl_shm_buffer_get_data(struct poor_wl_shm_buffer *buffer);

int32_t
poor_wl_shm_buffer_get_stride(const struct poor_wl_shm_buffer *buffer);

uint32_t
poor_wl_shm_buffer_get_format(const struct poor_wl_shm_buffer *buffer);

int32_t
poor_wl_shm_buffer_get_width(const struct poor_wl_shm_buffer *buffer);

int32_t
poor_wl_shm_buffer_get_height(const struct poor_wl_shm_buffer *buffer);

struct poor_wl_shm_buffer *
poor_wl_shm_buffer_ref(struct poor_wl_shm_buffer *buffer);

void
poor_wl_shm_buffer_unref(struct poor_wl_shm_buffer *buffer);

struct poor_wl_shm_mapping *
poor_wl_shm_buffer_ref_mapping(struct poor_wl_shm_buffer *buffer);

void
poor_wl_shm_mapping_unref(struct poor_wl_shm_mapping *mapping);

/* Announce poor_wl_shm with 'global'. Pools and buffers come from the allocator of the display. */
void
poor_wl_display_init_shm(struct poor_wl_display *display, struct poor_wl_global *global);

/* Advertise 'formats', an array or a pointer to one, besides ARGB8888 and XRGB8888. The caller keeps them valid. */
void
poor_wl_display_set_shm_formats_impl(struct poor_wl_display *display,
				size_t count, const uint32_t (*formats)[count]);
#define poor_wl_display_set_shm_formats(display, formats) \
	poor_wl_display_set_shm_formats_impl(display, ARRAY_SIZE(formats), &auto_arr(formats))

void
poor_wl_log_set_handler_server(poor_wl_log_func_t handler);

void
poor_wl_display_add_protocol_logger(struct poor_wl_display *display,
			       struct poor_wl_protocol_logger *logger,
			       poor_wl_protocol_logger_func_t func, void *user_data);

void
poor_wl_protocol_logger_destroy(struct poor_wl_protocol_logger *logger);

void
poor_wl_fixes_handle_ack_global_remove(struct poor_wl_fixes_resource *fixes_resource,
				  struct poor_wl_registry_resource *registry_resource,
				  uint32_t global_name);

#ifdef  __cplusplus
}
#endif

#endif
