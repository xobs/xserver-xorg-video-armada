/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 *
 * Notes:
 *  * For a window, the drawable inside the window structure has an
 *    x and y position for the underlying pixmap.
 *  * Composite clips have the drawable position already included.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include "boxutil.h"
#include "pixmaputil.h"
#include "unaccel.h"

#include "vivante_accel.h"
#include "vivante_utils.h"
#include "utils.h"

#include <gc_hal_raster.h>
#include <gc_hal_enum.h>
#include <gc_hal.h>

static inline uint32_t scale16(uint32_t val, int bits)
{
	val <<= (16 - bits);
	while (bits < 16) {
		val |= val >> bits;
		bits <<= 1;
	}
	return val >> 8;
}

static void vivante_set_blend(struct vivante *vivante,
	const struct vivante_blend_op *blend)
{
#ifdef RENDER
	gceSTATUS err;

	if (blend) {
		err = gco2D_EnableAlphaBlend(vivante->e2d,
			blend->src_alpha,
			blend->dst_alpha,
			gcvSURF_PIXEL_ALPHA_STRAIGHT,
			gcvSURF_PIXEL_ALPHA_STRAIGHT,
			blend->src_global_alpha,
			blend->dst_global_alpha,
			blend->src_blend,
			blend->dst_blend,
			gcvSURF_COLOR_STRAIGHT,
			gcvSURF_COLOR_STRAIGHT);
		if (err != gcvSTATUS_OK)
			vivante_error(vivante, "gco2D_EnableAlphaBlend", err);

		vivante->alpha_blend_enabled = TRUE;
	} else if (vivante->alpha_blend_enabled) {
		vivante->alpha_blend_enabled = FALSE;

		err = gco2D_DisableAlphaBlend(vivante->e2d);
		if (err)
			vivante_error(vivante, "DisableAlphaBlend", err);
	}
#endif
}



#ifdef VIVANTE_BATCH
struct vivante_batch {
	struct xorg_list node;
	struct xorg_list head;
	uint32_t index;
	int32_t serial;
	int32_t *current;
};

static void vivante_batch_destroy(struct vivante_batch *batch)
{
	struct vivante_pixmap *vp, *vn;

	/* Unlink all pixmaps that this batch is connected to */
	xorg_list_for_each_entry_safe(vp, vn, &batch->head, batch_node) {
		vp->batch = NULL;
		xorg_list_del(&vp->batch_node);
	}

	xorg_list_del(&batch->node);
	free(batch);
}

static void vivante_batch_reap(struct vivante *vivante)
{
	struct vivante_batch *batch, *n;

	xorg_list_for_each_entry_safe(batch, n, &vivante->batch_list, node) {
		if (*batch->current == batch->serial) {
#ifdef DEBUG_BATCH
			dbg("batch %p: reaping at %08x\n",
			    batch, *batch->current);
#endif
			vivante_batch_destroy(batch);
		}
	}
}

static void __vivante_batch_wait(struct vivante_batch *batch)
{
#ifdef DEBUG_BATCH
	dbg("batch %p: waiting: %08x %08x\n",
	    batch, *batch->current, batch->serial);
#endif
	while (*batch->current != batch->serial)
		usleep(5);
	vivante_batch_destroy(batch);
}

/*
 * If the pixmap is part of a batch which is not the current batch,
 * wait for its batch to indicate operations are complete on it.
 */
static void
vivante_batch_wait(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	struct vivante_batch *batch = vPix->batch;

	if (batch && batch != vivante->batch)
		__vivante_batch_wait(batch);
}

/*
 * Issue and wait for all outstanding GPU activity for this pixmap to
 * complete.  Essentially, that means if this pixmap is attached to a
 * batch, it is busy, and if the batch is the current batch, we need
 * to commit the current batch of operations.
 */
void
vivante_batch_wait_commit(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	struct vivante_batch *batch = vPix->batch;

	if (batch) {
		if (batch == vivante->batch)
			vivante_commit(vivante, TRUE);
		__vivante_batch_wait(batch);
	}
}

static Bool vivante_batch_new(struct vivante *vivante)
{
	struct vivante_batch *batch;
	int32_t serial;

	vivante_batch_reap(vivante);

	serial = vivante->batch_serial + 1;
	if (serial <= 0)
		serial = 1;
	vivante->batch_serial = serial;

	batch = malloc(sizeof *batch);
	if (batch) {
		uint16_t i = vivante->batch_idx;

		batch->index = i;
		batch->serial = serial;
		batch->current = vivante->batch_ptr + i;
		*batch->current = -1;
		xorg_list_init(&batch->head);

		i += 1;
		if (i >= vivante->batch_idx_max)
			i = 0;

		vivante->batch_idx = i;
	}

	vivante->batch = batch;

	return batch ? TRUE : FALSE;
}

/* Add the pixmap to the current batch, if not already added */
static void
vivante_batch_add(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	struct vivante_batch *batch = vPix->batch;

	if (!batch) {
		vPix->batch = batch = vivante->batch;
		xorg_list_add(&vPix->batch_node, &batch->head);
#ifdef DEBUG_BATCH
		dbg("Allocated batch %p for vPix %p\n", batch, vPix);
#endif
		vivante->need_commit = TRUE;
	}
	assert(vPix->batch == vivante->batch);
}

/* Add the batch to the GPU operations right at the very end of the GPU ops */
static void vivante_batch_commit(struct vivante *vivante)
{
	struct vivante_batch *batch = vivante->batch;
	uint32_t col = batch->serial;
	uint32_t handle = vivante->batch_handle;
	gceSTATUS err;
	gcsRECT rect;

#define BATCH_PITCH 64
#define BATCH_WIDTH (BATCH_PITCH / sizeof(uint32_t))

	rect.left = batch->index & (BATCH_WIDTH - 1);
	rect.top = batch->index / BATCH_WIDTH;
	rect.right = rect.left + 1;
	rect.bottom = rect.top + 1;

#ifdef DEBUG_BATCH
	dbg("batch %p: current %08x next %08x handle %08x index %04x rect [%u,%u,%u,%u]\n",
	    batch, *batch->current, col, handle, batch->index,
	    rect.left, rect.top, rect.right, rect.bottom);
#endif

	vivante_set_blend(vivante, NULL);

	err = gco2D_LoadSolidBrush(vivante->e2d, gcvSURF_A8R8G8B8, 0, col, ~0ULL);
	if (err != gcvSTATUS_OK)
		goto error;

	err = gco2D_SetClipping(vivante->e2d, &rect);
	if (err != gcvSTATUS_OK)
		goto error;

	err = gco2D_SetTarget(vivante->e2d, handle, BATCH_PITCH,
			      gcvSURF_0_DEGREE, 0);
	if (err != gcvSTATUS_OK)
		goto error;

	err = gco2D_Blit(vivante->e2d, 1, &rect, 0xf0, 0xf0, gcvSURF_A8R8G8B8);
	if (err != gcvSTATUS_OK)
		goto error;

	xorg_list_append(&batch->node, &vivante->batch_list);
	vivante->batch = NULL;
	return;

 error:
	vivante_error(vivante, "batch blit", err);
}
#else
void
vivante_batch_wait_commit(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	if (vPix->need_stall && vivante->need_stall) {
		vivante_commit(vivante, TRUE);
		vivante->need_stall = FALSE;
	}
}

