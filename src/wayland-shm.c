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
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>

#include "wayland-private.h"
#include "poor_wayland_server_display.h"
#include "poor_wayland_server.h"
#include "wayland-server-private.h"

/*
 * The once is used to install the SIGBUS handler in the first call of poor_wl_shm_buffer_begin_access(), which can happen
 * from any thread
 */
static pthread_once_t poor_wl_shm_sigbus_once = PTHREAD_ONCE_INIT;
static struct sigaction poor_wl_shm_old_sigbus_action;

struct poor_wl_shm_mapping {
	struct poor_wl_shm_pool_resource resource;
	struct poor_wl_display *display;
	int internal_refcount;
	int external_refcount;
	char *data;
	ssize_t size;
	ssize_t new_size;
	bool resource_destroyed;
};

/** \class poor_wl_shm_buffer
 *
 * \brief A SHM buffer
 *
 * poor_wl_shm_buffer provides a helper for accessing the contents of a poor_wl_buffer
 * resource created via the poor_wl_shm interface.
 *
 * A poor_wl_shm_buffer becomes invalid as soon as its #poor_wl_resource is destroyed.
 */
struct poor_wl_shm_buffer {
	struct poor_wl_buffer_resource resource;
	int internal_refcount;
	int external_refcount;
	struct poor_wl_client *client;
	struct poor_wl_listener client_destroy_listener;
	int32_t width, height;
	int32_t stride;
	uint32_t format;
	int offset;
	struct poor_wl_shm_mapping *mapping;
	bool resource_destroyed;
};

struct poor_wl_shm_sigbus_data {
	struct poor_wl_shm_mapping *current_mapping;
	int access_count;
	bool fallback_mapping_used;
};

/* Initial exec, so the signal handler doesn't allocate it */
[[gnu::tls_model("initial-exec")]] static thread_local struct poor_wl_shm_sigbus_data sigbus_data;

static void shm_mapping_finish_resize(struct poor_wl_shm_mapping *mapping)
{
	void *data;

	if (mapping->size == mapping->new_size)
		return;
	data = mremap(mapping->data, mapping->size, mapping->new_size, MREMAP_MAYMOVE);
	if (data == MAP_FAILED) {
		if (!mapping->resource_destroyed)
			poor_wl_resource_post_error(&mapping->resource.resource, POOR_WL_SHM_ERROR_INVALID_FD, "failed mremap");
		return;
	}
	mapping->data = data;
	mapping->size = mapping->new_size;
}

static void shm_mapping_unref(struct poor_wl_shm_mapping *mapping, bool external)
{
	if (external) {
		mapping->external_refcount--;
		if (mapping->external_refcount < 0)
			poor_wl_abort("Requested to unref an external reference to mapping but none found\n");
		if (mapping->external_refcount == 0)
			shm_mapping_finish_resize(mapping);
	} else {
		mapping->internal_refcount--;
		if (mapping->internal_refcount < 0)
			poor_wl_abort("Requested to unref an internal reference to mapping but none found\n");
	}
	if (mapping->internal_refcount + mapping->external_refcount > 0)
		return;
	munmap(mapping->data, mapping->size);
	mapping->display->free(mapping->display, mapping);
}

static void shm_buffer_unref(struct poor_wl_shm_buffer *buffer, bool external)
{
	struct poor_wl_shm_mapping *mapping = buffer->mapping;

	if (external) {
		buffer->external_refcount--;
		if (buffer->external_refcount < 0)
			poor_wl_abort("Requested to unref an external reference to buffer but none found\n");
	} else {
		buffer->internal_refcount--;
		if (buffer->internal_refcount < 0)
			poor_wl_abort("Requested to unref an internal reference to buffer but none found\n");
	}
	if (buffer->internal_refcount + buffer->external_refcount > 0)
		return;
	if (buffer->client)
		poor_wl_list_remove(&buffer->client_destroy_listener.link);
	mapping->display->free(mapping->display, buffer);
	shm_mapping_unref(mapping, false);
}

static void destroy_buffer(struct poor_wl_resource *resource)
{
	struct poor_wl_shm_buffer *buffer = container_of(resource, struct poor_wl_shm_buffer, resource.resource);

	buffer->resource_destroyed = true;
	shm_buffer_unref(buffer, false);
}

