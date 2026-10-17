/* SCANNER TEST */

#ifndef POOR_SMALL_TEST_SERVER_PROTOCOL_H
#define POOR_SMALL_TEST_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "poor_wayland_server.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct poor_wl_client;
struct poor_wl_resource;

/**
 * @page page_poor_small_test The poor_small_test protocol
 * a protocol for the scanner tests
 *
 * @section page_desc_poor_small_test Description
 *
 * It has every kind of argument.
 *
 * @section page_ifaces_poor_small_test Interfaces
 * - @subpage page_iface_poor_intf_A - the thing A
 * @section page_copyright_poor_small_test Copyright
 * <pre>
 *
 * Copyright © 2016 Collabora, Ltd.
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
 * </pre>
 */
struct poor_another_intf_resource;
struct poor_intf_A_resource;
struct poor_intf_not_here_resource;

#ifndef POOR_INTF_A_INTERFACE
#define POOR_INTF_A_INTERFACE
/**
 * @page page_iface_poor_intf_A poor_intf_A
 * @section page_iface_poor_intf_A_desc Description
 *
 * A useless example trying to tickle the scanner.
 * @section page_iface_poor_intf_A_api API
 * See @ref iface_poor_intf_A.
 */
/**
 * @defgroup iface_poor_intf_A The poor_intf_A interface
 *
 * A useless example trying to tickle the scanner.
 */
extern const struct poor_wl_interface poor_intf_A_interface;
#endif

#ifndef POOR_INTF_A_FOO_ENUM
#define POOR_INTF_A_FOO_ENUM
enum poor_intf_A_foo {
	/**
	 * this is the first
	 */
	POOR_INTF_A_FOO_FIRST = 0,
	/**
	 * this is the second
	 */
	POOR_INTF_A_FOO_SECOND = 1,
	/**
	 * this is the third
	 * @since 2
	 */
	POOR_INTF_A_FOO_THIRD = 2,
	/**
	 * this is a negative value
	 * @since 2
	 */
	POOR_INTF_A_FOO_NEGATIVE = -1,
	/**
	 * this is a deprecated value
	 * @since 2
	 * @deprecated Deprecated since version 3
	 */
	POOR_INTF_A_FOO_DEPRECATED = 3,
};
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_FOO_THIRD_SINCE_VERSION 2
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_FOO_NEGATIVE_SINCE_VERSION 2
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_FOO_DEPRECATED_SINCE_VERSION 2
#endif /* POOR_INTF_A_FOO_ENUM */

#ifndef POOR_INTF_A_FOO_ENUM_IS_VALID
#define POOR_INTF_A_FOO_ENUM_IS_VALID
/**
 * @ingroup iface_poor_intf_A
 * Validate a poor_intf_A foo value.
 *
 * @return true on success, false on error.
 * @ref poor_intf_A_foo
 */
static inline bool
poor_intf_A_foo_is_valid(uint32_t value, uint32_t version) {
	switch (value) {
	case POOR_INTF_A_FOO_FIRST:
		return version >= 1;
	case POOR_INTF_A_FOO_SECOND:
		return version >= 1;
	case POOR_INTF_A_FOO_THIRD:
		return version >= 2;
	case (uint32_t)POOR_INTF_A_FOO_NEGATIVE:
		return version >= 2;
	case POOR_INTF_A_FOO_DEPRECATED:
		return version >= 2;
	default:
		return false;
	}
}
#endif /* POOR_INTF_A_FOO_ENUM_IS_VALID */

#ifndef POOR_INTF_A_BAR_ENUM
#define POOR_INTF_A_BAR_ENUM
enum poor_intf_A_bar {
	/**
	 * this is the first
	 */
	POOR_INTF_A_BAR_FIRST = 0x01,
	/**
	 * this is the second
	 */
	POOR_INTF_A_BAR_SECOND = 0x02,
	/**
	 * this is the third
	 * @since 2
	 */
	POOR_INTF_A_BAR_THIRD = 0x04,
};
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_BAR_THIRD_SINCE_VERSION 2
#endif /* POOR_INTF_A_BAR_ENUM */

#ifndef POOR_INTF_A_BAR_ENUM_IS_VALID
#define POOR_INTF_A_BAR_ENUM_IS_VALID
/**
 * @ingroup iface_poor_intf_A
 * Validate a poor_intf_A bar value.
 *
 * @return true on success, false on error.
 * @ref poor_intf_A_bar
 */
static inline bool
poor_intf_A_bar_is_valid(uint32_t value, uint32_t version) {
	uint32_t valid = 0;
	if (version >= 1)
		valid |= POOR_INTF_A_BAR_FIRST;
	if (version >= 1)
		valid |= POOR_INTF_A_BAR_SECOND;
	if (version >= 2)
		valid |= POOR_INTF_A_BAR_THIRD;
	return (value & ~valid) == 0;
}
#endif /* POOR_INTF_A_BAR_ENUM_IS_VALID */

/**
 * @ingroup iface_poor_intf_A
 * @struct poor_intf_A_resource
 */
struct poor_intf_A_resource {
	struct poor_wl_resource resource;
};

