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
#include "wayland-server-display.h"
#include "wayland-server.h"

/*
 * The once is used to install the SIGBUS handler in the first call of wl_shm_buffer_begin_access(), which can happen
 * from any thread
 */
static pthread_once_t wl_shm_sigbus_once = PTHREAD_ONCE_INIT;
static struct sigaction wl_shm_old_sigbus_action;

struct wl_shm_pool {
	struct wl_resource storage;
	struct wl_resource *resource;
	struct wl_display *display;
	int internal_refcount;
	int external_refcount;
	char *data;
	ssize_t size;
	ssize_t new_size;
};

/** \class wl_shm_buffer
 *
 * \brief A SHM buffer
 *
 * wl_shm_buffer provides a helper for accessing the contents of a wl_buffer
 * resource created via the wl_shm interface.
 *
 * A wl_shm_buffer becomes invalid as soon as its #wl_resource is destroyed.
 */
struct wl_shm_buffer {
	struct wl_resource storage;
	struct wl_resource *resource;
	int internal_refcount;
	int external_refcount;
	struct wl_client *client;
	struct wl_listener client_destroy_listener;
	int32_t width, height;
	int32_t stride;
	uint32_t format;
	int offset;
	struct wl_shm_pool *pool;
};

struct wl_shm_sigbus_data {
	struct wl_shm_pool *current_pool;
	int access_count;
	bool fallback_mapping_used;
};

/* Initial exec, so the signal handler doesn't allocate it */
[[gnu::tls_model("initial-exec")]] static thread_local struct wl_shm_sigbus_data sigbus_data;

static void shm_pool_finish_resize(struct wl_shm_pool *pool)
{
	void *data;

	if (pool->size == pool->new_size)
		return;
	data = mremap(pool->data, pool->size, pool->new_size, MREMAP_MAYMOVE);
	if (data == MAP_FAILED) {
		if (pool->resource)
			wl_resource_post_error(pool->resource, WL_SHM_ERROR_INVALID_FD, "failed mremap");
		return;
	}
	pool->data = data;
	pool->size = pool->new_size;
}

static void shm_pool_unref(struct wl_shm_pool *pool, bool external)
{
	if (external) {
		pool->external_refcount--;
		if (pool->external_refcount < 0)
			wl_abort("Requested to unref an external reference to pool but none found\n");
		if (pool->external_refcount == 0)
			shm_pool_finish_resize(pool);
	} else {
		pool->internal_refcount--;
		if (pool->internal_refcount < 0)
			wl_abort("Requested to unref an internal reference to pool but none found\n");
	}
	if (pool->internal_refcount + pool->external_refcount > 0)
		return;
	munmap(pool->data, pool->size);
	pool->display->free(pool->display, pool);
}

static void shm_buffer_unref(struct wl_shm_buffer *buffer, bool external)
{
	struct wl_shm_pool *pool = buffer->pool;

	if (external) {
		buffer->external_refcount--;
		if (buffer->external_refcount < 0)
			wl_abort("Requested to unref an external reference to buffer but none found\n");
	} else {
		buffer->internal_refcount--;
		if (buffer->internal_refcount < 0)
			wl_abort("Requested to unref an internal reference to buffer but none found\n");
	}
	if (buffer->internal_refcount + buffer->external_refcount > 0)
		return;
	if (buffer->client)
		wl_list_remove(&buffer->client_destroy_listener.link);
	pool->display->free(pool->display, buffer);
	shm_pool_unref(pool, false);
}

static void destroy_buffer(struct wl_resource *resource)
{
	struct wl_shm_buffer *buffer = wl_resource_get_user_data(resource);

	buffer->resource = nullptr;
	shm_buffer_unref(buffer, false);
}

static void shm_buffer_destroy(struct wl_client *, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface shm_buffer_interface = {
	shm_buffer_destroy,
};

static bool format_is_supported(struct wl_client *client, uint32_t format)
{
	struct wl_display *display = wl_client_get_display(client);

	if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888)
		return true;
	for (size_t i = 0; i < display->shm_formats_count; i++)
		if (display->shm_formats[i] == format)
			return true;
	return false;
}

static void shm_buffer_client_destroy_notify(struct wl_listener *listener, void *)
{
	struct wl_shm_buffer *buffer = container_of(listener, struct wl_shm_buffer, client_destroy_listener);

	buffer->client = nullptr;
	wl_list_remove(&buffer->client_destroy_listener.link);
}

