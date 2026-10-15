/* SCANNER TEST */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "poor_wayland_util.h"


static const struct poor_wl_interface *poor_empty_types[] = {
};

static const struct poor_wl_message poor_empty_requests[] = {
	{ "empty", "", poor_empty_types + 0 },
};

POOR_WL_EXPORT const struct poor_wl_interface poor_empty_interface = {
	"empty", 1,
	1, poor_empty_requests,
	0, NULL,
};

