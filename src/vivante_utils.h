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

struct vivante;
struct vivante_pixmap;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

const char *vivante_strerror(int err);
#define vivante_error(v,w,e) __vivante_error(v,__func__,w,e)
void __vivante_error(struct vivante *, const char *, const char *, int);

static inline PixmapPtr vivante_drawable_pixmap(DrawablePtr pDrawable)
{
	if (OnScreenDrawable(pDrawable->type)) {
		WindowPtr pWin = container_of(pDrawable, struct _Window, drawable);

		return pDrawable->pScreen->GetWindowPixmap(pWin);
	} else {
		return container_of(pDrawable, struct _Pixmap, drawable);
	}
}

PixmapPtr vivante_drawable_pixmap_deltas(DrawablePtr pDrawable, int *x, int *y);
void vivante_unmap_gpu(struct vivante *vivante, struct vivante_pixmap *vPix);
Bool vivante_map_gpu(struct vivante *vivante, struct vivante_pixmap *vPix);

enum {
	ACCESS_RO,
	ACCESS_RW,
};

void vivante_finish_drawable(DrawablePtr pDrawable, int access);
void vivante_prepare_drawable(DrawablePtr pDrawable, int access);

gceSURF_FORMAT vivante_format(PictFormatShort format, Bool force);

/*
 * The following functions are here to allow the compiler to inline them
 * if it so wishes; otherwise it will include a copy of these functions
 * in each object including this file.
 */

/*
 * Calculate the intersection of two boxes.  Returns TRUE if
 * they do not overlap.
 */
static inline Bool BoxClip(BoxPtr out, const BoxRec *in, const BoxRec *clip)
{
   out->x1 = max(in->x1, clip->x1);
   out->y1 = max(in->y1, clip->y1);
   out->x2 = min(in->x2, clip->x2);
   out->y2 = min(in->y2, clip->y2);
   return out->x1 >= out->x2 || out->y1 >= out->y2;
}

/* Translate the box by off_x, off_y, and convert to a vivante rectangle */
static inline void RectBox(gcsRECT_PTR rect, const BoxRec *box, int off_x, int off_y)
{
	rect->left   = box->x1 + off_x;
	rect->top    = box->y1 + off_y;
	rect->right  = box->x2 + off_x;
	rect->bottom = box->y2 + off_y;
}

void dump_Drawable(DrawablePtr pDraw, const char *, ...);
void dump_Picture(PicturePtr pDst, const char *, ...);
void dump_vPix(struct vivante *vivante, struct vivante_pixmap *vPix,
	int alpha, const char *fmt, ...);

#endif