/**
 * @ingroup iface_poor_intf_A
 * @struct poor_intf_A_interface
 */
struct poor_intf_A_interface {
	/**
	 * @param interface name of the objects interface
	 * @param version version of the objects interface
	 */
	void (*rq1)(struct poor_wl_client *client,
		    struct poor_intf_A_resource *resource,
		    const char *interface, uint32_t version, uint32_t untyped_new);
	/**
	 */
	void (*rq2)(struct poor_wl_client *client,
		    struct poor_intf_A_resource *resource,
		    uint32_t typed_new,
		    const char *str,
		    int32_t i,
		    uint32_t u,
		    poor_wl_fixed_t f,
		    int32_t fd,
		    struct poor_another_intf_resource *obj);
	/**
	 */
	void (*destroy)(struct poor_wl_client *client,
			struct poor_intf_A_resource *resource);
	/**
	 * @param interface name of the objects interface
	 * @param version version of the objects interface
	 * @since 2
	 */
	void (*rq3)(struct poor_wl_client *client,
		    struct poor_intf_A_resource *resource,
		    const char *interface, uint32_t version, uint32_t untyped_new,
		    struct poor_another_intf_resource *obj);
	/**
	 * @since 2
	 */
	void (*rq4)(struct poor_wl_client *client,
		    struct poor_intf_A_resource *resource,
		    struct poor_wl_resource *any);
};

/** @ingroup iface_poor_intf_A */
static inline int
poor_intf_A_dispatch_request(const void *implementation, void *target, uint32_t opcode,
			     const struct poor_wl_message *, union poor_wl_argument *args)
{
	const struct poor_intf_A_interface *impl = (const struct poor_intf_A_interface *) implementation;
	struct poor_wl_resource *resource = (struct poor_wl_resource *) target;

	switch (opcode) {
	case 0:
		if (!impl->rq1)
			return -1;
		impl->rq1(resource->client, (struct poor_intf_A_resource *) resource, args[0].s, args[1].u, args[2].n);
		return 0;
	case 1:
		if (!impl->rq2)
			return -1;
		impl->rq2(resource->client, (struct poor_intf_A_resource *) resource, args[0].n, args[1].s, args[2].i, args[3].u, args[4].f, args[5].h, (struct poor_another_intf_resource *) args[6].o);
		return 0;
	case 2:
		if (!impl->destroy)
			return -1;
		impl->destroy(resource->client, (struct poor_intf_A_resource *) resource);
		return 0;
	case 3:
		if (!impl->rq3)
			return -1;
		impl->rq3(resource->client, (struct poor_intf_A_resource *) resource, args[0].s, args[1].u, args[2].n, (struct poor_another_intf_resource *) args[3].o);
		return 0;
	case 4:
		if (!impl->rq4)
			return -1;
		impl->rq4(resource->client, (struct poor_intf_A_resource *) resource, (struct poor_wl_resource *) args[0].o);
		return 0;
	}
	return -1;
}

/** @ingroup iface_poor_intf_A */
static inline int
poor_intf_A_resource_init(struct poor_intf_A_resource *resource,
			  struct poor_wl_client *client, int version, uint32_t id)
{
	return poor_wl_resource_init(&resource->resource, client, &poor_intf_A_interface, version, id);
}

/** @ingroup iface_poor_intf_A */
static inline void
poor_intf_A_resource_set_implementation(struct poor_intf_A_resource *resource,
					const struct poor_intf_A_interface *implementation, void *data,
					poor_wl_resource_destroy_func_t destroy)
{
	poor_wl_resource_set_dispatcher(&resource->resource, poor_intf_A_dispatch_request, implementation, data, destroy);
}

#define POOR_INTF_A_HEY 0
#define POOR_INTF_A_YO 1
#define POOR_INTF_A_ANY_OBJECT 2

/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_HEY_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_YO_SINCE_VERSION 2
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_ANY_OBJECT_SINCE_VERSION 3

/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_RQ1_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_RQ2_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_DESTROY_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_RQ3_SINCE_VERSION 2
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_RQ4_SINCE_VERSION 2

/**
 * @ingroup iface_poor_intf_A
 * Sends an hey event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
poor_intf_A_send_hey(struct poor_intf_A_resource *resource_)
{
	poor_wl_resource_post_event(&resource_->resource, POOR_INTF_A_HEY);
}

/**
 * @ingroup iface_poor_intf_A
 * Sends an yo event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
poor_intf_A_send_yo(struct poor_intf_A_resource *resource_)
{
	poor_wl_resource_post_event(&resource_->resource, POOR_INTF_A_YO);
}

/**
 * @ingroup iface_poor_intf_A
 * Sends an any_object event to the client owning the resource.
 * @param resource_ The client's resource
 */
static inline void
poor_intf_A_send_any_object(struct poor_intf_A_resource *resource_, struct poor_wl_resource *any)
{
	poor_wl_resource_post_event(&resource_->resource, POOR_INTF_A_ANY_OBJECT, any);
}

#ifdef  __cplusplus
}
#endif

#endif
