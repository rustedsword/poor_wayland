/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <assert.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <poor_loop_fd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysmacros.h>

#include "poor_wayland_swapchain.h"
#include "test-ring.h"
#include "test-runner.h"

constexpr uint64_t TIMEOUT_NS = 5'000'000'000;

/* Only there with LeakSanitizer */
[[gnu::weak]] void __lsan_disable(void);
[[gnu::weak]] void __lsan_enable(void);

static void watchdog_fire(struct poor_loop *, struct poor_loop_timer *)
{
	fprintf(stderr, "loop timed out\n");
	abort();
}

static struct poor_loop_timer watchdog = POOR_LOOP_TIMER_INIT(watchdog_fire);

#define run_until(loop, cond) \
	do { \
		while (!(cond)) \
			assert(!poor_loop_run_once(loop)); \
	} while (0)

static void loop_init(struct poor_loop *loop)
{
	struct io_uring_params params = {};

	test_ring_params(&params);
	assert(!poor_loop_init(loop, 64, &params));
	poor_loop_timer_arm(loop, &watchdog, poor_loop_now() + TIMEOUT_NS);
}

static void loop_fini(struct poor_loop *loop)
{
	poor_loop_timer_disarm(&watchdog);
	assert(!poor_loop_drain(loop, poor_loop_now() + TIMEOUT_NS));
	poor_loop_exit(loop);
}

static bool gbm_ready_called, gbm_done_called;

static void gbm_ready(struct poor_wl_gbm *)
{
	gbm_ready_called = true;
}

static void gbm_done(struct poor_wl_gbm *)
{
	gbm_done_called = true;
}

/* Without a GPU, as in CI, the devices are not there */
TEST(gbm_device)
{
	const dev_t devices[] = { makedev(226, 0), makedev(226, 128) };
	static const uint64_t implicit = DRM_FORMAT_MOD_INVALID;
	struct poor_wl_swapchain_buffer buffer;
	struct poor_wl_gbm gbm;
	struct poor_loop loop;

	loop_init(&loop);
	assert(poor_wl_gbm_open(&gbm, &loop, makedev(1, 3), gbm_ready, gbm_done) == -1 && errno == ENODEV);
	for (unsigned i = 0; i < ARRAY_SIZE(devices); i++) {
		gbm_ready_called = gbm_done_called = false;
		/* Mesa's driver leaks while GBM loads it, and is unloaded before the leaks could be told apart */
		if (__lsan_disable)
			__lsan_disable();
		assert(!poor_wl_gbm_open(&gbm, &loop, devices[i], gbm_ready, gbm_done));
		run_until(&loop, gbm_ready_called);
		if (__lsan_enable)
			__lsan_enable();
		assert(!gbm.device != !gbm.error);
		if (gbm.device) {
			buffer.width = buffer.height = 64;
			assert(!poor_wl_gbm_allocator.allocate(&gbm, &buffer, DRM_FORMAT_XRGB8888, &implicit, 1, false));
			assert(buffer.modifier == DRM_FORMAT_MOD_INVALID && buffer.plane_count >= 1 && buffer.planes[0].stride >= 64 * 4);
			for (unsigned j = 0; j < buffer.plane_count; j++)
				poor_loop_close_fd_or_submit(&loop, buffer.planes[j].fd, 0);
			poor_wl_gbm_allocator.free(&gbm, &buffer);
		}
		poor_wl_gbm_close(&gbm);
		run_until(&loop, gbm_done_called);
	}
	loop_fini(&loop);
}
