#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xv_image_format.h"

const struct xv_image_format *xv_image_xvfourcc(
	const struct xv_image_format *tbl, size_t nent, int fmt)
{
	size_t i;

	for (i = 0; i < nent; i++)
		if (tbl[i].xv_image.id == fmt)
			return &tbl[i];

	return NULL;
}

const struct xv_image_format *xv_image_drm(
	const struct xv_image_format *tbl, size_t nent, uint32_t fmt)
{
	size_t i;

	for (i = 0; i < nent; i++)
		if (tbl[i].u.drm_format == fmt)
			return &tbl[i];

	return NULL;
}