static void
vivante_batch_add(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	vivante->need_stall = TRUE;
	vivante->need_commit = TRUE;
	vPix->need_stall = TRUE;
}
#endif


static Bool
gal_prepare_gpu(struct vivante *vivante, struct vivante_pixmap *vPix)
{
#ifdef DEBUG_CHECK_DRAWABLE_USE
	if (vPix->in_use) {
		fprintf(stderr, "Trying to accelerate: %p %p %u\n",
				vPix, vPix->bo, vPix->in_use);
		return FALSE;
	}
#endif

#ifdef VIVANTE_BATCH
	/*
	 * If we don't have a batch already in place, then add one now.
	 * This gives us a chance to error out and fallback to CPU based
	 * blit if this allocation fails.
	 */
	if (!vivante->batch && !vivante_batch_new(vivante)) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "[vivante] %s failed\n", "batch allocation");
		return FALSE;
	}

	vivante_batch_wait(vivante, vPix);
#endif

	if (!vivante_map_gpu(vivante, vPix))
		return FALSE;

	/*
	 * This should never happen - if it does, and we proceeed, we will
	 * take the machine out, so assert and kill ourselves instead.
	 */
	assert(vPix->handle != 0 && vPix->handle != -1);

	return TRUE;
}

static void vivante_flush(struct vivante *vivante)
{
	gceSTATUS err = gco2D_Flush(vivante->e2d);

	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Flush", err);
}

static void vivante_blit_complete(struct vivante *vivante)
{
	vivante_flush(vivante);
}

static void vivante_load_dst(struct vivante *vivante,
	struct vivante_pixmap *vPix)
{
	gceSTATUS err;

	vivante_batch_add(vivante, vPix);
	err = gco2D_SetTarget(vivante->e2d, vPix->handle, vPix->pitch,
			      gcvSURF_0_DEGREE, 0);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "gco2D_SetTarget", err);
}

static void vivante_load_src(struct vivante *vivante,
	struct vivante_pixmap *vPix, gceSURF_FORMAT fmt, const xPoint *offset)
{
	gceSTATUS err;

	vivante_batch_add(vivante, vPix);
	err = gco2D_SetColorSourceAdvanced(vivante->e2d, vPix->handle,
				  vPix->pitch, fmt, gcvSURF_0_DEGREE,
				  vPix->width, vPix->height, !!offset);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "gco2D_SetColourSourceAdvanced", err);

	if (offset) {
		gcsRECT src_rect = {
			.left = offset->x,
			.top = offset->y,
			.right = offset->x + 1,
			.bottom = offset->y + 1,
		};

		err = gco2D_SetSource(vivante->e2d, &src_rect);
		if (err != gcvSTATUS_OK)
			vivante_error(vivante, "gco2D_SetSource", err);
	}
}

void vivante_commit(struct vivante *vivante, Bool stall)
{
	gceSTATUS err;

#ifdef VIVANTE_BATCH
	if (vivante->batch)
		vivante_batch_commit(vivante);
#endif

	vivante_flush(vivante);

	err = gcoHAL_Commit(vivante->hal, stall ? gcvTRUE : gcvFALSE);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Commit", err);

	vivante->need_commit = FALSE;
}





/*
 * All operations must respect clips and planemask
 * Colors: fgcolor and bgcolor are indexes into the colormap
 * PolyLine, PolySegment, PolyRect, PolyArc:
 *   line width (pixels, 0=1pix), line style, cap style, join style
 * FillPolygon, PolyFillRect, PolyFillArc:
 *   fill rule, fill style
 * fill style:
 *   a solid foreground color, a transparent stipple,a n opaque stipple,
 *   or a tile.
 *   Stipples are bitmaps where the 1 bits represent that the foreground
 *   color is written, and 0 bits represent that either the pixel is left
 *   alone (transparent) or that the background color is written (opaque).
 *   A tile is a pixmap of the full depth of the GC that is applied in its
 *   full glory to all areas.
 *
 *   The stipple and tile patterns can be any rectangular size, although
 *   some implementations will be faster for certain sizes such as 8x8
 *   or 32x32.
 *
 *
 * 0 = Black,      1 = !src & !dst, 2 = !src &  dst, 3 = !src
 * 4 = src & !dst, 5 = !dst,        6 =  src ^  dst, 7 = !src | !dst
 * 8 = src &  dst, 9 = !src ^  dst, a =  dst,        b = !src |  dst
 * c = src,        d =  src | !dst, e =  src |  dst, f = White
 *
 * high nibble: brush color bit is 1
 * low nibble:  brush color bit is 0
 *
 * fgrop: used when mask bit is 1
 * bgrop: used when mask bit is 0
 * mask (in brush): is an 8x8 mask: LSB is top line, LS bit righthand-most
 */
static const gctUINT8 vivante_fill_rop[] = {
	/* GXclear        */  0x00,		// ROP_BLACK,
	/* GXand          */  0xa0,		// ROP_BRUSH_AND_DST,
	/* GXandReverse   */  0x50,		// ROP_BRUSH_AND_NOT_DST,
	/* GXcopy         */  0xf0,		// ROP_BRUSH,
	/* GXandInverted  */  0x0a,		// ROP_NOT_BRUSH_AND_DST,
	/* GXnoop         */  0xaa,		// ROP_DST,
	/* GXxor          */  0x5a,		// ROP_BRUSH_XOR_DST,
	/* GXor           */  0xfa,		// ROP_BRUSH_OR_DST,
	/* GXnor          */  0x05,		// ROP_NOT_BRUSH_AND_NOT_DST,
	/* GXequiv        */  0xa5,		// ROP_NOT_BRUSH_XOR_DST,
	/* GXinvert       */  0x55,		// ROP_NOT_DST,
	/* GXorReverse    */  0xf5,		// ROP_BRUSH_OR_NOT_DST,
	/* GXcopyInverted */  0x0f,		// ROP_NOT_BRUSH,
	/* GXorInverted   */  0xaf,		// ROP_NOT_BRUSH_OR_DST,
	/* GXnand         */  0x5f,		// ROP_NOT_BRUSH_OR_NOT_DST,
	/* GXset          */  0xff		// ROP_WHITE
};

static uint32_t vivante_fg_col(GCPtr pGC)
{
	if (pGC->fillStyle == FillTiled)
		return pGC->tileIsPixel ? pGC->tile.pixel :
			get_first_pixel(&pGC->tile.pixmap->drawable);
	else
		return pGC->fgPixel;
}

/*
 * Generic solid-like blit fill - takes a set of boxes, and fills them
 * according to the clips in the GC.
 */