static void shm_buffer_destroy(struct poor_wl_client *, struct poor_wl_buffer_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_buffer_interface shm_buffer_interface = {
	shm_buffer_destroy,
};

static bool format_is_supported(struct poor_wl_client *client, uint32_t format)
{
	struct poor_wl_display *display = poor_wl_client_get_display(client);

	if (format == POOR_WL_SHM_FORMAT_ARGB8888 || format == POOR_WL_SHM_FORMAT_XRGB8888)
		return true;
	foreach_array_const_ref(array_ptr(display->shm_formats, display->shm_formats_count), supported)
		if (*supported == format)
			return true;
	return false;
}

static void shm_buffer_client_destroy_notify(struct poor_wl_listener *listener, void *)
{
	struct poor_wl_shm_buffer *buffer = container_of(listener, struct poor_wl_shm_buffer, client_destroy_listener);

	buffer->client = nullptr;
	poor_wl_list_remove(&buffer->client_destroy_listener.link);
}

static void shm_pool_create_buffer(struct poor_wl_client *client, struct poor_wl_shm_pool_resource *resource, uint32_t id,
				   int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format)
{
	struct poor_wl_shm_mapping *mapping = container_of(resource, struct poor_wl_shm_mapping, resource);
	struct poor_wl_display *display = mapping->display;
	struct poor_wl_shm_buffer *buffer;

	if (!format_is_supported(client, format)) {
		poor_wl_resource_post_error(&resource->resource, POOR_WL_SHM_ERROR_INVALID_FORMAT, "invalid format 0x%x", format);
		return;
	}
	if (offset < 0 || width <= 0 || height <= 0 || stride < width || INT32_MAX / stride < height ||
	    offset > mapping->size - stride * height) {
		poor_wl_resource_post_error(&resource->resource, POOR_WL_SHM_ERROR_INVALID_STRIDE,
				       "invalid width, height or stride (%dx%d, %u)", width, height, stride);
		return;
	}
	buffer = display->alloc ? display->alloc(display, client, sizeof(*buffer)) : nullptr;
	if (!buffer) {
		poor_wl_client_post_no_memory(client);
		return;
	}
	if (poor_wl_buffer_resource_init(&buffer->resource, client, 1, id) < 0) {
		poor_wl_client_post_no_memory(client);
		display->free(display, buffer);
		return;
	}
	buffer->resource_destroyed = false;
	buffer->client = client;
	buffer->client_destroy_listener.notify = shm_buffer_client_destroy_notify;
	poor_wl_client_add_destroy_listener(client, &buffer->client_destroy_listener);
	buffer->internal_refcount = 1;
	buffer->external_refcount = 0;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->stride = stride;
	buffer->offset = offset;
	buffer->mapping = mapping;
	mapping->internal_refcount++;
	poor_wl_buffer_resource_set_implementation(&buffer->resource, &shm_buffer_interface, nullptr, destroy_buffer);
}

static void destroy_pool(struct poor_wl_resource *resource)
{
	struct poor_wl_shm_mapping *mapping = container_of(resource, struct poor_wl_shm_mapping, resource.resource);

	mapping->resource_destroyed = true;
	shm_mapping_unref(mapping, false);
}

static void shm_pool_destroy(struct poor_wl_client *, struct poor_wl_shm_pool_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

/*
 * If the compositor has taken references on the mapping, it may be caching pointers into it, so the resize, which may
 * move the mapping, waits until the compositor drops them.
 */
static void shm_pool_resize(struct poor_wl_client *, struct poor_wl_shm_pool_resource *resource, int32_t size)
{
	struct poor_wl_shm_mapping *mapping = container_of(resource, struct poor_wl_shm_mapping, resource);

	if (size < mapping->size) {
		poor_wl_resource_post_error(&resource->resource, POOR_WL_SHM_ERROR_INVALID_FD, "shrinking pool invalid");
		return;
	}
	mapping->new_size = size;
	if (mapping->external_refcount == 0)
		shm_mapping_finish_resize(mapping);
}

static const struct poor_wl_shm_pool_interface shm_pool_interface = {
	shm_pool_create_buffer,
	shm_pool_destroy,
	shm_pool_resize,
};

/* The fd is only needed for the mapping */
static void shm_create_pool(struct poor_wl_client *client, struct poor_wl_shm_resource *resource, uint32_t id, int fd,
			    int32_t size)
{
	struct poor_wl_display *display = poor_wl_client_get_display(client);
	struct poor_wl_shm_mapping *mapping;

	if (size <= 0) {
		poor_wl_resource_post_error(&resource->resource, POOR_WL_SHM_ERROR_INVALID_STRIDE, "invalid size (%d)", size);
		goto out;
	}
	mapping = display->alloc ? display->alloc(display, client, sizeof(*mapping)) : nullptr;
	if (!mapping) {
		poor_wl_client_post_no_memory(client);
		goto out;
	}
	mapping->data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping->data == MAP_FAILED) {
		poor_wl_resource_post_error(&resource->resource, POOR_WL_SHM_ERROR_INVALID_FD, "failed mmap fd %d: %s", fd,
				       strerror(errno));
		display->free(display, mapping);
		goto out;
	}
	if (poor_wl_shm_pool_resource_init(&mapping->resource, client, resource->resource.version, id) < 0) {
		poor_wl_client_post_no_memory(client);
		munmap(mapping->data, size);
		display->free(display, mapping);
		goto out;
	}
	mapping->resource_destroyed = false;
	mapping->display = display;
	mapping->internal_refcount = 1;
	mapping->external_refcount = 0;
	mapping->size = size;
	mapping->new_size = size;
	poor_wl_shm_pool_resource_set_implementation(&mapping->resource, &shm_pool_interface, nullptr, destroy_pool);
out:
	poor_loop_close_fd_or_submit(display->loop, fd, IOSQE_ASYNC);
}

static void shm_release(struct poor_wl_client *, struct poor_wl_shm_resource *resource)
{
	poor_wl_resource_destroy(&resource->resource);
}

static const struct poor_wl_shm_interface shm_interface = {
	shm_create_pool,
	shm_release,
};

static void destroy_shm(struct poor_wl_resource *resource)
{
	struct poor_wl_display *display = poor_wl_client_get_display(resource->client);

	display->free(display, container_of(resource, struct poor_wl_shm_resource, resource));
}

static void bind_shm(struct poor_wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct poor_wl_display *display = poor_wl_client_get_display(client);
	struct poor_wl_shm_resource *resource = display->alloc ? display->alloc(display, client, sizeof(*resource)) : nullptr;

	if (!resource) {
		poor_wl_client_post_no_memory(client);
		return;
	}
	if (poor_wl_shm_resource_init(resource, client, version, id) < 0) {
		display->free(display, resource);
		poor_wl_client_post_no_memory(client);
		return;
	}
	poor_wl_shm_resource_set_implementation(resource, &shm_interface, data, destroy_shm);
	poor_wl_shm_send_format(resource, POOR_WL_SHM_FORMAT_ARGB8888);
	poor_wl_shm_send_format(resource, POOR_WL_SHM_FORMAT_XRGB8888);
	foreach_array_const_ref(array_ptr(display->shm_formats, display->shm_formats_count), format)
		poor_wl_shm_send_format(resource, *format);
}

POOR_WL_EXPORT void poor_wl_display_init_shm(struct poor_wl_display *display, struct poor_wl_global *global)
{
	poor_wl_global_init(global, display, &poor_wl_shm_interface, 2, nullptr, bind_shm);
}

POOR_WL_EXPORT struct poor_wl_shm_buffer *poor_wl_shm_buffer_get(struct poor_wl_buffer_resource *resource)
{
	if (resource && poor_wl_resource_instance_of(&resource->resource, &poor_wl_buffer_interface, &shm_buffer_interface))
		return container_of(resource, struct poor_wl_shm_buffer, resource);
	return nullptr;
}

POOR_WL_EXPORT int32_t poor_wl_shm_buffer_get_stride(const struct poor_wl_shm_buffer *buffer)
{
	return buffer->stride;
}

/*
 * As this buffer is memory-mapped, reading from it may generate SIGBUS signals. This can happen if the client claims
 * that the buffer is larger than it is or if something truncates the underlying file. To prevent this signal from
 * causing the compositor to crash, call poor_wl_shm_buffer_begin_access() and poor_wl_shm_buffer_end_access() around code that
 * reads from the memory.
 */
POOR_WL_EXPORT void *poor_wl_shm_buffer_get_data(struct poor_wl_shm_buffer *buffer)
{
	if (buffer->mapping->external_refcount && buffer->mapping->size != buffer->mapping->new_size)
		poor_wl_log(poor_wl_server_log_handler, "Buffer address requested when its parent pool has an external reference "
			    "and a deferred resize pending.\n");
	return buffer->mapping->data + buffer->offset;
}

POOR_WL_EXPORT uint32_t poor_wl_shm_buffer_get_format(const struct poor_wl_shm_buffer *buffer)
{
	return buffer->format;
}

POOR_WL_EXPORT int32_t poor_wl_shm_buffer_get_width(const struct poor_wl_shm_buffer *buffer)
{
	return buffer->width;
}

POOR_WL_EXPORT int32_t poor_wl_shm_buffer_get_height(const struct poor_wl_shm_buffer *buffer)
{
	return buffer->height;
}

/* Take a reference, which the compositor drops with poor_wl_shm_buffer_unref() */
POOR_WL_EXPORT struct poor_wl_shm_buffer *poor_wl_shm_buffer_ref(struct poor_wl_shm_buffer *buffer)
{
	buffer->external_refcount++;
	return buffer;
}

POOR_WL_EXPORT void poor_wl_shm_buffer_unref(struct poor_wl_shm_buffer *buffer)
{
	shm_buffer_unref(buffer, true);
}

/* Keep the memory of the buffer's pool mapped at the same address until poor_wl_shm_mapping_unref() */
POOR_WL_EXPORT struct poor_wl_shm_mapping *poor_wl_shm_buffer_ref_mapping(struct poor_wl_shm_buffer *buffer)
{
	buffer->mapping->external_refcount++;
	return buffer->mapping;
}

POOR_WL_EXPORT void poor_wl_shm_mapping_unref(struct poor_wl_shm_mapping *mapping)
{
	shm_mapping_unref(mapping, true);
}

/* A SIGBUS for something else than accessing the pool kills the process with the previous handler */
static void reraise_sigbus(void)
{
	sigaction(SIGBUS, &poor_wl_shm_old_sigbus_action, nullptr);
	raise(SIGBUS);
}

static void sigbus_handler(int, siginfo_t *info, void *)
{
	struct poor_wl_shm_mapping *mapping = sigbus_data.current_mapping;

	if (!mapping || (char *)info->si_addr < mapping->data || (char *)info->si_addr >= mapping->data + mapping->size) {
		reraise_sigbus();
		return;
	}
	sigbus_data.fallback_mapping_used = true;
	if (mmap(mapping->data, mapping->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, 0, 0) ==
	    MAP_FAILED)
		reraise_sigbus();
}

static void init_sigbus_handler(void)
{
	struct sigaction new_action = {
		.sa_sigaction = sigbus_handler,
		.sa_flags = SA_SIGINFO | SA_NODEFER,
	};

	sigemptyset(&new_action.sa_mask);
	sigaction(SIGBUS, &new_action, &poor_wl_shm_old_sigbus_action);
}

/*
 * Mark that the given SHM buffer is about to be accessed.
 *
 * Reading from a memory-mapped region that extends off the end of the file generates a SIGBUS signal. This installs
 * a SIGBUS handler for the lifetime of the process that maps anonymous memory over the pool instead, and the client
 * gets an error event in poor_wl_shm_buffer_end_access(). Signals for addresses outside the pool are reraised.
 *
 * Nested calls must access the same pool and be balanced with poor_wl_shm_buffer_end_access(). Different threads can
 * access different buffers or the same buffer.
 */
POOR_WL_EXPORT void poor_wl_shm_buffer_begin_access(struct poor_wl_shm_buffer *buffer)
{
	struct poor_wl_shm_mapping *mapping = buffer->mapping;

	pthread_once(&poor_wl_shm_sigbus_once, init_sigbus_handler);
	if (sigbus_data.current_mapping && sigbus_data.current_mapping != mapping)
		poor_wl_abort("Incorrect pool passed for current thread\n");
	sigbus_data.current_mapping = mapping;
	sigbus_data.access_count++;
}

POOR_WL_EXPORT void poor_wl_shm_buffer_end_access(struct poor_wl_shm_buffer *buffer)
{
	if (sigbus_data.access_count < 1)
		poor_wl_abort("poor_wl_shm_buffer_begin_access wasn't called before\n");
	if (--sigbus_data.access_count)
		return;
	if (sigbus_data.fallback_mapping_used) {
		if (!buffer->resource_destroyed)
			poor_wl_resource_post_error(&buffer->resource.resource, POOR_WL_SHM_ERROR_INVALID_FD,
					       "error accessing SHM buffer");
		else if (buffer->client)
			poor_wl_client_post_implementation_error(buffer->client, "Error accessing SHM buffer of a wl_buffer resource "
									    "which has already been destroyed");
		sigbus_data.fallback_mapping_used = false;
	}
	sigbus_data.current_mapping = nullptr;
}
