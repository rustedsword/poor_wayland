/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <string.h>
#include <sys/mman.h>

#include "poor_wayland_swapchain.h"

/* An entry of the format table of the feedback */
struct format_modifier {
	uint32_t format;
	uint32_t padding;
	uint64_t modifier;
};

static void swapchain_fail(struct poor_wl_swapchain *swapchain, int error)
{
	if (swapchain->failed)
		return;
	swapchain->failed = true;
	swapchain->error_fn(swapchain, error);
}

static void buffer_free(struct poor_wl_swapchain_buffer *buffer)
{
	poor_wl_buffer_destroy(&buffer->buffer);
	buffer->allocator->free(buffer->allocator_data, buffer);
	buffer->allocated = buffer->busy = buffer->stale = false;
}

/* A buffer that the compositor or the caller still uses goes once they are done with it */
static void swapchain_drop_buffers(struct poor_wl_swapchain *swapchain)
{
	for (unsigned i = 0; i < POOR_WL_SWAPCHAIN_BUFFERS; i++) {
		struct poor_wl_swapchain_buffer *buffer = &swapchain->buffers[i];

		if (!buffer->allocated)
			continue;
		if (buffer->busy || buffer == swapchain->rendering)
			buffer->stale = true;
		else
			buffer_free(buffer);
	}
}

static void swapchain_render(struct poor_wl_swapchain *swapchain);

static void buffer_release(void *data, struct poor_wl_buffer *)
{
	struct poor_wl_swapchain_buffer *buffer = data;
	struct poor_wl_swapchain *swapchain = buffer->swapchain;

	buffer->busy = false;
	if (buffer->stale)
		buffer_free(buffer);
	swapchain_render(swapchain);
}

static const struct poor_wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

