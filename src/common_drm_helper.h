#ifndef COMMON_DRM_HELPER_H
#define COMMON_DRM_HELPER_H

#include "xf86.h"
#include "xf86Crtc.h"
#include <xf86drm.h>

xf86CrtcPtr common_drm_covering_crtc(ScrnInfoPtr pScrn, BoxPtr box,
	xf86CrtcPtr desired, BoxPtr box_ret);

#endif
