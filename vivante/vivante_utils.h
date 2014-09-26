/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_UTILS_H
#define VIVANTE_UTILS_H

/* Needed for gcsRECT */
#include <gc_hal_base.h>

struct drm_armada_bo;
struct vivante;
struct vivante_pixmap;

const char *vivante_strerror(int err);
#define vivante_error(v,w,e) __vivante_error(v,__func__,w,e)
void __vivante_error(struct vivante *, const char *, const char *, int);

Bool vivante_map_dmabuf(struct vivante *vivante, int fd,
	struct vivante_pixmap *vPix);
Bool vivante_map_bo_to_gpu(struct vivante *vivante, struct drm_armada_bo *bo,
	void **info, uint32_t *handle);
void vivante_unmap_from_gpu(struct vivante *vivante, void *info,
	uint32_t handle);

void vivante_unmap_gpu(struct vivante *vivante, struct vivante_pixmap *vPix);
Bool vivante_map_gpu(struct vivante *vivante, struct vivante_pixmap *vPix);

gceSURF_FORMAT vivante_pict_format(PictFormatShort format, Bool force);
Bool vivante_format_valid(struct vivante *vivante, gceSURF_FORMAT fmt);

/*
 * The following functions are here to allow the compiler to inline them
 * if it so wishes; otherwise it will include a copy of these functions
 * in each object including this file.
 */

/* Translate the box by off_x, off_y, and convert to a vivante rectangle */
static inline void RectBox(gcsRECT_PTR rect, const BoxRec *box, int off_x, int off_y)
{
	rect->left   = box->x1 + off_x;
	rect->top    = box->y1 + off_y;
	rect->right  = box->x2 + off_x;
	rect->bottom = box->y2 + off_y;
}

void dump_Drawable(DrawablePtr pDraw, const char *, ...)
	__attribute__((__format__(__printf__, 2, 3)));
void dump_Picture(PicturePtr pDst, const char *, ...)
	__attribute__((__format__(__printf__, 2, 3)));
void dump_vPix(struct vivante *vivante, struct vivante_pixmap *vPix,
	int alpha, const char *fmt, ...)
	__attribute__((__format__(__printf__, 4, 5)));

#endif