static Bool vivante_fill(struct vivante *vivante, struct vivante_pixmap *vPix,
	GCPtr pGC, const BoxRec *clipBox, const BoxRec *pBox, unsigned nBox,
	xPoint dst_offset)
{
	const BoxRec *b;
	unsigned chunk;
	gceSTATUS err;
	gctUINT32 fg;
	gctUINT8 rop;
	gcsRECT *rects, *r, clip;

	chunk = vivante->max_rect_count;
	if (nBox < chunk)
		chunk = nBox;

	rects = malloc(chunk * sizeof *rects);
	if (!rects) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "[vivante] %s: %s failed\n", __FUNCTION__, "malloc rects");
		return FALSE;
	}

	vivante_load_dst(vivante, vPix);
	vivante_set_blend(vivante, NULL);

	RectBox(&clip, clipBox, dst_offset.x, dst_offset.y);
	err = gco2D_SetClipping(vivante->e2d, &clip);
	if (err) {
		vivante_error(vivante, "gco2D_SetClipping", err);
		free(rects);
		return FALSE;
	}

	fg = vivante_fg_col(pGC);
	err = gco2D_LoadSolidBrush(vivante->e2d, vPix->format, 0, fg, ~0ULL);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_LoadSolidBrush", err);
		free(rects);
		return FALSE;
	}

	rop = vivante_fill_rop[pGC->alu];
	b = pBox;
	while (nBox) {
		unsigned i;

		if (nBox < chunk)
			chunk = nBox;

		for (i = 0, r = rects; i < chunk; i++, r++, b++)
			RectBox(r, b, dst_offset.x, dst_offset.y);

		err = gco2D_Blit(vivante->e2d, chunk, rects, rop, rop, vPix->format);
		if (err)
			break;

		nBox -= chunk;
	}
	free(rects);

	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Blit", err);

	return TRUE;
}


static const gctUINT8 vivante_copy_rop[] = {
	/* GXclear        */  0x00,		// ROP_BLACK,
	/* GXand          */  0x88,		// ROP_DST_AND_SRC,
	/* GXandReverse   */  0x44,		// ROP_SRC_AND_NOT_DST,
	/* GXcopy         */  0xcc,		// ROP_SRC,
	/* GXandInverted  */  0x22,		// ROP_NOT_SRC_AND_DST,
	/* GXnoop         */  0xaa,		// ROP_DST,
	/* GXxor          */  0x66,		// ROP_DST_XOR_SRC,
	/* GXor           */  0xee,		// ROP_DST_OR_SRC,
	/* GXnor          */  0x11,		// ROP_NOT_SRC_AND_NOT_DST,
	/* GXequiv        */  0x99,		// ROP_NOT_SRC_XOR_DST,
	/* GXinvert       */  0x55,		// ROP_NOT_DST,
	/* GXorReverse    */  0xdd,		// ROP_SRC_OR_NOT_DST,
	/* GXcopyInverted */  0x33,		// ROP_NOT_SRC,
	/* GXorInverted   */  0xbb,		// ROP_NOT_SRC_OR_DST,
	/* GXnand         */  0x77,		// ROP_NOT_SRC_OR_NOT_DST,
	/* GXset          */  0xff		// ROP_WHITE
};

static gceSTATUS
vivante_blit_copy(struct vivante *vivante, GCPtr pGC, const BoxRec *total,
	const BoxRec *pbox, int nbox, xPoint dst_offset,
	struct vivante_pixmap *vDst)
{
	gctUINT8 rop = vivante_copy_rop[pGC ? pGC->alu : GXcopy];
	gceSTATUS err = gcvSTATUS_OK;
	gcsRECT dst;

	vivante_load_dst(vivante, vDst);
	vivante_set_blend(vivante, NULL);

	RectBox(&dst, total, dst_offset.x, dst_offset.y);
	err = gco2D_SetClipping(vivante->e2d, &dst);
	if (err != gcvSTATUS_OK)
		return err;

	for (; nbox; nbox--, pbox++) {
		BoxRec clipped;

		if (__box_intersect(&clipped, total, pbox))
			continue;

		RectBox(&dst, &clipped, dst_offset.x, dst_offset.y);

		err = gco2D_Blit(vivante->e2d, 1, &dst, rop, rop,
				 vDst->format);
		if (err != gcvSTATUS_OK)
			break;
	}

	return err;
}



Bool vivante_accel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n,
	DDXPointPtr ppt, int *pwidth, int fSorted)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	BoxPtr pBox, p;
	RegionRec region;
	xPoint dst_offset;
	int i;
	Bool ret, overlap;

	vPix = vivante_drawable_offset(pDrawable, &dst_offset);

	if (!gal_prepare_gpu(vivante, vPix))
		return FALSE;

	pBox = malloc(n * sizeof *pBox);
	if (!pBox)
		return FALSE;

	for (i = n, p = pBox; i; i--, p++, ppt++, pwidth++) {
		p->x1 = ppt->x;
		p->x2 = p->x1 + *pwidth;
		p->y1 = ppt->y;
		p->y2 = p->y1 + 1;
	}

	/* Convert the boxes to a region */
	RegionInitBoxes(&region, pBox, n);
	free(pBox);

	if (!fSorted)
		RegionValidate(&region, &overlap);

	/* Intersect them with the clipping region */
	RegionIntersect(&region, &region, fbGetCompositeClip(pGC));

	ret = vivante_fill(vivante, vPix, pGC, RegionExtents(&region),
			   RegionRects(&region), RegionNumRects(&region),
			   dst_offset);
	vivante_blit_complete(vivante);

	RegionUninit(&region);

	return ret;
}

Bool vivante_accel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits)
{
	ScreenPtr pScreen = pDrawable->pScreen;
	PixmapPtr pPix, pTemp;
	GCPtr gc;

	if (format != ZPixmap)
		return FALSE;

	pPix = drawable_pixmap(pDrawable);

	x += pDrawable->x;
	y += pDrawable->y;

	pTemp = pScreen->CreatePixmap(pScreen, w, h, pPix->drawable.depth, 0);
	if (!pTemp)
		return FALSE;

	gc = GetScratchGC(pTemp->drawable.depth, pScreen);
	if (!gc) {
		pScreen->DestroyPixmap(pTemp);
		return FALSE;
	}

	ValidateGC(&pTemp->drawable, gc);
	unaccel_PutImage(&pTemp->drawable, gc, depth, 0, 0, w, h, leftPad,
			 format, bits);
	FreeScratchGC(gc);

	pGC->ops->CopyArea(&pTemp->drawable, &pPix->drawable, pGC,
			   0, 0, w, h, x, y);
	pScreen->DestroyPixmap(pTemp);
	return TRUE;
}

void vivante_accel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, BoxPtr pBox, int nBox, int dx, int dy, Bool reverse,
	Bool upsidedown, Pixel bitPlane, void *closure)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pScreen);
	struct vivante_pixmap *vSrc, *vDst;
	PixmapPtr pixSrc, pixDst;
	xPoint src_offset, dst_offset;
	BoxRec extent;
	gceSTATUS err;

	if (!nBox)
		return;

	if (vivante->force_fallback)
		goto fallback;

	/* Get the source and destination pixmaps and offsets */
	pixSrc = drawable_pixmap_offset(pSrc, &src_offset);
	pixDst = drawable_pixmap_offset(pDst, &dst_offset);

	vSrc = vivante_get_pixmap_priv(pixSrc);
	vDst = vivante_get_pixmap_priv(pixDst);
	if (!vSrc || !vDst)
		goto fallback;

	/* Include the copy delta on the source */
	src_offset.x += dx - dst_offset.x;
	src_offset.y += dy - dst_offset.y;

	/* Calculate the overall extent */
	extent.x1 = max_t(short, pDst->x, pSrc->x - dx);
	extent.y1 = max_t(short, pDst->y, pSrc->y - dy);
	extent.x2 = min_t(short, pDst->x + pDst->width,
				 pSrc->x + pSrc->width - dx);
	extent.y2 = min_t(short, pDst->y + pDst->height,
				 pSrc->y + pSrc->height - dy);
	if (extent.x1 < 0)
		extent.x1 = 0;
	if (extent.y1 < 0)
		extent.y1 = 0;

	/* Right, we're all good to go */
	if (!gal_prepare_gpu(vivante, vDst) || !gal_prepare_gpu(vivante, vSrc))
		goto fallback;

	vivante_load_src(vivante, vSrc, vSrc->format, &src_offset);

	/* No need to load the brush here - the blit copy doesn't use it. */

	/* Submit the blit operations */
	err = vivante_blit_copy(vivante, pGC, &extent, pBox, nBox,
				dst_offset, vDst);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Blit", err);

	vivante_blit_complete(vivante);

	return;

 fallback:
	unaccel_CopyNtoN(pSrc, pDst, pGC, pBox, nBox, dx, dy, reverse,
		upsidedown, bitPlane, closure);
}

