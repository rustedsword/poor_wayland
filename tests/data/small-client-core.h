/* SCANNER TEST */

#ifndef POOR_SMALL_TEST_CLIENT_PROTOCOL_H
#define POOR_SMALL_TEST_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "poor_wayland_client_core.h"

#ifdef  __cplusplus
extern "C" {
#endif

/**
 * @page page_poor_small_test The poor_small_test protocol
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
struct poor_another_intf;
struct poor_intf_A;
struct poor_intf_not_here;

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

/**
 * @ingroup iface_poor_intf_A
 * @struct poor_intf_A
 */
struct poor_intf_A {
	struct poor_wl_proxy proxy;
};

/**
 * @ingroup iface_poor_intf_A
 * @struct poor_intf_A_listener
 */
struct poor_intf_A_listener {
	/**
	 */
	void (*hey)(void *data,
		    struct poor_intf_A *poor_intf_A);
	/**
	 * @since 2
	 * @deprecated Deprecated since version 3
	 */
	void (*yo)(void *data,
		   struct poor_intf_A *poor_intf_A);
};

/**
 * @ingroup iface_poor_intf_A
 */
static inline int
poor_intf_A_add_listener(struct poor_intf_A *poor_intf_A,
			 const struct poor_intf_A_listener *listener, void *data)
{
	return poor_wl_proxy_add_listener((struct poor_wl_proxy *) poor_intf_A,
				     (void (**)(void)) listener, data);
}

#define POOR_INTF_A_RQ1 0
#define POOR_INTF_A_RQ2 1
#define POOR_INTF_A_DESTROY 2

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
#define POOR_INTF_A_RQ1_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_RQ2_SINCE_VERSION 1
/**
 * @ingroup iface_poor_intf_A
 */
#define POOR_INTF_A_DESTROY_SINCE_VERSION 1

/** @ingroup iface_poor_intf_A */
static inline void
poor_intf_A_set_user_data(struct poor_intf_A *poor_intf_A, void *user_data)
{
	poor_wl_proxy_set_user_data((struct poor_wl_proxy *) poor_intf_A, user_data);
}

/** @ingroup iface_poor_intf_A */
static inline void *
poor_intf_A_get_user_data(struct poor_intf_A *poor_intf_A)
{
	return poor_wl_proxy_get_user_data((struct poor_wl_proxy *) poor_intf_A);
}

static inline uint32_t
poor_intf_A_get_version(struct poor_intf_A *poor_intf_A)
{
	return poor_wl_proxy_get_version((struct poor_wl_proxy *) poor_intf_A);
}

/**
 * @ingroup iface_poor_intf_A
 */
static inline void *
poor_intf_A_rq1(struct poor_intf_A *poor_intf_A, const struct poor_wl_interface *interface, uint32_t version, void *untyped_new)
{
	return (void *) poor_wl_proxy_marshal_flags((struct poor_wl_proxy *) poor_intf_A,
			 POOR_INTF_A_RQ1, interface, version, 0, interface->name, version, untyped_new);
}

/**
 * @ingroup iface_poor_intf_A
 */
static inline struct poor_intf_not_here *
poor_intf_A_rq2(struct poor_intf_A *poor_intf_A, struct poor_intf_not_here *typed_new, const char *str, int32_t i, uint32_t u, poor_wl_fixed_t f, int32_t fd, struct poor_another_intf *obj)
{
	return (struct poor_intf_not_here *) poor_wl_proxy_marshal_flags((struct poor_wl_proxy *) poor_intf_A,
			 POOR_INTF_A_RQ2, &poor_intf_not_here_interface, poor_wl_proxy_get_version((struct poor_wl_proxy *) poor_intf_A), 0, typed_new, str, i, u, f, fd, obj);
}

/**
 * @ingroup iface_poor_intf_A
 */
static inline void
poor_intf_A_destroy(struct poor_intf_A *poor_intf_A)
{
	poor_wl_proxy_marshal_flags((struct poor_wl_proxy *) poor_intf_A,
			 POOR_INTF_A_DESTROY, NULL, poor_wl_proxy_get_version((struct poor_wl_proxy *) poor_intf_A), POOR_WL_MARSHAL_FLAG_DESTROY);
}

#ifdef  __cplusplus
}
#endif

#endif
