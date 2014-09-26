/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef ARMADA_ACCEL_H
#define ARMADA_ACCEL_H

#include "xf86.h"
#include "xf86xv.h"
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common_drm.h"

struct drm_armada_bufmgr;

struct armada_accel_ops {
	Bool (*pre_init)(ScrnInfoPtr, int);
	int (*screen_init)(ScreenPtr, struct drm_armada_bufmgr *);
	Bool (*import_dmabuf)(ScreenPtr, PixmapPtr, int);
	void (*set_pixmap_bo)(PixmapPtr, struct drm_armada_bo *);
	void (*free_pixmap)(PixmapPtr);
	void (*vblank_handler)(int fd, unsigned int sequence,
			       unsigned int tv_sec, unsigned int tv_usec,
			       void *user_data);
	XF86VideoAdaptorPtr (*xv_init)(ScreenPtr);
};

extern Bool accel_module_init(const struct armada_accel_ops **);

#endif