Bool vivante_accel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	BoxPtr pBox;
	RegionRec region;
	xPoint dst_offset;
	int i;
	Bool ret, overlap;

	vPix = vivante_drawable_offset(pDrawable, &dst_offset);

	if (!gal_prepare_gpu(vivante, vPix))
		return FALSE;

	pBox = malloc(npt * sizeof *pBox);
	if (!pBox)
		return FALSE;

	if (mode == CoordModePrevious) {
		int x, y;

		x = y = 0;
		for (i = 0; i < npt; i++) {
			x += ppt[i].x;
			y += ppt[i].y;
			pBox[i].x1 = x + pDrawable->x;
			pBox[i].y1 = y + pDrawable->y;
			pBox[i].x2 = pBox[i].x1 + 1;
			pBox[i].y2 = pBox[i].y1 + 1;
		}
	} else {
		for (i = 0; i < npt; i++) {
			pBox[i].x1 = ppt[i].x + pDrawable->x;
			pBox[i].y1 = ppt[i].y + pDrawable->y;
			pBox[i].x2 = pBox[i].x1 + 1;
			pBox[i].y2 = pBox[i].y1 + 1;
		}
	}

	/* Convert the boxes to a region */
	RegionInitBoxes(&region, pBox, npt);
	free(pBox);

	RegionValidate(&region, &overlap);

	/* Intersect them with the clipping region */
	RegionIntersect(&region, &region, fbGetCompositeClip(pGC));

	ret = vivante_fill(vivante, vPix, pGC, RegionExtents(&region),
			   RegionRects(&region), RegionNumRects(&region),
			   dst_offset);
	vivante_blit_complete(vivante);

	RegionUninit(&region);

	return ret;
}

Bool vivante_accel_PolyFillRectSolid(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	RegionPtr clip;
	BoxPtr box, extents;
	BoxRec boxes[255];
	xPoint dst_offset;
	int nclip, nb;
	Bool ret = TRUE;

	vPix = vivante_drawable_offset(pDrawable, &dst_offset);

	if (!gal_prepare_gpu(vivante, vPix))
		return FALSE;

	clip = fbGetCompositeClip(pGC);
	extents = RegionExtents(clip);

	nb = 0;
	while (n--) {
		BoxRec full_rect;

		full_rect.x1 = prect->x + pDrawable->x;
		full_rect.y1 = prect->y + pDrawable->y;
		full_rect.x2 = full_rect.x1 + prect->width;
		full_rect.y2 = full_rect.y1 + prect->height;

		prect++;

		for (box = RegionRects(clip), nclip = RegionNumRects(clip);
		     nclip; nclip--, box++) {
			if (__box_intersect(&boxes[nb], &full_rect, box))
				continue;

			if (++nb > 254) {
				ret = vivante_fill(vivante, vPix, pGC, extents,
						   boxes, nb, dst_offset);
				nb = 0;
				if (!ret)
					break;
			}
		}
		if (!ret)
			break;
	}
	if (nb)
		ret = vivante_fill(vivante, vPix, pGC, extents,
				   boxes, nb, dst_offset);
	vivante_blit_complete(vivante);

	return ret;
}

Bool vivante_accel_PolyFillRectTiled(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix, *vTile;
	PixmapPtr pTile = pGC->tile.pixmap;
	RegionPtr rects;
	xPoint dst_offset;
	int nbox;
	Bool ret;

	vPix = vivante_drawable_offset(pDrawable, &dst_offset);
	vTile = vivante_get_pixmap_priv(pTile);
	if (!vTile)
		return FALSE;

	/* Convert the rectangles to a region */
	rects = RegionFromRects(n, prect, CT_UNSORTED);

	/* Translate them for the drawable position */
	RegionTranslate(rects, pDrawable->x, pDrawable->y);

	/* Intersect them with the clipping region */
	RegionIntersect(rects, rects, fbGetCompositeClip(pGC));

	nbox = RegionNumRects(rects);
	if (nbox) {
		int tile_w, tile_h;
		BoxPtr pBox;
		gceSTATUS err = gcvSTATUS_OK;

		/* Translate them for the drawable offset */
		RegionTranslate(rects, dst_offset.x, dst_offset.y);

		ret = FALSE;

		/* Right, we're all good to go */
		if (!gal_prepare_gpu(vivante, vPix) ||
		    !gal_prepare_gpu(vivante, vTile))
			goto fallback;

		vivante_load_dst(vivante, vPix);
		vivante_load_src(vivante, vTile, vTile->format, NULL);
		vivante_set_blend(vivante, NULL);

		err = gco2D_LoadSolidBrush(vivante->e2d, vPix->format, 0, 0, ~0ULL);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "LoadSolidBrush", err);
			goto fallback;
		}

		/* Calculate the tile offset from the rect coords */
		dst_offset.x += pDrawable->x + pGC->patOrg.x;
		dst_offset.y += pDrawable->y + pGC->patOrg.y;

		tile_w = pTile->drawable.width;
		tile_h = pTile->drawable.height;

		pBox = RegionRects(rects);
		while (nbox--) {
			int dst_y, height, tile_y;
			gcsRECT clip;
			gctUINT8 rop = vivante_copy_rop[pGC ? pGC->alu : GXcopy];

			RectBox(&clip, pBox, 0, 0);

			err = gco2D_SetClipping(vivante->e2d, &clip);
			if (err != gcvSTATUS_OK) {
				vivante_error(vivante, "SetClipping", err);
				break;
			}

			dst_y = pBox->y1;
			height = pBox->y2 - dst_y;
			modulus(dst_y - dst_offset.y, tile_h, tile_y);

			while (height > 0) {
				int dst_x, width, tile_x, h;

				dst_x = pBox->x1;
				width = pBox->x2 - dst_x;
				modulus(dst_x - dst_offset.x, tile_w, tile_x);

				h = tile_h - tile_y;
				if (h > height)
					h = height;
				height -= h;

				while (width > 0) {
					gcsRECT dst, src;
					int w;

					w = tile_w - tile_x;
					if (w > width)
						w = width;
					width -= w;

					src.left = tile_x;
					src.top = tile_y;
					src.right = tile_x + w;
					src.bottom = tile_y + h;
					dst.left = dst_x;
					dst.top = dst_y;
					dst.right = dst_x + w;
					dst.bottom = dst_y + h;

					err = gco2D_BatchBlit(vivante->e2d, 1, &src, &dst, rop, rop, vPix->format);
					if (err)
						break;

					dst_x += w;
					tile_x = 0;
				}
				if (err)
					break;
				dst_y += h;
				tile_y = 0;
			}
			if (err)
				break;
			pBox++;
		}
		vivante_blit_complete(vivante);
		ret = err == 0 ? TRUE : FALSE;
	} else {
		ret = TRUE;
	}

 fallback:
	RegionUninit(rects);
	RegionDestroy(rects);

	return ret;
}

