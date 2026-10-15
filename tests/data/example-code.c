/* SCANNER TEST */

/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2012-2013 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
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

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "poor_wayland_util.h"

extern const struct poor_wl_interface poor_wl_buffer_interface;
extern const struct poor_wl_interface poor_wl_callback_interface;
extern const struct poor_wl_interface poor_wl_data_device_interface;
extern const struct poor_wl_interface poor_wl_data_offer_interface;
extern const struct poor_wl_interface poor_wl_data_source_interface;
extern const struct poor_wl_interface poor_wl_keyboard_interface;
extern const struct poor_wl_interface poor_wl_output_interface;
extern const struct poor_wl_interface poor_wl_pointer_interface;
extern const struct poor_wl_interface poor_wl_region_interface;
extern const struct poor_wl_interface poor_wl_registry_interface;
extern const struct poor_wl_interface poor_wl_seat_interface;
extern const struct poor_wl_interface poor_wl_shell_surface_interface;
extern const struct poor_wl_interface poor_wl_shm_pool_interface;
extern const struct poor_wl_interface poor_wl_subsurface_interface;
extern const struct poor_wl_interface poor_wl_surface_interface;
extern const struct poor_wl_interface poor_wl_touch_interface;

static const struct poor_wl_interface *poor_wayland_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&poor_wl_callback_interface,
	&poor_wl_registry_interface,
	&poor_wl_surface_interface,
	&poor_wl_region_interface,
	&poor_wl_buffer_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&poor_wl_shm_pool_interface,
	NULL,
	NULL,
	&poor_wl_data_source_interface,
	&poor_wl_surface_interface,
	&poor_wl_surface_interface,
	NULL,
	&poor_wl_data_source_interface,
	NULL,
	&poor_wl_data_offer_interface,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	&poor_wl_data_offer_interface,
	&poor_wl_data_offer_interface,
	&poor_wl_data_source_interface,
	&poor_wl_data_device_interface,
	&poor_wl_seat_interface,
	&poor_wl_shell_surface_interface,
	&poor_wl_surface_interface,
	&poor_wl_seat_interface,
	NULL,
	&poor_wl_seat_interface,
	NULL,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&poor_wl_output_interface,
	&poor_wl_seat_interface,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	NULL,
	&poor_wl_output_interface,
	&poor_wl_buffer_interface,
	NULL,
	NULL,
	&poor_wl_callback_interface,
	&poor_wl_region_interface,
	&poor_wl_region_interface,
	&poor_wl_output_interface,
	&poor_wl_output_interface,
	&poor_wl_pointer_interface,
	&poor_wl_keyboard_interface,
	&poor_wl_touch_interface,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	&poor_wl_surface_interface,
	NULL,
	NULL,
	NULL,
	&poor_wl_subsurface_interface,
	&poor_wl_surface_interface,
	&poor_wl_surface_interface,
	&poor_wl_surface_interface,
	&poor_wl_surface_interface,
};

static const struct poor_wl_message poor_wl_display_requests[] = {
	{ "sync", "n", poor_wayland_types + 8 },
	{ "get_registry", "n", poor_wayland_types + 9 },
};

