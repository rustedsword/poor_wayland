/* SCANNER TEST */

#ifndef POOR_EMPTY_SERVER_PROTOCOL_H
#define POOR_EMPTY_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "poor_wayland_server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct poor_wl_client;
struct poor_wl_resource;

/**
 * @page page_poor_empty The poor_empty protocol
 * @section page_ifaces_poor_empty Interfaces
 * - @subpage page_iface_poor_empty - 
 */
struct poor_empty_resource;

#ifndef POOR_EMPTY_INTERFACE
#define POOR_EMPTY_INTERFACE
/**
 * @page page_iface_poor_empty poor_empty
 * @section page_iface_poor_empty_api API
 * See @ref iface_poor_empty.
 */
/**
 * @defgroup iface_poor_empty The poor_empty interface
 */
extern const struct poor_wl_interface poor_empty_interface;
#endif

/**
 * @ingroup iface_poor_empty
 * @struct poor_empty_resource
 */
struct poor_empty_resource {
	struct poor_wl_resource resource;
};

/**
 * @ingroup iface_poor_empty
 * @struct poor_empty_interface
 */
struct poor_empty_interface {
	/**
	 */
	void (*empty)(struct poor_wl_client *client,
		      struct poor_empty_resource *resource);
};

/** @ingroup iface_poor_empty */
static inline int
poor_empty_dispatch_request(const void *implementation, void *target, uint32_t opcode,
			    const struct poor_wl_message *, union poor_wl_argument *)
{
	const struct poor_empty_interface *impl = (const struct poor_empty_interface *) implementation;
	struct poor_wl_resource *resource = (struct poor_wl_resource *) target;

	switch (opcode) {
	case 0:
		if (!impl->empty)
			return -1;
		impl->empty(resource->client, (struct poor_empty_resource *) resource);
		return 0;
	}
	return -1;
}

/** @ingroup iface_poor_empty */
static inline int
poor_empty_resource_init(struct poor_empty_resource *resource,
			 struct poor_wl_client *client, int version, uint32_t id)
{
	return poor_wl_resource_init(&resource->resource, client, &poor_empty_interface, version, id);
}

/** @ingroup iface_poor_empty */
static inline void
poor_empty_resource_set_implementation(struct poor_empty_resource *resource,
				       const struct poor_empty_interface *implementation, void *data,
				       poor_wl_resource_destroy_func_t destroy)
{
	poor_wl_resource_set_dispatcher(&resource->resource, poor_empty_dispatch_request, implementation, data, destroy);
}


/**
 * @ingroup iface_poor_empty
 */
#define POOR_EMPTY_EMPTY_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