#ifdef RENDER
#include "mipict.h"
#include "fbpict.h"
#include "pictureutil.h"

static void adjust_repeat(PicturePtr pPict, int x, int y, unsigned w, unsigned h)
{
	int tx, ty;

	if (pPict->pDrawable &&
	    pPict->repeat &&
	    pPict->filter != PictFilterConvolution &&
	    transform_is_integer_translation(pPict->transform, &tx, &ty) &&
	    (pPict->pDrawable->width > 1 || pPict->pDrawable->height > 1) &&
	    drawable_contains(pPict->pDrawable, x + tx, y + ty, w, h)) {
//fprintf(stderr, "%s: removing repeat on %p\n", __FUNCTION__, pPict);
		pPict->repeat = 0;
	}
}

static const struct vivante_blend_op vivante_composite_op[] = {
#define OP(op,s,d) \
	[PictOp##op] = { \
		.src_blend = gcvSURF_BLEND_##s, \
		.dst_blend = gcvSURF_BLEND_##d, \
		.src_global_alpha = gcvSURF_GLOBAL_ALPHA_OFF, \
		.dst_global_alpha = gcvSURF_GLOBAL_ALPHA_OFF, \
	}
	OP(Clear,       ZERO,     ZERO),
	OP(Src,         ONE,      ZERO),
	OP(Dst,         ZERO,     ONE),
	OP(Over,        ONE,      INVERSED),
	OP(OverReverse, INVERSED, ONE),
	OP(In,          STRAIGHT, ZERO),
	OP(InReverse,   ZERO,     STRAIGHT),
	OP(Out,         INVERSED, ZERO),
	OP(OutReverse,  ZERO,     INVERSED),
	OP(Atop,        STRAIGHT, INVERSED),
	OP(AtopReverse, INVERSED, STRAIGHT),
	OP(Xor,         INVERSED, INVERSED),
	OP(Add,         ONE,      ONE),
//	OP(Saturate,    SRC_ALPHA_SATURATED, ZERO ),
#undef OP
};

static Bool vivante_fill_single(struct vivante *vivante,
	struct vivante_pixmap *vPix, const BoxRec *clip, uint32_t colour)
{
	gceSTATUS err;
	gcsRECT dst;

	RectBox(&dst, clip, 0, 0);

	if (!gal_prepare_gpu(vivante, vPix))
		return FALSE;

	vivante_load_dst(vivante, vPix);
	vivante_set_blend(vivante, NULL);

	err = gco2D_LoadSolidBrush(vivante->e2d, vPix->pict_format, 0, colour, ~0ULL);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_LoadSolidBrush", err);
		return FALSE;
	}

	err = gco2D_SetClipping(vivante->e2d, &dst);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_SetClipping", err);
		return FALSE;
	}

	err = gco2D_Blit(vivante->e2d, 1, &dst, 0xf0, 0xf0, vPix->pict_format);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_Blit", err);
		return FALSE;
	}

	vivante_blit_complete(vivante);

	return TRUE;
}

static Bool vivante_blend(struct vivante *vivante, const BoxRec *clip,
	const struct vivante_blend_op *blend,
	struct vivante_pixmap *vDst, struct vivante_pixmap *vSrc,
	const BoxRec *pBox, unsigned nBox, xPoint src_offset,
	xPoint dst_offset)
{
	gcsRECT dst;
	gceSTATUS err;

	if (!gal_prepare_gpu(vivante, vDst) || !gal_prepare_gpu(vivante, vSrc))
		return FALSE;

	src_offset.x -= dst_offset.x;
	src_offset.y -= dst_offset.y;

	vivante_load_dst(vivante, vDst);
	vivante_load_src(vivante, vSrc, vSrc->pict_format, &src_offset);
	vivante_set_blend(vivante, blend);

	RectBox(&dst, clip, dst_offset.x, dst_offset.y);
	err = gco2D_SetClipping(vivante->e2d, &dst);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_SetClipping", err);
		return FALSE;
	}

	while (nBox--) {
		RectBox(&dst, pBox, dst_offset.x, dst_offset.y);

		pBox++;

		err = gco2D_Blit(vivante->e2d, 1, &dst, 0xcc, 0xcc,
				 vDst->pict_format);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "gco2D_BatchBlit", err);
			return FALSE;
		}
	}

	vivante_blit_complete(vivante);

	return TRUE;
}

static void vivante_set_format(struct vivante_pixmap *vpix, PicturePtr pict)
{
	vpix->pict_format = vivante_pict_format(pict->format, FALSE);
}

static Bool vivante_pict_solid_argb(PicturePtr pict, uint32_t *col)
{
	unsigned r, g, b, a, rbits, gbits, bbits, abits;
	PictFormatPtr pFormat;
	xRenderColor colour;
	CARD32 pixel;
	uint32_t argb;

	if (!picture_is_solid(pict, &pixel))
		return FALSE;

	pFormat = pict->pFormat;
	/* If no format (eg, source-only) assume it's the correct format */
	if (!pFormat || pict->format == PICT_a8r8g8b8) {
		*col = pixel;
		return TRUE;
	}

	switch (pFormat->type) {
	case PictTypeDirect:
		r = (pixel >> pFormat->direct.red) & pFormat->direct.redMask;
		g = (pixel >> pFormat->direct.green) & pFormat->direct.greenMask;
		b = (pixel >> pFormat->direct.blue) & pFormat->direct.blueMask;
		a = (pixel >> pFormat->direct.alpha) & pFormat->direct.alphaMask;
		rbits = Ones(pFormat->direct.redMask);
		gbits = Ones(pFormat->direct.greenMask);
		bbits = Ones(pFormat->direct.blueMask);
		abits = Ones(pFormat->direct.alphaMask);
		if (abits)
			argb = scale16(a, abits) << 24;
		else
			argb = 0xff000000;
		if (rbits)
			argb |= scale16(r, rbits) << 16;
		if (gbits)
			argb |= scale16(g, gbits) << 8;
		if (bbits)
			argb |= scale16(b, bbits);
		break;
	case PictTypeIndexed:
		miRenderPixelToColor(pFormat, pixel, &colour);
		argb = (colour.alpha >> 8) << 24;
		argb |= (colour.red >> 8) << 16;
		argb |= (colour.green >> 8) << 8;
		argb |= (colour.blue >> 8);
		break;
	default:
		/* unknown type, just assume pixel value */
		argb = pixel;
		break;
	}

	*col = argb;

	return TRUE;
}

