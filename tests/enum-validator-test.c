#define _GNU_SOURCE

#include <assert.h>
#include "data/small-server-core.h"

int
main(int argc, char *argv[]) {
	assert(poor_intf_A_foo_is_valid(POOR_INTF_A_FOO_FIRST, 1));
	assert(poor_intf_A_foo_is_valid(POOR_INTF_A_FOO_FIRST, 2));

	assert(!poor_intf_A_foo_is_valid(POOR_INTF_A_FOO_THIRD, 1));
	assert(poor_intf_A_foo_is_valid(POOR_INTF_A_FOO_THIRD, 2));

	assert(poor_intf_A_foo_is_valid(POOR_INTF_A_FOO_NEGATIVE, 2));

	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST, 1));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST, 2));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_SECOND, 1));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_SECOND, 2));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST | POOR_INTF_A_BAR_SECOND, 1));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST | POOR_INTF_A_BAR_SECOND, 2));

	assert(!poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_THIRD, 1));
	assert(!poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST | POOR_INTF_A_BAR_THIRD, 1));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_THIRD, 2));
	assert(poor_intf_A_bar_is_valid(POOR_INTF_A_BAR_FIRST | POOR_INTF_A_BAR_THIRD, 2));

	assert(!poor_intf_A_bar_is_valid(0xFF, 1));
	assert(!poor_intf_A_bar_is_valid(0xFF, 2));
}
