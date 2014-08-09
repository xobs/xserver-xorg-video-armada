#ifndef XV_IMAGE_FORMAT_H
#define XV_IMAGE_FORMAT_H

#include "xf86xv.h"

struct xv_image_format {
	union {
		uint32_t drm_format;
	} u;
	XF86ImageRec	xv_image;
};

const struct xv_image_format *xv_image_xvfourcc(const struct xv_image_format *,
	size_t, int);
const struct xv_image_format *xv_image_drm(const struct xv_image_format *,
	size_t, uint32_t);

#endif