/*
 *  If we're filling a solid
 * surface, force it to have alpha; it may be used in combination
 * with a mask.  Otherwise, we ask for the plain source format,
 * with or without alpha, and convert later when copying.
 */
static struct vivante_pixmap *vivante_acquire_src(struct vivante *vivante,
	PicturePtr pict, const BoxRec *clip,
	PixmapPtr pix, struct vivante_pixmap *vTemp, xPoint *src_topleft)
{
	struct vivante_pixmap *vSrc;
	DrawablePtr drawable = pict->pDrawable;
	uint32_t colour;
	xPoint src_offset;
	int tx, ty;

	if (vivante_pict_solid_argb(pict, &colour)) {
		src_topleft->x = 0;
		src_topleft->y = 0;
		if (!vivante_fill_single(vivante, vTemp, clip, colour))
			return NULL;

		return vTemp;
	}

	vSrc = vivante_drawable_offset(pict->pDrawable, &src_offset);
	if (!vSrc)
		return NULL;

	vivante_set_format(vSrc, pict);
	if (!pict->repeat &&
	    transform_is_integer_translation(pict->transform, &tx, &ty) &&
	    vivante_format_valid(vivante, vSrc->pict_format)) {
		src_topleft->x += src_offset.x + tx;
		src_topleft->y += src_offset.y + ty;
	} else {
		PictFormatPtr f;
		PicturePtr dest;
		int err;
		int x = src_topleft->x - drawable->x;
		int y = src_topleft->y - drawable->y;
		int w = clip->x2;
		int h = clip->y2;

		f = PictureMatchFormat(drawable->pScreen, 32, PICT_a8r8g8b8);
		if (!f)
			return NULL;

		dest = CreatePicture(0, &pix->drawable, f, 0, 0, serverClient, &err);
		if (!dest)
			return NULL;
		ValidatePicture(dest);

		unaccel_Composite(PictOpSrc, pict, NULL, dest,
				  x, y, 0, 0, 0, 0, w, h);
		FreePicture(dest, 0);
		src_topleft->x = 0;
		src_topleft->y = 0;
		vSrc = vTemp;
	}

	return vSrc;
}

static int vivante_accel_final_blend(struct vivante *vivante,
	const struct vivante_blend_op *blend,
	xPoint dst_offset, RegionPtr region,
	PicturePtr pDst, struct vivante_pixmap *vDst, int xDst, int yDst,
	PicturePtr pSrc, struct vivante_pixmap *vSrc, xPoint src_offset)
{
	int rc;

	/* The correction needed to map the region boxes to the source */
	src_offset.x -= xDst;
	src_offset.y -= yDst;

#if 0
	fprintf(stderr, "%s: dst %d,%d,%d,%d %d,%d %u (%x) bo %p\n",
		__FUNCTION__, pDst->pDrawable->x, pDst->pDrawable->y,
		pDst->pDrawable->x + pDst->pDrawable->width,
		pDst->pDrawable->y + pDst->pDrawable->height,
		xDst, yDst, vDst->pict_format, pDst->format, vDst->bo);
#endif

#if 0
	dump_vPix(buf, vivante, vSrc, 1, "A-FSRC%02.2x-%p", op, pSrc);
	dump_vPix(buf, vivante, vDst, 1, "A-FDST%02.2x-%p", op, pDst);
#endif

	rc = vivante_blend(vivante, RegionExtents(region), blend, vDst, vSrc,
			   RegionRects(region), RegionNumRects(region),
			   src_offset, dst_offset);

#if 0
	dump_vPix(buf, vivante, vDst, PICT_FORMAT_A(pDst->format) != 0,
		  "A-DEST%02.2x-%p", op, pDst);
#endif

	return rc;
}

/*
 * There is a bug in the GPU hardware with destinations lacking alpha and
 * swizzles BGRA/RGBA.  Rather than the GPU treating bits 7:0 as alpha, it
 * continues to treat bits 31:24 as alpha.  This results in it replacing
 * the B or R bits on input to the blend operation with 1.0.  However, it
 * continues to accept the non-existent source alpha from bits 31:24.
 *
 * Work around this by switching to the equivalent alpha format, and using
 * global alpha to replace the alpha channel.  The alpha channel subsitution
 * is performed at this function's callsite.
 */
static Bool vivante_workaround_nonalpha(struct vivante_pixmap *vpix)
{
	switch (vpix->pict_format) {
	case gcvSURF_X4R4G4B4:
		vpix->pict_format = gcvSURF_A4R4G4B4;
		return TRUE;
	case gcvSURF_X4B4G4R4:
		vpix->pict_format = gcvSURF_A4B4G4R4;
		return TRUE;
	case gcvSURF_R4G4B4X4:
		vpix->pict_format = gcvSURF_R4G4B4A4;
		return TRUE;
	case gcvSURF_B4G4R4X4:
		vpix->pict_format = gcvSURF_B4G4R4A4;
		return TRUE;
	case gcvSURF_X1R5G5B5:
		vpix->pict_format = gcvSURF_A1R5G5B5;
		return TRUE;
	case gcvSURF_X1B5G5R5:
		vpix->pict_format = gcvSURF_A1B5G5R5;
		return TRUE;
	case gcvSURF_R5G5B5X1:
		vpix->pict_format = gcvSURF_R5G5B5A1;
		return TRUE;
	case gcvSURF_B5G5R5X1:
		vpix->pict_format = gcvSURF_B5G5R5A1;
		return TRUE;
	case gcvSURF_X8R8G8B8:
		vpix->pict_format = gcvSURF_A8R8G8B8;
		return TRUE;
	case gcvSURF_X8B8G8R8:
		vpix->pict_format = gcvSURF_A8B8G8R8;
		return TRUE;
	case gcvSURF_R8G8B8X8:
		vpix->pict_format = gcvSURF_R8G8B8A8;
		return TRUE;
	case gcvSURF_B8G8R8X8:
		vpix->pict_format = gcvSURF_B8G8R8A8;
		return TRUE;
	case gcvSURF_R5G6B5:
	case gcvSURF_B5G6R5:
		return TRUE;
	default:
		return FALSE;
	}
}

/* Perform the simple PictOpClear operation. */
static Bool vivante_Composite_Clear(PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_pixmap *vDst;
	RegionRec region;
	xPoint src_topleft, dst_offset;
	int rc;

	vDst = vivante_drawable_offset(pDst->pDrawable, &dst_offset);
	if (!vDst)
		return FALSE;

	vivante_set_format(vDst, pDst);
	vivante_workaround_nonalpha(vDst);
	if (!vivante_format_valid(vivante, vDst->pict_format))
		return FALSE;

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	/*
	 * The picture's pCompositeClip includes the destination drawable
	 * position, so we must adjust the picture position for that prior
	 * to miComputeCompositeRegion().
	 */
	if (pSrc->pDrawable) {
		xSrc += pSrc->pDrawable->x;
		ySrc += pSrc->pDrawable->y;
	}
	if (pMask && pMask->pDrawable) {
		xMask += pMask->pDrawable->x;
		yMask += pMask->pDrawable->y;
	}

	memset(&region, 0, sizeof(region));
	if (!miComputeCompositeRegion(&region, pSrc, pMask, pDst,
				      xSrc, ySrc, xMask, yMask,
				      xDst, yDst, width, height))
		return TRUE;

	src_topleft.x = xDst + dst_offset.x;
	src_topleft.y = yDst + dst_offset.y;

	rc = vivante_accel_final_blend(vivante,
				       &vivante_composite_op[PictOpClear],
				       dst_offset, &region,
				       pDst, vDst, xDst, yDst,
				       pSrc, vDst, src_topleft);
	RegionUninit(&region);

	return rc ? TRUE : FALSE;
}