static const struct poor_wl_message poor_wl_display_events[] = {
	{ "error", "ous", poor_wayland_types + 0 },
	{ "delete_id", "u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_display_interface = {
	"wl_display", 1,
	2, poor_wl_display_requests,
	2, poor_wl_display_events,
};

static const struct poor_wl_message poor_wl_registry_requests[] = {
	{ "bind", "usun", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_registry_events[] = {
	{ "global", "usu", poor_wayland_types + 0 },
	{ "global_remove", "u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_registry_interface = {
	"wl_registry", 1,
	1, poor_wl_registry_requests,
	2, poor_wl_registry_events,
};

static const struct poor_wl_message poor_wl_callback_events[] = {
	{ "done", "u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_callback_interface = {
	"wl_callback", 1,
	0, NULL,
	1, poor_wl_callback_events,
};

static const struct poor_wl_message poor_wl_compositor_requests[] = {
	{ "create_surface", "n", poor_wayland_types + 10 },
	{ "create_region", "n", poor_wayland_types + 11 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_compositor_interface = {
	"wl_compositor", 4,
	2, poor_wl_compositor_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_shm_pool_requests[] = {
	{ "create_buffer", "niiiiu", poor_wayland_types + 12 },
	{ "destroy", "", poor_wayland_types + 0 },
	{ "resize", "i", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_shm_pool_interface = {
	"wl_shm_pool", 1,
	3, poor_wl_shm_pool_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_shm_requests[] = {
	{ "create_pool", "nhi", poor_wayland_types + 18 },
};

static const struct poor_wl_message poor_wl_shm_events[] = {
	{ "format", "u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_shm_interface = {
	"wl_shm", 1,
	1, poor_wl_shm_requests,
	1, poor_wl_shm_events,
};

static const struct poor_wl_message poor_wl_buffer_requests[] = {
	{ "destroy", "", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_buffer_events[] = {
	{ "release", "", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_buffer_interface = {
	"wl_buffer", 1,
	1, poor_wl_buffer_requests,
	1, poor_wl_buffer_events,
};

static const struct poor_wl_message poor_wl_data_offer_requests[] = {
	{ "accept", "u?s", poor_wayland_types + 0 },
	{ "receive", "sh", poor_wayland_types + 0 },
	{ "destroy", "", poor_wayland_types + 0 },
	{ "finish", "3", poor_wayland_types + 0 },
	{ "set_actions", "3uu", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_data_offer_events[] = {
	{ "offer", "s", poor_wayland_types + 0 },
	{ "source_actions", "3u", poor_wayland_types + 0 },
	{ "action", "3u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_data_offer_interface = {
	"wl_data_offer", 3,
	5, poor_wl_data_offer_requests,
	3, poor_wl_data_offer_events,
};

static const struct poor_wl_message poor_wl_data_source_requests[] = {
	{ "offer", "s", poor_wayland_types + 0 },
	{ "destroy", "", poor_wayland_types + 0 },
	{ "set_actions", "3u", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_data_source_events[] = {
	{ "target", "?s", poor_wayland_types + 0 },
	{ "send", "sh", poor_wayland_types + 0 },
	{ "cancelled", "", poor_wayland_types + 0 },
	{ "dnd_drop_performed", "3", poor_wayland_types + 0 },
	{ "dnd_finished", "3", poor_wayland_types + 0 },
	{ "action", "3u", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_data_source_interface = {
	"wl_data_source", 3,
	3, poor_wl_data_source_requests,
	6, poor_wl_data_source_events,
};

static const struct poor_wl_message poor_wl_data_device_requests[] = {
	{ "start_drag", "?oo?ou", poor_wayland_types + 21 },
	{ "set_selection", "?ou", poor_wayland_types + 25 },
	{ "release", "2", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_data_device_events[] = {
	{ "data_offer", "n", poor_wayland_types + 27 },
	{ "enter", "uoff?o", poor_wayland_types + 28 },
	{ "leave", "", poor_wayland_types + 0 },
	{ "motion", "uff", poor_wayland_types + 0 },
	{ "drop", "", poor_wayland_types + 0 },
	{ "selection", "?o", poor_wayland_types + 33 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_data_device_interface = {
	"wl_data_device", 3,
	3, poor_wl_data_device_requests,
	6, poor_wl_data_device_events,
};

static const struct poor_wl_message poor_wl_data_device_manager_requests[] = {
	{ "create_data_source", "n", poor_wayland_types + 34 },
	{ "get_data_device", "no", poor_wayland_types + 35 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_data_device_manager_interface = {
	"wl_data_device_manager", 3,
	2, poor_wl_data_device_manager_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_shell_requests[] = {
	{ "get_shell_surface", "no", poor_wayland_types + 37 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_shell_interface = {
	"wl_shell", 1,
	1, poor_wl_shell_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_shell_surface_requests[] = {
	{ "pong", "u", poor_wayland_types + 0 },
	{ "move", "ou", poor_wayland_types + 39 },
	{ "resize", "ouu", poor_wayland_types + 41 },
	{ "set_toplevel", "", poor_wayland_types + 0 },
	{ "set_transient", "oiiu", poor_wayland_types + 44 },
	{ "set_fullscreen", "uu?o", poor_wayland_types + 48 },
	{ "set_popup", "ouoiiu", poor_wayland_types + 51 },
	{ "set_maximized", "?o", poor_wayland_types + 57 },
	{ "set_title", "s", poor_wayland_types + 0 },
	{ "set_class", "s", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_shell_surface_events[] = {
	{ "ping", "u", poor_wayland_types + 0 },
	{ "configure", "uii", poor_wayland_types + 0 },
	{ "popup_done", "", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_shell_surface_interface = {
	"wl_shell_surface", 1,
	10, poor_wl_shell_surface_requests,
	3, poor_wl_shell_surface_events,
};

static const struct poor_wl_message poor_wl_surface_requests[] = {
	{ "destroy", "", poor_wayland_types + 0 },
	{ "attach", "?oii", poor_wayland_types + 58 },
	{ "damage", "iiii", poor_wayland_types + 0 },
	{ "frame", "n", poor_wayland_types + 61 },
	{ "set_opaque_region", "?o", poor_wayland_types + 62 },
	{ "set_input_region", "?o", poor_wayland_types + 63 },
	{ "commit", "", poor_wayland_types + 0 },
	{ "set_buffer_transform", "2i", poor_wayland_types + 0 },
	{ "set_buffer_scale", "3i", poor_wayland_types + 0 },
	{ "damage_buffer", "4iiii", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_surface_events[] = {
	{ "enter", "o", poor_wayland_types + 64 },
	{ "leave", "o", poor_wayland_types + 65 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_surface_interface = {
	"wl_surface", 4,
	10, poor_wl_surface_requests,
	2, poor_wl_surface_events,
};

static const struct poor_wl_message poor_wl_seat_requests[] = {
	{ "get_pointer", "n", poor_wayland_types + 66 },
	{ "get_keyboard", "n", poor_wayland_types + 67 },
	{ "get_touch", "n", poor_wayland_types + 68 },
	{ "release", "5", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_seat_events[] = {
	{ "capabilities", "u", poor_wayland_types + 0 },
	{ "name", "2s", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_seat_interface = {
	"wl_seat", 6,
	4, poor_wl_seat_requests,
	2, poor_wl_seat_events,
};

static const struct poor_wl_message poor_wl_pointer_requests[] = {
	{ "set_cursor", "u?oii", poor_wayland_types + 69 },
	{ "release", "3", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_pointer_events[] = {
	{ "enter", "uoff", poor_wayland_types + 73 },
	{ "leave", "uo", poor_wayland_types + 77 },
	{ "motion", "uff", poor_wayland_types + 0 },
	{ "button", "uuuu", poor_wayland_types + 0 },
	{ "axis", "uuf", poor_wayland_types + 0 },
	{ "frame", "5", poor_wayland_types + 0 },
	{ "axis_source", "5u", poor_wayland_types + 0 },
	{ "axis_stop", "5uu", poor_wayland_types + 0 },
	{ "axis_discrete", "5ui", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_pointer_interface = {
	"wl_pointer", 6,
	2, poor_wl_pointer_requests,
	9, poor_wl_pointer_events,
};

static const struct poor_wl_message poor_wl_keyboard_requests[] = {
	{ "release", "3", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_keyboard_events[] = {
	{ "keymap", "uhu", poor_wayland_types + 0 },
	{ "enter", "uoa", poor_wayland_types + 79 },
	{ "leave", "uo", poor_wayland_types + 82 },
	{ "key", "uuuu", poor_wayland_types + 0 },
	{ "modifiers", "uuuuu", poor_wayland_types + 0 },
	{ "repeat_info", "4ii", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_keyboard_interface = {
	"wl_keyboard", 6,
	1, poor_wl_keyboard_requests,
	6, poor_wl_keyboard_events,
};

static const struct poor_wl_message poor_wl_touch_requests[] = {
	{ "release", "3", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_touch_events[] = {
	{ "down", "uuoiff", poor_wayland_types + 84 },
	{ "up", "uui", poor_wayland_types + 0 },
	{ "motion", "uiff", poor_wayland_types + 0 },
	{ "frame", "", poor_wayland_types + 0 },
	{ "cancel", "", poor_wayland_types + 0 },
	{ "shape", "6iff", poor_wayland_types + 0 },
	{ "orientation", "6if", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_touch_interface = {
	"wl_touch", 6,
	1, poor_wl_touch_requests,
	7, poor_wl_touch_events,
};

static const struct poor_wl_message poor_wl_output_requests[] = {
	{ "release", "3", poor_wayland_types + 0 },
};

static const struct poor_wl_message poor_wl_output_events[] = {
	{ "geometry", "iiiiissi", poor_wayland_types + 0 },
	{ "mode", "uiii", poor_wayland_types + 0 },
	{ "done", "2", poor_wayland_types + 0 },
	{ "scale", "2i", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_output_interface = {
	"wl_output", 3,
	1, poor_wl_output_requests,
	4, poor_wl_output_events,
};

static const struct poor_wl_message poor_wl_region_requests[] = {
	{ "destroy", "", poor_wayland_types + 0 },
	{ "add", "iiii", poor_wayland_types + 0 },
	{ "subtract", "iiii", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_region_interface = {
	"wl_region", 1,
	3, poor_wl_region_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_subcompositor_requests[] = {
	{ "destroy", "", poor_wayland_types + 0 },
	{ "get_subsurface", "noo", poor_wayland_types + 90 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_subcompositor_interface = {
	"wl_subcompositor", 1,
	2, poor_wl_subcompositor_requests,
	0, NULL,
};

static const struct poor_wl_message poor_wl_subsurface_requests[] = {
	{ "destroy", "", poor_wayland_types + 0 },
	{ "set_position", "ii", poor_wayland_types + 0 },
	{ "place_above", "o", poor_wayland_types + 93 },
	{ "place_below", "o", poor_wayland_types + 94 },
	{ "set_sync", "", poor_wayland_types + 0 },
	{ "set_desync", "", poor_wayland_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_wl_subsurface_interface = {
	"wl_subsurface", 1,
	6, poor_wl_subsurface_requests,
	0, NULL,
};