/* The tranches come in the order that the compositor prefers, the first one that the allocator manages is used */
static int buffer_allocate(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer)
{
	const struct poor_wl_swapchain_tranche *tranche;
	int ret = -ENOTSUP;

	buffer->width = swapchain->width;
	buffer->height = swapchain->height;
	for (tranche = swapchain->tranches; tranche < swapchain->tranches + swapchain->tranche_count; tranche++) {
		ret = swapchain->allocator->allocate(swapchain->allocator_data, buffer, swapchain->format, tranche->modifiers,
						     tranche->count,
						     tranche->flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
		if (ret >= 0)
			break;
	}
	if (ret < 0)
		return ret;
	assert(buffer->plane_count && buffer->plane_count <= POOR_WL_SWAPCHAIN_PLANES);
	buffer->allocator = swapchain->allocator;
	buffer->allocator_data = swapchain->allocator_data;
	poor_zwp_linux_dmabuf_v1_create_params(swapchain->dmabuf, &swapchain->params);
	if (tranche->flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING &&
	    poor_zwp_linux_buffer_params_v1_get_version(&swapchain->params) >=
		    POOR_ZWP_LINUX_BUFFER_PARAMS_V1_SET_SAMPLING_DEVICE_SINCE_VERSION) {
		dev_t device = tranche->device;
		struct poor_wl_array array = { .size = sizeof(device), .alloc = sizeof(device), .data = &device };

		poor_zwp_linux_buffer_params_v1_set_sampling_device(&swapchain->params, &array);
	}
	for (unsigned i = 0; i < buffer->plane_count; i++) {
		poor_zwp_linux_buffer_params_v1_add(&swapchain->params, buffer->planes[i].fd, i, buffer->planes[i].offset,
						    buffer->planes[i].stride, buffer->modifier >> 32, buffer->modifier & 0xffffffff);
		poor_loop_close_fd_or_submit(swapchain->loop, buffer->planes[i].fd, 0);
		buffer->planes[i].fd = -1;
	}
	poor_zwp_linux_buffer_params_v1_create_immed(&swapchain->params, &buffer->buffer, buffer->width, buffer->height,
						     swapchain->format, 0);
	poor_zwp_linux_buffer_params_v1_destroy(&swapchain->params);
	poor_wl_buffer_add_listener(&buffer->buffer, &buffer_listener, buffer);
	buffer->allocated = true;
	buffer->busy = buffer->stale = false;
	return 0;
}

static struct poor_wl_swapchain_buffer *swapchain_free_buffer(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *unallocated = nullptr;

	for (unsigned i = 0; i < POOR_WL_SWAPCHAIN_BUFFERS; i++) {
		struct poor_wl_swapchain_buffer *buffer = &swapchain->buffers[i];

		if (buffer->allocated && !buffer->busy && !buffer->stale)
			return buffer;
		if (!buffer->allocated && !unallocated)
			unallocated = buffer;
	}
	return unallocated;
}

/* The caller renders once the compositor wants a frame and a buffer is free, after the feedback came */
static void swapchain_render(struct poor_wl_swapchain *swapchain)
{
	struct poor_wl_swapchain_buffer *buffer;
	int ret;

	if (!swapchain->frame_wanted || swapchain->frame_pending || swapchain->rendering || swapchain->failed ||
	    !swapchain->tranche_count || !swapchain->allocator || !swapchain->width || !swapchain->height)
		return;
	buffer = swapchain_free_buffer(swapchain);
	if (!buffer)
		return;
	if (!buffer->allocated && (ret = buffer_allocate(swapchain, buffer)) < 0) {
		swapchain_fail(swapchain, -ret);
		return;
	}
	swapchain->frame_wanted = false;
	swapchain->rendering = buffer;
	swapchain->frame_fn(swapchain, buffer, swapchain->time);
}

static void frame_done(void *data, struct poor_wl_callback *callback, uint32_t time)
{
	struct poor_wl_swapchain *swapchain = data;

	poor_wl_callback_destroy(callback);
	swapchain->frame_pending = false;
	swapchain->time = time;
	swapchain_render(swapchain);
}

static const struct poor_wl_callback_listener frame_listener = {
	.done = frame_done,
};

static bool tranches_equal(const struct poor_wl_swapchain_tranche *a, const struct poor_wl_swapchain_tranche *b)
{
	return a->device == b->device && a->flags == b->flags && a->count == b->count &&
	       !memcmp(a->modifiers, b->modifiers, a->count * sizeof(a->modifiers[0]));
}

/* Version 6 has no main device, then the first device that samples the buffers is the one to allocate on */
static dev_t feedback_device(const struct poor_wl_swapchain *swapchain)
{
	if (swapchain->has_main_device)
		return swapchain->main_device;
	for (unsigned i = 0; i < swapchain->tranche_count; i++)
		if (swapchain->tranches[i].flags & POOR_ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING)
			return swapchain->tranches[i].device;
	return swapchain->tranches[0].device;
}

static void feedback_done(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *)
{
	struct poor_wl_swapchain *swapchain = data;
	bool changed = swapchain->pending_count != swapchain->tranche_count;
	dev_t device;

	for (unsigned i = 0; i < swapchain->pending_count && !changed; i++)
		changed = !tranches_equal(&swapchain->pending[i], &swapchain->tranches[i]);
	for (unsigned i = 0; i < swapchain->pending_count; i++)
		swapchain->tranches[i] = swapchain->pending[i];
	swapchain->tranche_count = swapchain->pending_count;
	swapchain->pending_count = 0;
	if (!swapchain->tranche_count) {
		swapchain_fail(swapchain, ENOTSUP);
		return;
	}
	device = feedback_device(swapchain);
	if (changed)
		swapchain_drop_buffers(swapchain);
	if (!swapchain->has_device || device != swapchain->device) {
		swapchain->has_device = true;
		swapchain->device = device;
		swapchain->device_fn(swapchain, device);
	}
	swapchain_render(swapchain);
}

/* The table stays until the next one comes */
static void feedback_format_table(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size)
{
	struct poor_wl_swapchain *swapchain = data;
	void *table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

	poor_loop_close_fd_or_submit(swapchain->loop, fd, 0);
	if (table == MAP_FAILED) {
		swapchain_fail(swapchain, errno);
		return;
	}
	if (swapchain->table)
		munmap(swapchain->table, swapchain->table_size);
	swapchain->table = table;
	swapchain->table_size = size;
}

static void feedback_main_device(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *device)
{
	struct poor_wl_swapchain *swapchain = data;

	if (device->size != sizeof(dev_t))
		return;
	memcpy(&swapchain->main_device, device->data, sizeof(dev_t));
	swapchain->has_main_device = true;
}

/* Tranches without the format of the swapchain are left out, as are the ones that don't fit */
static void feedback_tranche_done(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *)
{
	struct poor_wl_swapchain *swapchain = data;

	if (swapchain->tranche.count && swapchain->pending_count < POOR_WL_SWAPCHAIN_TRANCHES)
		swapchain->pending[swapchain->pending_count++] = swapchain->tranche;
	swapchain->tranche.device = 0;
	swapchain->tranche.flags = 0;
	swapchain->tranche.count = 0;
}

static void feedback_tranche_target_device(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *device)
{
	struct poor_wl_swapchain *swapchain = data;

	if (device->size == sizeof(dev_t))
		memcpy(&swapchain->tranche.device, device->data, sizeof(dev_t));
}

static void feedback_tranche_formats(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, struct poor_wl_array *indices)
{
	struct poor_wl_swapchain *swapchain = data;
	const struct format_modifier *table = swapchain->table;
	size_t entries = swapchain->table_size / sizeof(*table);
	struct poor_wl_swapchain_tranche *tranche = &swapchain->tranche;
	const uint16_t *index;

	poor_wl_array_for_each(index, indices)
		if (*index < entries && table[*index].format == swapchain->format && tranche->count < POOR_WL_SWAPCHAIN_MODIFIERS)
			tranche->modifiers[tranche->count++] = table[*index].modifier;
}

static void feedback_tranche_flags(void *data, struct poor_zwp_linux_dmabuf_feedback_v1 *, uint32_t flags)
{
	((struct poor_wl_swapchain *)data)->tranche.flags = flags;
}

static const struct poor_zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
	.done = feedback_done,
	.format_table = feedback_format_table,
	.main_device = feedback_main_device,
	.tranche_done = feedback_tranche_done,
	.tranche_target_device = feedback_tranche_target_device,
	.tranche_formats = feedback_tranche_formats,
	.tranche_flags = feedback_tranche_flags,
};