static void shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t offset,
				   int32_t width, int32_t height, int32_t stride, uint32_t format)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);
	struct wl_display *display = pool->display;
	struct wl_shm_buffer *buffer;

	if (!format_is_supported(client, format)) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT, "invalid format 0x%x", format);
		return;
	}
	if (offset < 0 || width <= 0 || height <= 0 || stride < width || INT32_MAX / stride < height ||
	    offset > pool->size - stride * height) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE, "invalid width, height or stride (%dx%d, %u)", width,
				       height, stride);
		return;
	}
	buffer = display->alloc ? display->alloc(display, client, sizeof(*buffer)) : nullptr;
	if (!buffer) {
		wl_client_post_no_memory(client);
		return;
	}
	if (wl_resource_init(&buffer->storage, client, &wl_buffer_interface, 1, id) < 0) {
		wl_client_post_no_memory(client);
		display->free(display, buffer);
		return;
	}
	buffer->resource = &buffer->storage;
	buffer->client = client;
	buffer->client_destroy_listener.notify = shm_buffer_client_destroy_notify;
	wl_client_add_destroy_listener(client, &buffer->client_destroy_listener);
	buffer->internal_refcount = 1;
	buffer->external_refcount = 0;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->stride = stride;
	buffer->offset = offset;
	buffer->pool = pool;
	pool->internal_refcount++;
	wl_resource_set_implementation(buffer->resource, &shm_buffer_interface, buffer, destroy_buffer);
}

static void destroy_pool(struct wl_resource *resource)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);

	pool->resource = nullptr;
	shm_pool_unref(pool, false);
}

static void shm_pool_destroy(struct wl_client *, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

/*
 * If the compositor has taken references on the pool, it may be caching pointers into it, so the resize, which may
 * move the mapping, waits until the compositor drops them.
 */
static void shm_pool_resize(struct wl_client *, struct wl_resource *resource, int32_t size)
{
	struct wl_shm_pool *pool = wl_resource_get_user_data(resource);

	if (size < pool->size) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "shrinking pool invalid");
		return;
	}
	pool->new_size = size;
	if (pool->external_refcount == 0)
		shm_pool_finish_resize(pool);
}

static const struct wl_shm_pool_interface shm_pool_interface = {
	shm_pool_create_buffer,
	shm_pool_destroy,
	shm_pool_resize,
};

/* The fd is only needed for the mapping */
static void shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id, int fd, int32_t size)
{
	struct wl_display *display = wl_client_get_display(client);
	struct wl_shm_pool *pool;

	if (size <= 0) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE, "invalid size (%d)", size);
		goto out;
	}
	pool = display->alloc ? display->alloc(display, client, sizeof(*pool)) : nullptr;
	if (!pool) {
		wl_client_post_no_memory(client);
		goto out;
	}
	pool->data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pool->data == MAP_FAILED) {
		wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FD, "failed mmap fd %d: %s", fd, strerror(errno));
		display->free(display, pool);
		goto out;
	}
	if (wl_resource_init(&pool->storage, client, &wl_shm_pool_interface, wl_resource_get_version(resource), id) < 0) {
		wl_client_post_no_memory(client);
		munmap(pool->data, size);
		display->free(display, pool);
		goto out;
	}
	pool->resource = &pool->storage;
	pool->display = display;
	pool->internal_refcount = 1;
	pool->external_refcount = 0;
	pool->size = size;
	pool->new_size = size;
	wl_resource_set_implementation(pool->resource, &shm_pool_interface, pool, destroy_pool);
out:
	poor_loop_close_fd_or_submit(display->loop, fd, 0);
}

static void shm_release(struct wl_client *, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_shm_interface shm_interface = {
	shm_create_pool,
	shm_release,
};

static void destroy_shm(struct wl_resource *resource)
{
	struct wl_display *display = wl_client_get_display(resource->client);

	display->free(display, resource);
}

static void bind_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_display *display = wl_client_get_display(client);
	struct wl_resource *resource = display->alloc ? display->alloc(display, client, sizeof(*resource)) : nullptr;

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	if (wl_resource_init(resource, client, &wl_shm_interface, version, id) < 0) {
		display->free(display, resource);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &shm_interface, data, destroy_shm);
	wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
	for (size_t i = 0; i < display->shm_formats_count; i++)
		wl_shm_send_format(resource, display->shm_formats[i]);
}

WL_EXPORT void wl_display_init_shm(struct wl_display *display, struct wl_global *global)
{
	wl_global_init(global, display, &wl_shm_interface, 2, nullptr, bind_shm);
}

WL_EXPORT struct wl_shm_buffer *wl_shm_buffer_get(struct wl_resource *resource)
{
	if (resource && wl_resource_instance_of(resource, &wl_buffer_interface, &shm_buffer_interface))
		return wl_resource_get_user_data(resource);
	return nullptr;
}

WL_EXPORT int32_t wl_shm_buffer_get_stride(const struct wl_shm_buffer *buffer)
{
	return buffer->stride;
}