int vivante_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_pixmap *vDst, *vSrc, *vMask, *vTemp = NULL;
	struct vivante_blend_op final_op;
	PixmapPtr pPixTemp = NULL;
	RegionRec region;
	BoxRec clip_temp;
	xPoint src_topleft, dst_offset;
	int rc;

	/* If the destination has an alpha map, fallback */
	if (pDst->alphaMap)
		return FALSE;

	/* Short-circuit for PictOpClear */
	if (op == PictOpClear)
		return vivante_Composite_Clear(pSrc, pMask, pDst,
					       xSrc, ySrc, xMask, yMask,
					       xDst, yDst, width, height);

	/* If we can't do the op, there's no point going any further */
	if (op >= ARRAY_SIZE(vivante_composite_op))
		return FALSE;

	if (pSrc->alphaMap || (pMask && pMask->alphaMap))
		return FALSE;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		return FALSE;

	/* The destination pixmap must have a bo */
	vDst = vivante_drawable_offset(pDst->pDrawable, &dst_offset);
	if (!vDst)
		return FALSE;

	vivante_set_format(vDst, pDst);
	if (!vivante_format_valid(vivante, vDst->pict_format))
		return FALSE;

	final_op = vivante_composite_op[op];

	if (vivante_workaround_nonalpha(vDst)) {
		final_op.dst_global_alpha = gcvSURF_GLOBAL_ALPHA_ON;
		final_op.dst_alpha = 255;
	}

	if (pMask) {
		uint32_t colour;

		if (pMask->componentAlpha)
			return FALSE;

		/*
		 * A PictOpOver with a mask looks like this:
		 *
		 *  dst.A = src.A * mask.A + dst.A * (1 - src.A * mask.A)
		 *  dst.C = src.C * mask.A + dst.C * (1 - src.A * mask.A)
		 *
		 * Or, in terms of the generic alpha blend equations:
		 *
		 *  dst.A = src.A * Fa + dst.A * Fb
		 *  dst.C = src.C * Fa + dst.C * Fb
		 *
		 * with Fa = mask.A, Fb = (1 - src.A * mask.A).  With a
		 * solid mask, mask.A is constant.
		 *
		 * Our GPU provides us with the ability to replace or scale
		 * src.A and/or dst.A inputs in the generic alpha blend
		 * equations, and using a PictOpAtop operation, the factors
		 * are Fa = dst.A, Fb = 1 - src.A.
		 *
		 * If we subsitute src.A with src.A * mask.A, and dst.A with
		 * mask.A, then we get pretty close for the colour channels.
		 * However, the alpha channel becomes simply:
		 *
		 *  dst.A = mask.A
		 *
		 * and hence will be incorrect.  Therefore, the destination
		 * format must not have an alpha channel.
		 */
		if (op == PictOpOver && !PICT_FORMAT_A(pDst->format) &&
		    vivante_pict_solid_argb(pMask, &colour)) {
			/* Convert the colour to A8 */
			colour >>= 24;

			final_op = vivante_composite_op[PictOpAtop];

			/*
			 * With global scaled alpha and a non-alpha source,
			 * the GPU appears to buggily read and use the X bits
			 * as source alpha.  Work around this by using global
			 * source alpha instead for this case.
			 */
			if (PICT_FORMAT_A(pSrc->format))
				final_op.src_global_alpha = gcvSURF_GLOBAL_ALPHA_SCALE;
			else
				final_op.src_global_alpha = gcvSURF_GLOBAL_ALPHA_ON;

			final_op.dst_global_alpha = gcvSURF_GLOBAL_ALPHA_ON;
			final_op.src_alpha =
			final_op.dst_alpha = colour;
			pMask = NULL;
		} else if (pMask->pDrawable) {
			int tx, ty;

			if (!transform_is_integer_translation(pMask->transform, &tx, &ty))
				return FALSE;

			xMask += tx;
			yMask += ty;
		} else {
			return FALSE;
		}
	}

#if 0
fprintf(stderr, "%s: i: op 0x%02x src=%p,%d,%d mask=%p,%d,%d dst=%p,%d,%d %ux%u\n",
	__FUNCTION__, op,  pSrc, xSrc, ySrc,  pMask, xMask, yMask,
	pDst, xDst, yDst,  width, height);
#endif

	memset(&region, 0, sizeof(region));

	/* Remove repeat on source or mask if useless */
	adjust_repeat(pSrc, xSrc, ySrc, width, height);
	if (pMask) {
		adjust_repeat(pMask, xMask, yMask, width, height);

		/* We don't handle mask repeats (yet) */
		if (pMask->repeat)
			goto fallback;

		/* Include the mask drawable's position on the pixmap */
		if (pMask->pDrawable) {
			xMask += pMask->pDrawable->x;
			yMask += pMask->pDrawable->y;
		}
	}

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	if (pSrc->pDrawable) {
		xSrc += pSrc->pDrawable->x;
		ySrc += pSrc->pDrawable->y;
	}

	src_topleft.x = xSrc;
	src_topleft.y = ySrc;

	/*
	 * Compute the regions to be composited.  This provides us with the
	 * rectangles which need to be composited at each stage, where the
	 * rectangle coordinates are based on the destination image.
	 *
	 * Clips are interesting.  A picture composite clip has the drawable
	 * position included in it.  A picture client clip does not.
	 *
	 * The clip region below is calculated by beginning with the box
	 * xDst,yDst,xDst+width,yDst+width, and then intersecting that with
	 * the destination composite clips.  Therefore, xDst,yDst must
	 * contain the drawable position.
	 *
	 * The source and mask pictures are then factored in, intersecting
	 * their client clips (which doesn't have a drawable position) with
	 * the current set of clips from the destination, first translating
	 * them by (xDst - xSrc),(yDst - ySrc).
	 *
	 * However, the X unaccelerated fb layer ignores any clips in the
	 * source and mask.  So... we ignore them here too.
	 */
	if (!miComputeCompositeRegion(&region, pSrc, NULL, pDst, xSrc, ySrc,
				      0, 0, xDst, yDst, width, height))
		return TRUE;

	/*
	 * Compute the temporary image clipping box, which is the
	 * clipping region extents without the destination offset.
	 */
	clip_temp = *RegionExtents(&region);
	clip_temp.x1 -= xDst;
	clip_temp.y1 -= yDst;
	clip_temp.x2 -= xDst;
	clip_temp.y2 -= yDst;

	/*
	 * Get a temporary pixmap.  We don't know whether we will need
	 * this at this stage.  Its size is the size of the temporary clip
	 * box.
	 */
	pPixTemp = pScreen->CreatePixmap(pScreen, clip_temp.x2, clip_temp.y2,
					 32, 0);
	if (!pPixTemp)
		goto failed;

	vTemp = vivante_get_pixmap_priv(pPixTemp);
	if (!vTemp)
		goto failed;

	vTemp->pict_format = vivante_pict_format(PICT_a8r8g8b8, FALSE);

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This may or may not be the temporary image,
	 * and vSrc->pict_format describes its format, including whether the
	 * alpha channel is valid.
	 */
	vSrc = vivante_acquire_src(vivante, pSrc, &clip_temp,
				   pPixTemp, vTemp, &src_topleft);
	if (!vSrc)
		goto failed;

	/*
	 * Apply the same work-around for a non-alpha source as for
	 * a non-alpha destination.
	 */
	if (!pMask && vSrc != vTemp &&
	    final_op.src_global_alpha == gcvSURF_GLOBAL_ALPHA_OFF &&
	    vivante_workaround_nonalpha(vSrc)) {
		final_op.src_global_alpha = gcvSURF_GLOBAL_ALPHA_ON;
		final_op.src_alpha = 255;
	}