POOR_WL_EXPORT void poor_wl_swapchain_init(struct poor_wl_swapchain *swapchain, struct poor_loop *loop,
					   struct poor_zwp_linux_dmabuf_v1 *dmabuf, struct poor_wl_surface *surface,
					   uint32_t format, poor_wl_swapchain_device_fn *device,
					   poor_wl_swapchain_frame_fn *frame, poor_wl_swapchain_error_fn *error)
{
	assert(poor_zwp_linux_dmabuf_v1_get_version(dmabuf) >= POOR_ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION);
	swapchain->loop = loop;
	swapchain->dmabuf = dmabuf;
	swapchain->surface = surface;
	swapchain->format = format;
	swapchain->device_fn = device;
	swapchain->frame_fn = frame;
	swapchain->error_fn = error;
	swapchain->allocator = nullptr;
	swapchain->allocator_data = nullptr;
	swapchain->rendering = nullptr;
	swapchain->table = nullptr;
	swapchain->table_size = 0;
	swapchain->tranche_count = swapchain->pending_count = 0;
	swapchain->tranche.device = 0;
	swapchain->tranche.flags = 0;
	swapchain->tranche.count = 0;
	swapchain->main_device = swapchain->device = 0;
	swapchain->time = 0;
	swapchain->width = swapchain->height = 0;
	swapchain->has_main_device = swapchain->has_device = false;
	swapchain->frame_pending = swapchain->frame_wanted = swapchain->failed = false;
	for (unsigned i = 0; i < POOR_WL_SWAPCHAIN_BUFFERS; i++) {
		swapchain->buffers[i].swapchain = swapchain;
		swapchain->buffers[i].allocated = swapchain->buffers[i].busy = swapchain->buffers[i].stale = false;
	}
	poor_zwp_linux_dmabuf_v1_get_surface_feedback(dmabuf, &swapchain->feedback, surface);
	poor_zwp_linux_dmabuf_feedback_v1_add_listener(&swapchain->feedback, &feedback_listener, swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_set_allocator(struct poor_wl_swapchain *swapchain,
						    const struct poor_wl_swapchain_allocator *allocator, void *data)
{
	swapchain_drop_buffers(swapchain);
	swapchain->allocator = allocator;
	swapchain->allocator_data = data;
	swapchain_render(swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_resize(struct poor_wl_swapchain *swapchain, int32_t width, int32_t height)
{
	assert(width > 0 && height > 0);
	if (width == swapchain->width && height == swapchain->height)
		return;
	swapchain->width = width;
	swapchain->height = height;
	swapchain_drop_buffers(swapchain);
	swapchain_render(swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_request_frame(struct poor_wl_swapchain *swapchain)
{
	swapchain->frame_wanted = true;
	swapchain_render(swapchain);
}

POOR_WL_EXPORT void poor_wl_swapchain_present(struct poor_wl_swapchain *swapchain, struct poor_wl_swapchain_buffer *buffer)
{
	struct poor_wl_surface *surface = swapchain->surface;

	assert(buffer == swapchain->rendering);
	swapchain->rendering = nullptr;
	poor_wl_surface_attach(surface, &buffer->buffer, 0, 0);
	if (poor_wl_surface_get_version(surface) >= POOR_WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
		poor_wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	else
		poor_wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
	poor_wl_surface_frame(surface, &swapchain->frame);
	poor_wl_callback_add_listener(&swapchain->frame, &frame_listener, swapchain);
	poor_wl_surface_commit(surface);
	buffer->busy = true;
	swapchain->frame_pending = true;
}

/* A destroyed swapchain doesn't render anymore, if it is destroyed from one of its callbacks */
POOR_WL_EXPORT void poor_wl_swapchain_destroy(struct poor_wl_swapchain *swapchain)
{
	swapchain->failed = true;
	for (unsigned i = 0; i < POOR_WL_SWAPCHAIN_BUFFERS; i++)
		if (swapchain->buffers[i].allocated)
			buffer_free(&swapchain->buffers[i]);
	if (swapchain->frame_pending)
		poor_wl_callback_destroy(&swapchain->frame);
	poor_zwp_linux_dmabuf_feedback_v1_destroy(&swapchain->feedback);
	if (swapchain->table)
		munmap(swapchain->table, swapchain->table_size);
}