/*
 * As this buffer is memory-mapped, reading from it may generate SIGBUS signals. This can happen if the client claims
 * that the buffer is larger than it is or if something truncates the underlying file. To prevent this signal from
 * causing the compositor to crash, call wl_shm_buffer_begin_access() and wl_shm_buffer_end_access() around code that
 * reads from the memory.
 */
WL_EXPORT void *wl_shm_buffer_get_data(struct wl_shm_buffer *buffer)
{
	if (buffer->pool->external_refcount && buffer->pool->size != buffer->pool->new_size)
		wl_log("Buffer address requested when its parent pool has an external reference and a deferred resize "
		       "pending.\n");
	return buffer->pool->data + buffer->offset;
}

WL_EXPORT uint32_t wl_shm_buffer_get_format(const struct wl_shm_buffer *buffer)
{
	return buffer->format;
}

WL_EXPORT int32_t wl_shm_buffer_get_width(const struct wl_shm_buffer *buffer)
{
	return buffer->width;
}

WL_EXPORT int32_t wl_shm_buffer_get_height(const struct wl_shm_buffer *buffer)
{
	return buffer->height;
}

/* Take a reference, which the compositor drops with wl_shm_buffer_unref() */
WL_EXPORT struct wl_shm_buffer *wl_shm_buffer_ref(struct wl_shm_buffer *buffer)
{
	buffer->external_refcount++;
	return buffer;
}

WL_EXPORT void wl_shm_buffer_unref(struct wl_shm_buffer *buffer)
{
	shm_buffer_unref(buffer, true);
}

/* Take a reference to the buffer's pool, which the compositor drops with wl_shm_pool_unref() */
WL_EXPORT struct wl_shm_pool *wl_shm_buffer_ref_pool(struct wl_shm_buffer *buffer)
{
	buffer->pool->external_refcount++;
	return buffer->pool;
}

WL_EXPORT void wl_shm_pool_unref(struct wl_shm_pool *pool)
{
	shm_pool_unref(pool, true);
}

/* A SIGBUS for something else than accessing the pool kills the process with the previous handler */
static void reraise_sigbus(void)
{
	sigaction(SIGBUS, &wl_shm_old_sigbus_action, nullptr);
	raise(SIGBUS);
}

static void sigbus_handler(int, siginfo_t *info, void *)
{
	struct wl_shm_pool *pool = sigbus_data.current_pool;

	if (!pool || (char *)info->si_addr < pool->data || (char *)info->si_addr >= pool->data + pool->size) {
		reraise_sigbus();
		return;
	}
	sigbus_data.fallback_mapping_used = true;
	if (mmap(pool->data, pool->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, 0, 0) == MAP_FAILED)
		reraise_sigbus();
}

static void init_sigbus_handler(void)
{
	struct sigaction new_action = {
		.sa_sigaction = sigbus_handler,
		.sa_flags = SA_SIGINFO | SA_NODEFER,
	};

	sigemptyset(&new_action.sa_mask);
	sigaction(SIGBUS, &new_action, &wl_shm_old_sigbus_action);
}

/*
 * Mark that the given SHM buffer is about to be accessed.
 *
 * Reading from a memory-mapped region that extends off the end of the file generates a SIGBUS signal. This installs
 * a SIGBUS handler for the lifetime of the process that maps anonymous memory over the pool instead, and the client
 * gets an error event in wl_shm_buffer_end_access(). Signals for addresses outside the pool are reraised.
 *
 * Nested calls must access the same pool and be balanced with wl_shm_buffer_end_access(). Different threads can
 * access different buffers or the same buffer.
 */
WL_EXPORT void wl_shm_buffer_begin_access(struct wl_shm_buffer *buffer)
{
	struct wl_shm_pool *pool = buffer->pool;

	pthread_once(&wl_shm_sigbus_once, init_sigbus_handler);
	if (sigbus_data.current_pool && sigbus_data.current_pool != pool)
		wl_abort("Incorrect pool passed for current thread\n");
	sigbus_data.current_pool = pool;
	sigbus_data.access_count++;
}

WL_EXPORT void wl_shm_buffer_end_access(struct wl_shm_buffer *buffer)
{
	if (sigbus_data.access_count < 1)
		wl_abort("wl_shm_buffer_begin_access wasn't called before\n");
	if (--sigbus_data.access_count)
		return;
	if (sigbus_data.fallback_mapping_used) {
		if (buffer->resource)
			wl_resource_post_error(buffer->resource, WL_SHM_ERROR_INVALID_FD, "error accessing SHM buffer");
		else if (buffer->client)
			wl_client_post_implementation_error(buffer->client, "Error accessing SHM buffer of a wl_buffer resource "
									    "which has already been destroyed");
		sigbus_data.fallback_mapping_used = false;
	}
	sigbus_data.current_pool = nullptr;
}