//dump_vPix(buf, vivante, vSrc, 1, "A-ISRC%02.2x-%p", op, pSrc);

#if 0
#define C(p,e) ((p) ? (p)->e : 0)
fprintf(stderr, "%s: 0: OP 0x%02x src=%p[%p,%p,%u,%ux%u]x%dy%d mask=%p[%p,%u,%ux%u]x%dy%d dst=%p[%p]x%dy%d %ux%u\n",
	__FUNCTION__, op,
	pSrc, pSrc->transform, pSrc->pDrawable, pSrc->repeat, C(pSrc->pDrawable, width), C(pSrc->pDrawable, height), src_topleft.x, src_topleft.y,
	pMask, C(pMask, pDrawable), C(pMask, repeat), C(C(pMask, pDrawable), width), C(C(pMask, pDrawable), height), xMask, yMask,
	pDst, pDst->pDrawable, xDst, yDst,
	width, height);
}
#endif

	/*
	 * If we have a mask, handle it.  We deal with the mask by doing a
	 * InReverse operation.  However, note that the source may already
	 * be in the temporary buffer.  Also note that the temporary buffer
	 * must have valid alpha upon completion of this operation for the
	 * subsequent final blend to work.
	 *
	 *  If vTemp != vSrc
	 *     vTemp <= vSrc (if non-alpha, + max alpha)
	 *  vTemp <= vTemp BlendOp(In) vMask
	 *  vSrc = vTemp
	 */
	if (pMask) {
		xPoint mask_offset, temp_offset;

		vMask = vivante_drawable_offset(pMask->pDrawable, &mask_offset);
		if (!vMask)
			goto failed;

		vivante_set_format(vMask, pMask);

		mask_offset.x += xMask;
		mask_offset.y += yMask;
		temp_offset.x = 0;
		temp_offset.y = 0;
//dump_vPix(buf, vivante, vMask, 1, "A-MASK%02.2x-%p", op, pMask);

		if (vTemp != vSrc) {
			/*
			 * Copy Source to Temp.
			 * The source may not have alpha, but we need the
			 * temporary pixmap to have alpha.  Try to convert
			 * while copying.  (If this doesn't work, use OR
			 * in the brush with maximum alpha value.)
			 */
			if (!vivante_blend(vivante, &clip_temp, NULL,
					   vTemp, vSrc, &clip_temp, 1,
					   src_topleft, temp_offset))
				goto failed;
//dump_vPix(buf, vivante, vTemp, 1, "A-TMSK%02.2x-%p", op, pMask);
		}

#if 0
if (pMask && pMask->pDrawable)
 fprintf(stderr, "%s: src %d,%d,%d,%d %d,%d %u (%x)\n",
  __FUNCTION__, pMask->pDrawable->x, pMask->pDrawable->y,
  pMask->pDrawable->x + pMask->pDrawable->width, pMask->pDrawable->y + pMask->pDrawable->height,
  xMask, yMask, vMask->pict_format, pMask->format);
#endif

		if (!vivante_blend(vivante, &clip_temp,
				   &vivante_composite_op[PictOpInReverse],
				   vTemp, vMask, &clip_temp, 1,
				   mask_offset, temp_offset))
			goto failed;

		vSrc = vTemp;
		src_topleft = temp_offset;
	}

	rc = vivante_accel_final_blend(vivante, &final_op,
				       dst_offset, &region,
				       pDst, vDst, xDst, yDst,
				       pSrc, vSrc, src_topleft);
	RegionUninit(&region);
	if (pPixTemp) {
		ScreenPtr pScreen = pPixTemp->drawable.pScreen;
		pScreen->DestroyPixmap(pPixTemp);
	}
	return !!rc;

 fallback:
#if 0
#define C(p,e) ((p) ? (p)->e : 0)
fprintf(stderr, "%s: op 0x%02x src=%p[%p,%u,%ux%u]x%dy%d mask=%p[%p,%u,%ux%u]x%dy%d dst=%p[%p]x%dy%d %ux%u\n",
	__FUNCTION__, op,
	pSrc, pSrc->pDrawable, pSrc->repeat, C(pSrc->pDrawable, width), C(pSrc->pDrawable, height), xSrc, ySrc,
	pMask, C(pMask, pDrawable), C(pMask, repeat), C(C(pMask, pDrawable), width), C(C(pMask, pDrawable), height), xMask, yMask,
	pDst, pDst->pDrawable, xDst, yDst,
	width, height);
#endif

 failed:
	RegionUninit(&region);
	if (pPixTemp) {
		ScreenPtr pScreen = pPixTemp->drawable.pScreen;
		pScreen->DestroyPixmap(pPixTemp);
	}
	return FALSE;
}
#endif

Bool vivante_accel_init(struct vivante *vivante)
{
	gceCHIPMODEL model;
	gctUINT32 rev, feat, minfeat;
	gceSTATUS ret;

	ret = gcoOS_Construct(gcvNULL, &vivante->os);
	if (ret != gcvSTATUS_OK) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to construct %s object: %s\n",
			   "OS", vivante_strerror(ret));
		return FALSE;
	}

	ret = gcoHAL_Construct(gcvNULL, vivante->os, &vivante->hal);
	if (ret != gcvSTATUS_OK) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to construct %s object: %s\n",
			   "HAL", vivante_strerror(ret));
		return FALSE;
	}

	ret = gcoHAL_QueryChipIdentity(vivante->hal, &model, &rev, &feat, &minfeat);
	if (ret != gcvSTATUS_OK)
		return FALSE;

	ret = gcoHAL_Get2DEngine(vivante->hal, &vivante->e2d);
	if (ret != gcvSTATUS_OK) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to construct %s object: %s\n",
			   "2d engine", vivante_strerror(ret));
		return FALSE;
	}

	vivante->pe20 = gcoHAL_IsFeatureAvailable(vivante->hal,
						  gcvFEATURE_2DPE20);

	xf86DrvMsg(vivante->scrnIndex, X_PROBED,
		   "Vivante GC%x GPU revision %x\n", model, rev);

	vivante->max_rect_count = gco2D_GetMaximumRectCount();

	return TRUE;
}

void vivante_accel_shutdown(struct vivante *vivante)
{
	if (vivante->hal) {
		gcoHAL_Commit(vivante->hal, gcvTRUE);
		gcoHAL_Destroy(vivante->hal);
	}
	if (vivante->os)
		gcoOS_Destroy(vivante->os);
}
