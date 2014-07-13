/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef DOVEFB_DRM_H
#define DOVEFB_DRM_H

#include "xf86.h"
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common_drm.h"

struct drm_armada_bo;
struct drm_armada_bufmgr;

struct armada_drm_info {
	OptionInfoPtr Options;
	CloseScreenProcPtr CloseScreen;
	CreateScreenResourcesProcPtr CreateScreenResources;
	drmVersionPtr version;
	struct drm_armada_bufmgr *bufmgr;
	struct drm_armada_bo *front_bo;
	const struct armada_accel_ops *accel_ops;
	void *accel_module;
	Bool accel;
	unsigned cpp;
};

struct all_drm_info {
	struct common_drm_info common;
	struct armada_drm_info armada;
};

#define GET_ARMADA_DRM_INFO(pScrn) \
	((struct armada_drm_info *)GET_DRM_INFO(pScrn)->private)

/* DRM core support */
Bool armada_drm_init_screen(ScrnInfoPtr pScrn);

/* DRM Xv support */
Bool armada_drm_XvInit(ScrnInfoPtr pScrn);

#endif
