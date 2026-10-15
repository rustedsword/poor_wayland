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
struct poor_empty;

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
 * @struct poor_empty_interface
 */
struct poor_empty_interface {
	/**
	 */
	void (*empty)(struct poor_wl_client *client,
		      struct poor_wl_resource *resource);
};


/**
 * @ingroup iface_poor_empty
 */
#define POOR_EMPTY_EMPTY_SINCE_VERSION 1

#ifdef  __cplusplus
}
#endif

#endif
