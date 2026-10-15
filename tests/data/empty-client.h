/* SCANNER TEST */

#ifndef POOR_EMPTY_CLIENT_PROTOCOL_H
#define POOR_EMPTY_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "poor_wayland_client.h"

#ifdef  __cplusplus
extern "C" {
#endif

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
 * @struct poor_empty
 */
struct poor_empty {
	struct poor_wl_proxy proxy;
};

#define POOR_EMPTY_EMPTY 0


/**
 * @ingroup iface_poor_empty
 */
#define POOR_EMPTY_EMPTY_SINCE_VERSION 1

/** @ingroup iface_poor_empty */
static inline void
poor_empty_set_user_data(struct poor_empty *poor_empty, void *user_data)
{
	poor_wl_proxy_set_user_data((struct poor_wl_proxy *) poor_empty, user_data);
}

/** @ingroup iface_poor_empty */
static inline void *
poor_empty_get_user_data(struct poor_empty *poor_empty)
{
	return poor_wl_proxy_get_user_data((struct poor_wl_proxy *) poor_empty);
}

static inline uint32_t
poor_empty_get_version(struct poor_empty *poor_empty)
{
	return poor_wl_proxy_get_version((struct poor_wl_proxy *) poor_empty);
}

/** @ingroup iface_poor_empty */
static inline void
poor_empty_destroy(struct poor_empty *poor_empty)
{
	poor_wl_proxy_destroy((struct poor_wl_proxy *) poor_empty);
}

/**
 * @ingroup iface_poor_empty
 */
static inline void
poor_empty_empty(struct poor_empty *poor_empty)
{
	poor_wl_proxy_marshal_flags((struct poor_wl_proxy *) poor_empty,
			 POOR_EMPTY_EMPTY, NULL, poor_wl_proxy_get_version((struct poor_wl_proxy *) poor_empty), 0);
}

#ifdef  __cplusplus
}
#endif

#endif
