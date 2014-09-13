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

#include "vivante_accel.h"
#include "vivante_unaccel.h"
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

static CARD32 get_first_pixel(DrawablePtr pDraw)
{
	union { CARD32 c32; CARD16 c16; CARD8 c8; char c; } pixel;

	pDraw->pScreen->GetImage(pDraw, 0, 0, 1, 1, ZPixmap, ~0, &pixel.c);

	switch (pDraw->bitsPerPixel) {
	case 32:
		return pixel.c32;
	case 16:
		return pixel.c16;
	case 8:
	case 4:
	case 1:
		return pixel.c8;
	default:
		assert(0);
	}
}

static void vivante_disable_alpha_blend(struct vivante *vivante)
{
#ifdef RENDER
	/* If alpha blending was enabled, disable it now */
	if (vivante->alpha_blend_enabled) {
		gceSTATUS err;

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

	vivante_disable_alpha_blend(vivante);

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


enum gpuid {
	GPU2D_Source,
	GPU2D_SourceBlend,
	GPU2D_Target,
};

static Bool
gal_prepare_gpu(struct vivante *vivante, struct vivante_pixmap *vPix,
	enum gpuid id)
{
	gceSTATUS err;

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

	switch (id) {
	case GPU2D_Target:
		err = gco2D_SetTarget(vivante->e2d, vPix->handle, vPix->pitch,
				      gcvSURF_0_DEGREE, 0);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "gco2D_SetTarget", err);
			return FALSE;
		}
		break;

	case GPU2D_Source:
		err = gco2D_SetColorSourceAdvanced(vivante->e2d, vPix->handle,
				  vPix->pitch, vPix->format, gcvSURF_0_DEGREE,
				  vPix->width, vPix->height, gcvFALSE);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "gco2D_SetColourSourceAdvanced", err);
			return FALSE;
		}
		break;

	case GPU2D_SourceBlend:
		break;
	}
	return TRUE;
}

static void vivante_flush(struct vivante *vivante)
{
	gceSTATUS err = gco2D_Flush(vivante->e2d);

	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Flush", err);
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
	int dx, int dy)
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

	if (!gal_prepare_gpu(vivante, vPix, GPU2D_Target)) {
		free(rects);
		return FALSE;
	}

	vivante_disable_alpha_blend(vivante);

	RectBox(&clip, clipBox, dx, dy);
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
			RectBox(r, b, dx, dy);

		err = gco2D_Blit(vivante->e2d, chunk, rects, rop, rop, vPix->format);
		if (err)
			break;

		nBox -= chunk;
	}
	free(rects);

	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Blit", err);

	vivante_batch_add(vivante, vPix);
	vivante_flush(vivante);

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
	const BoxRec *pbox, int nbox,
	int src_off_x, int src_off_y, int dst_off_x, int dst_off_y,
	gceSURF_FORMAT format)
{
	gctUINT8 rop = vivante_copy_rop[pGC ? pGC->alu : GXcopy];
	gceSTATUS err = gcvSTATUS_OK;

	for (; nbox; nbox--, pbox++) {
		BoxRec clipped;
		gcsRECT src, dst;

		if (BoxClip(&clipped, total, pbox))
			continue;

		RectBox(&src, &clipped, src_off_x, src_off_y);
		RectBox(&dst, &clipped, dst_off_x, dst_off_y);

		err = gco2D_SetClipping(vivante->e2d, &dst);
		if (err != gcvSTATUS_OK)
			break;

		err = gco2D_BatchBlit(vivante->e2d, 1, &src, &dst, rop, rop, format);
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
	PixmapPtr pPix;
	BoxPtr pBox, p;
	RegionRec region;
	int i, off_x, off_y;
	Bool ret, overlap;

	pPix = vivante_drawable_pixmap_deltas(pDrawable, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);
	if (!vPix)
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
			   off_x, off_y);

	RegionUninit(&region);

	return ret;
}

Bool vivante_accel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	RegionPtr pClip = fbGetCompositeClip(pGC);
	PixmapPtr pPix;
	BoxRec total;
	unsigned pitch, size;
	int dst_off_x, dst_off_y, off, src_off_x, src_off_y;
	gctPOINTER info;
	gctUINT32 addr;
	gceSTATUS err;
	char *buf = bits;

	if (format != ZPixmap)
		return FALSE;

	pPix = vivante_drawable_pixmap_deltas(pDrawable, &dst_off_x, &dst_off_y);
	vPix = vivante_get_pixmap_priv(pPix);
	if (!vPix)
		return FALSE;

	pitch = PixmapBytePad(w, depth);

	/*
	 * If the image is not appropriately aligned on each scanline, align
	 * it - it's cheaper to align it here, than to fall back and copy it
	 * manually to the scanout buffer.  It is unfortunate that we can't
	 * tell the X server/clients about this restriction.
	 */
	if (pitch & 15) {
		unsigned i, new_pitch = (pitch + 15) & ~15;

		buf = malloc(new_pitch * h);
		if (!buf)
			return FALSE;

		for (i = 0; i < h; i++) {
			memcpy(buf + new_pitch * i, bits + pitch * i, pitch);
			memset(buf + new_pitch * i + pitch, 0, new_pitch - pitch);
		}

		pitch = new_pitch;
	}

	size = pitch * h;

	err = gcoOS_MapUserMemory(vivante->os, buf, size, &info, &addr);
	if (err)
		return FALSE;

	/* Get the 'X' offset required to align the supplied data */
	off = addr & VIVANTE_ALIGN_MASK;

	if (!gal_prepare_gpu(vivante, vPix, GPU2D_Target))
		goto unmap;

	vivante_disable_alpha_blend(vivante);

	err = gco2D_SetColorSourceAdvanced(vivante->e2d, addr - off, pitch,
			  vPix->format, gcvSURF_0_DEGREE, w, h, gcvFALSE);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "SetColorSourceAdvanced", err);
		goto unmap;
	}

	x += pDrawable->x;
	y += pDrawable->y;

	total.x1 = x;
	total.y1 = y;
	total.x2 = x + w;
	total.y2 = y + h;
	src_off_x = -x + off * 8 / BitsPerPixel(depth);
	src_off_y = -y;

	err = vivante_blit_copy(vivante, pGC, &total, REGION_RECTS(pClip),
				REGION_NUM_RECTS(pClip), src_off_x, src_off_y,
				dst_off_x, dst_off_y, vPix->format);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Blit", err);

	vivante_batch_add(vivante, vPix);

	/* Ask for the memory to be unmapped upon completion */
	gcoHAL_ScheduleUnmapUserMemory(vivante->hal, info, size, addr, buf);

	/* We have to wait for this blit to finish... */
	vivante_batch_wait_commit(vivante, vPix);

	/* And free the buffer we may have allocated */
	if (buf != bits)
		free(buf);

	return TRUE;

 unmap:
	gcoOS_UnmapUserMemory(vivante->os, buf, size, info, addr);
	if (buf != bits)
		free(buf);

	return FALSE;
}

void vivante_accel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, BoxPtr pBox, int nBox, int dx, int dy, Bool reverse,
	Bool upsidedown, Pixel bitPlane, void *closure)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pScreen);
	struct vivante_pixmap *vSrc, *vDst;
	PixmapPtr pixSrc, pixDst;
	int dst_off_x, dst_off_y, src_off_x, src_off_y;
	BoxRec limits;
	gceSTATUS err;

	if (vivante->force_fallback)
		goto fallback;

	/* Get the source and destination pixmaps and offsets */
	pixSrc = vivante_drawable_pixmap_deltas(pSrc, &src_off_x, &src_off_y);
	pixDst = vivante_drawable_pixmap_deltas(pDst, &dst_off_x, &dst_off_y);

	vSrc = vivante_get_pixmap_priv(pixSrc);
	vDst = vivante_get_pixmap_priv(pixDst);
	if (!vSrc || !vDst)
		goto fallback;

	/* Include the copy delta on the source */
	src_off_x += dx;
	src_off_y += dy;

	/* Calculate the overall limits */
	limits.x1 = -min(src_off_x, dst_off_x);
	limits.y1 = -min(src_off_y, dst_off_y);
	limits.x2 = min(pixSrc->drawable.width - src_off_x, pixDst->drawable.width - dst_off_x);
	limits.y2 = min(pixSrc->drawable.height - src_off_y, pixDst->drawable.height - dst_off_y);

	/* Right, we're all good to go */
	if (!gal_prepare_gpu(vivante, vDst, GPU2D_Target) ||
	    !gal_prepare_gpu(vivante, vSrc, GPU2D_Source))
		goto fallback;

	vivante_disable_alpha_blend(vivante);

	/* No need to load the brush here - the blit copy doesn't use it. */

	/* Submit the blit operations */
	err = vivante_blit_copy(vivante, pGC, &limits, pBox, nBox,
				src_off_x, src_off_y,
				dst_off_x, dst_off_y, vDst->format);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "Blit", err);

	vivante_batch_add(vivante, vSrc);
	vivante_batch_add(vivante, vDst);
	vivante_flush(vivante);

	return;

 fallback:
	vivante_unaccel_CopyNtoN(pSrc, pDst, pGC, pBox, nBox, dx, dy, reverse,
		upsidedown, bitPlane, closure);
}

Bool vivante_accel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	PixmapPtr pPix;
	BoxPtr pBox;
	RegionRec region;
	int i, off_x, off_y;
	Bool ret, overlap;

	pPix = vivante_drawable_pixmap_deltas(pDrawable, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);
	if (!vPix)
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
			   off_x, off_y);

	RegionUninit(&region);

	return ret;
}

Bool vivante_accel_PolyFillRectSolid(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	PixmapPtr pPix;
	RegionPtr clip;
	BoxPtr box;
	BoxRec boxes[255], clipBox;
	int off_x, off_y, nclip, nb;
	Bool ret = TRUE;

	pPix = vivante_drawable_pixmap_deltas(pDrawable, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);
	if (!vPix)
		return FALSE;

	clip = fbGetCompositeClip(pGC);
	clipBox = *RegionExtents(clip);

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
			if (BoxClip(&boxes[nb], &full_rect, box))
				continue;

			if (++nb > 254) {
				ret = vivante_fill(vivante, vPix, pGC, &clipBox,
						   boxes, nb, off_x, off_y);
				nb = 0;
				if (!ret)
					break;
			}
		}
		if (!ret)
			break;
	}
	if (nb)
		ret = vivante_fill(vivante, vPix, pGC, &clipBox,
				   boxes, nb, off_x, off_y);

	return ret;
}

Bool vivante_accel_PolyFillRectTiled(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	struct vivante_pixmap *vPix, *vTile;
	PixmapPtr pPix, pTile = pGC->tile.pixmap;
	RegionPtr rects;
	int off_x, off_y, nbox;
	Bool ret;

	pPix = vivante_drawable_pixmap_deltas(pDrawable, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);
	vTile = vivante_get_pixmap_priv(pTile);
	if (!vPix || !vTile)
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
		RegionTranslate(rects, off_x, off_y);

		ret = FALSE;

		/* Right, we're all good to go */
		if (!gal_prepare_gpu(vivante, vPix, GPU2D_Target) ||
		    !gal_prepare_gpu(vivante, vTile, GPU2D_Source))
			goto fallback;

		vivante_disable_alpha_blend(vivante);

		err = gco2D_LoadSolidBrush(vivante->e2d, vPix->format, 0, 0, ~0ULL);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "LoadSolidBrush", err);
			goto fallback;
		}

		/* Calculate the tile offset from the rect coords */
		off_x += pDrawable->x + pGC->patOrg.x;
		off_y += pDrawable->y + pGC->patOrg.y;

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
			modulus(dst_y - off_y, tile_h, tile_y);

			while (height > 0) {
				int dst_x, width, tile_x, h;

				dst_x = pBox->x1;
				width = pBox->x2 - dst_x;
				modulus(dst_x - off_x, tile_w, tile_x);

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
		vivante_batch_add(vivante, vTile);
		vivante_batch_add(vivante, vPix);
		vivante_flush(vivante);
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
static Bool transform_is_integer_translation(PictTransformPtr t, int *tx, int *ty)
{
	if (t == NULL) {
		*tx = *ty = 0;
		return TRUE;
	}

	if (t->matrix[0][0] != IntToxFixed(1) ||
	    t->matrix[0][1] != 0 ||
	    t->matrix[1][0] != 0 ||
	    t->matrix[1][1] != IntToxFixed(1) ||
	    t->matrix[2][0] != 0 ||
	    t->matrix[2][1] != 0 ||
	    t->matrix[2][2] != IntToxFixed(1))
		return FALSE;

	if (xFixedFrac(t->matrix[0][2]) != 0 ||
	    xFixedFrac(t->matrix[1][2]) != 0)
		return FALSE;

	*tx = xFixedToInt(t->matrix[0][2]);
	*ty = xFixedToInt(t->matrix[1][2]);

	return TRUE;
}

static Bool drawable_contains(DrawablePtr drawable, int x, int y, int w, int h)
{
	if (x < 0 || y < 0 || x + w > drawable->width || y + h > drawable->height)
		return FALSE;

	return TRUE;
}

static void adjust_repeat(PicturePtr pPict, int x, int y, unsigned w, unsigned h)
{
	int tx, ty;

	if (pPict->pDrawable && 
	    pPict->repeat != RepeatNone &&
	    pPict->filter != PictFilterConvolution &&
	    transform_is_integer_translation(pPict->transform, &tx, &ty) &&
	    (pPict->pDrawable->width > 1 || pPict->pDrawable->height > 1) &&
	    drawable_contains(pPict->pDrawable, x + tx, y + ty, w, h)) {
//fprintf(stderr, "%s: removing repeat on %p\n", __FUNCTION__, pPict);
		pPict->repeat = RepeatNone;
	}
}

struct vivante_blend_op {
	gceSURF_BLEND_FACTOR_MODE src_blend;
	gceSURF_BLEND_FACTOR_MODE dst_blend;
	gceSURF_GLOBAL_ALPHA_MODE src_global_alpha;
	gceSURF_GLOBAL_ALPHA_MODE dst_global_alpha;
	uint8_t src_alpha;
	uint8_t dst_alpha;
};

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
	struct vivante_pixmap *vPix, gcsRECT_PTR rect, uint32_t colour)
{
	gceSTATUS err;

	if (!gal_prepare_gpu(vivante, vPix, GPU2D_Target))
		return FALSE;

	vivante_disable_alpha_blend(vivante);

	err = gco2D_LoadSolidBrush(vivante->e2d, vPix->pict_format, 0, colour, ~0ULL);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_LoadSolidBrush", err);
		return FALSE;
	}

	err = gco2D_SetClipping(vivante->e2d, rect);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_SetClipping", err);
		return FALSE;
	}

	err = gco2D_Blit(vivante->e2d, 1, rect, 0xf0, 0xf0, vPix->pict_format);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_Blit", err);
		return FALSE;
	}

	vivante_batch_add(vivante, vPix);

	return TRUE;
}

static Bool vivante_blend(struct vivante *vivante, gcsRECT_PTR clip,
	const struct vivante_blend_op *blend,
	struct vivante_pixmap *vDst, gcsRECT_PTR rDst,
	struct vivante_pixmap *vSrc, gcsRECT_PTR rSrc,
	unsigned nRect)
{
	gceSTATUS err;

	if (!gal_prepare_gpu(vivante, vDst, GPU2D_Target) ||
	    !gal_prepare_gpu(vivante, vSrc, GPU2D_SourceBlend))
		return FALSE;

	if (!blend) {
		vivante_disable_alpha_blend(vivante);
	} else {
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
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "gco2D_EnableAlphaBlend", err);
			return FALSE;
		}
		vivante->alpha_blend_enabled = TRUE;
	}

	err = gco2D_SetColorSourceAdvanced(vivante->e2d, vSrc->handle,
			  vSrc->pitch, vSrc->pict_format, gcvSURF_0_DEGREE,
			  vSrc->width, vSrc->height, gcvFALSE);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_SetColorSourceAdvanced", err);
		return FALSE;
	}

	err = gco2D_SetClipping(vivante->e2d, clip);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_SetClipping", err);
		return FALSE;
	}

	err = gco2D_BatchBlit(vivante->e2d, nRect, rSrc, rDst, 0xcc, 0xcc,
			vDst->pict_format);
	if (err != gcvSTATUS_OK) {
		vivante_error(vivante, "gco2D_BatchBlit", err);
		return FALSE;
	}

	vivante_batch_add(vivante, vDst);
	vivante_batch_add(vivante, vSrc);
	vivante_flush(vivante);

	return TRUE;
}

/*
 * Returns TRUE and the pixel value in COLOUR if the picture
 * represents a solid surface of constant colour.
 */
static Bool vivante_picture_is_solid(PicturePtr pict, CARD32 *colour)
{
	if (pict->pDrawable) {
		DrawablePtr pDraw = pict->pDrawable;

		if (pDraw->width == 1 && pDraw->height == 1 &&
		    pict->repeat != RepeatNone) {
			if (colour)
				*colour = get_first_pixel(pDraw);
			return TRUE;
		}
	} else {
		SourcePict *sp = pict->pSourcePict;

		if (sp->type == SourcePictTypeSolidFill) {
			if (colour)
				*colour = sp->solidFill.color;
			return TRUE;
		}
	}

	return FALSE;
}

static Bool vivante_pict_solid_argb(PicturePtr pict, uint32_t *col)
{
	unsigned r, g, b, a, rbits, gbits, bbits, abits;
	PictFormatPtr pFormat;
	xRenderColor colour;
	CARD32 pixel;
	uint32_t argb;

	if (!vivante_picture_is_solid(pict, &pixel))
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
	PicturePtr pict, int x, int y, int w, int h, gcsRECT_PTR clip,
	PixmapPtr pix, struct vivante_pixmap *vTemp,
	INT16 *xout, INT16 *yout)
{
	PixmapPtr pPixmap;
	struct vivante_pixmap *vSrc;
	DrawablePtr drawable = pict->pDrawable;
	uint32_t colour;
	int tx, ty, ox, oy;

	if (vivante_pict_solid_argb(pict, &colour)) {
		*xout = 0;
		*yout = 0;
		if (!vivante_fill_single(vivante, vTemp, clip, colour))
			return NULL;
		vivante_flush(vivante);

		return vTemp;
	}

	pPixmap = vivante_drawable_pixmap_deltas(pict->pDrawable, &ox, &oy);
	vSrc = vivante_get_pixmap_priv(pPixmap);
	if (!vSrc)
		return NULL;

	vSrc->pict_format = vivante_pict_format(pict->format, FALSE);
	if (pict->repeat == RepeatNone &&
	    transform_is_integer_translation(pict->transform, &tx, &ty) &&
	    vivante_format_valid(vivante, vSrc->pict_format)) {
		*xout = ox + x + tx + drawable->x;
		*yout = ox + y + ty + drawable->y;
	} else {
		PictFormatPtr f;
		PicturePtr dest;
		int err;

		f = PictureMatchFormat(drawable->pScreen, 32, PICT_a8r8g8b8);
		if (!f)
			return NULL;

		dest = CreatePicture(0, &pix->drawable, f, 0, 0, serverClient, &err);
		if (!dest)
			return NULL;
		ValidatePicture(dest);

		vivante_unaccel_Composite(PictOpSrc, pict, NULL, dest, x, y, 0, 0, 0, 0, w, h);
		FreePicture(dest, 0);
		*xout = 0;
		*yout = 0;
		vSrc = vTemp;
	}

	return vSrc;
}

static int vivante_accel_final_blend(struct vivante *vivante,
	const struct vivante_blend_op *blend,
	int oDst_x, int oDst_y, RegionPtr region,
	PicturePtr pDst, struct vivante_pixmap *vDst, int xDst, int yDst,
	PicturePtr pSrc, struct vivante_pixmap *vSrc, int xSrc, int ySrc)
{
	gcsRECT *rects, *rsrc, *rdst;
	gcsRECT clip;
	int i, nrects, rc;

	RectBox(&clip, RegionExtents(region), oDst_x, oDst_y);

#if 0
	fprintf(stderr, "%s: dst %d,%d,%d,%d %d,%d %u (%x) bo %p\n",
		__FUNCTION__, pDst->pDrawable->x, pDst->pDrawable->y,
		pDst->pDrawable->x + pDst->pDrawable->width,
		pDst->pDrawable->y + pDst->pDrawable->height,
		xDst, yDst, vDst->pict_format, pDst->format, vDst->bo);
#endif

	nrects = REGION_NUM_RECTS(region);
	rects = malloc(sizeof(*rects) * nrects * 2);
	if (!rects) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "%s: malloc failed\n", __FUNCTION__);
		return FALSE;
	}

	xSrc -= xDst;
	ySrc -= yDst;

	for (i = 0, rsrc = rects, rdst = rsrc + nrects;
	     i < nrects;
	     i++, rsrc++, rdst++) {
		RectBox(rsrc, REGION_RECTS(region) + i, xSrc, ySrc);
		RectBox(rdst, REGION_RECTS(region) + i, oDst_x, oDst_y);
	}

	rsrc = rects;
	rdst = rects + nrects;

#if 0
	vivante_batch_wait_commit(vivante, vSrc);
	dump_vPix(buf, vivante, vSrc, 1, "A-FSRC%02.2x-%p", op, pSrc);
	dump_vPix(buf, vivante, vDst, 1, "A-FDST%02.2x-%p", op, pDst);
#endif

	rc = vivante_blend(vivante, &clip, blend,
			   vDst, rdst, vSrc, rsrc, nrects);

	free(rects);

#if 0
	vivante_batch_wait_commit(vivante, vDst);
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

int vivante_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_pixmap *vDst, *vSrc, *vMask, *vTemp = NULL;
	struct vivante_blend_op final_op;
	PixmapPtr pPixmap, pPixTemp = NULL;
	RegionRec region;
	gcsRECT clipTemp;
	int oDst_x, oDst_y, rc;

	/* If we can't do the op, there's no point going any further */
	if (op >= ARRAY_SIZE(vivante_composite_op))
		return FALSE;

	/* If there are alpha maps, fallback for now */
	if (pDst->alphaMap || pSrc->alphaMap || (pMask && pMask->alphaMap))
		return FALSE;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !vivante_picture_is_solid(pSrc, NULL))
		return FALSE;

	/* The destination pixmap must have a bo */
	pPixmap = vivante_drawable_pixmap_deltas(pDst->pDrawable, &oDst_x, &oDst_y);
	vDst = vivante_get_pixmap_priv(pPixmap);
	if (!vDst)
		return FALSE;

	vDst->pict_format = vivante_pict_format(pDst->format, FALSE);
	if (!vivante_format_valid(vivante, vDst->pict_format))
		return FALSE;

	final_op = vivante_composite_op[op];

	/*
	 * There is a bug in the GPU hardware with destinations lacking
	 * alpha and swizzles BGRA/RGBA.  Rather than the GPU treating
	 * bits 7:0 as alpha, it continues to treat bits 31:24 as alpha.
	 * This results in it replacing the B or R bits on input to the
	 * blend operation with 1.0.  However, it continues to accept the
	 * non-existent source alpha from bits 31:24.
	 *
	 * Work around this by switching to the equivalent alpha format,
	 * and use global alpha to replace the alpha channel.
	 */
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
		if (pMask->repeat != RepeatNone)
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
	RectBox(&clipTemp, RegionExtents(&region), -xDst, -yDst);

	/*
	 * Get a temporary pixmap.  We don't really know yet whether we're
	 * going to use it or not.
	 */
	pPixTemp = pScreen->CreatePixmap(pScreen, width, height, 32, 0);
	if (!pPixTemp)
		goto failed;

	vTemp = vivante_get_pixmap_priv(pPixTemp);
	if (!vTemp)
		goto failed;

	vTemp->pict_format = vivante_pict_format(PICT_a8r8g8b8, FALSE);

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * offset xSrc/ySrc.  This may or may not be the temporary image, and
	 * vSrc->pict_format describes its format, including whether the
	 * alpha channel is valid.
	 */
	if (op == PictOpClear) {
		if (!vivante_fill_single(vivante, vTemp, &clipTemp, 0))
			goto failed;
		vivante_flush(vivante);
		vSrc = vTemp;
		xSrc = 0;
		ySrc = 0;
	} else {
		vSrc = vivante_acquire_src(vivante, pSrc, xSrc, ySrc,
					   width, height,
					   &clipTemp, pPixTemp, vTemp,
					   &xSrc, &ySrc);
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
	}

//vivante_batch_wait_commit(vivante, vSrc);
//dump_vPix(buf, vivante, vSrc, 1, "A-ISRC%02.2x-%p", op, pSrc);

#if 0
#define C(p,e) ((p) ? (p)->e : 0)
fprintf(stderr, "%s: 0: OP 0x%02x src=%p[%p,%p,%u,%ux%u]x%dy%d mask=%p[%p,%u,%ux%u]x%dy%d dst=%p[%p]x%dy%d %ux%u\n",
	__FUNCTION__, op,
	pSrc, pSrc->transform, pSrc->pDrawable, pSrc->repeat, C(pSrc->pDrawable, width), C(pSrc->pDrawable, height), xSrc, ySrc,
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
		PixmapPtr pPixMask;
		gcsRECT rsrc, rdst;
		int oMask_x, oMask_y;

		pPixMask = vivante_drawable_pixmap_deltas(pMask->pDrawable, &oMask_x, &oMask_y);
		vMask = vivante_get_pixmap_priv(pPixMask);
		if (!vMask)
			goto failed;

		vMask->pict_format = vivante_pict_format(pMask->format, FALSE);

		oMask_x += xMask;
		oMask_y += yMask;
//dump_vPix(buf, vivante, vMask, 1, "A-MASK%02.2x-%p", op, pMask);
		rdst.left = 0;
		rdst.top = 0;
		rdst.right = width;
		rdst.bottom = height;

		if (vTemp != vSrc) {
			/* Copy Source to Temp */
			rsrc.left = xSrc;
			rsrc.top = ySrc;
			rsrc.right = xSrc + width;
			rsrc.bottom = ySrc + height;

			/*
			 * The source may not have alpha, but we need the
			 * temporary pixmap to have alpha.  Try to convert
			 * while copying.  (If this doesn't work, use OR
			 * in the brush with maximum alpha value.)
			 */
			if (!vivante_blend(vivante, &clipTemp, NULL,
					   vTemp, &rdst,
					   vSrc, &rsrc, 1))
				goto failed;
//vivante_batch_wait_commit(vivante, vTemp);
//dump_vPix(buf, vivante, vTemp, 1, "A-TMSK%02.2x-%p", op, pMask);
		}

		rsrc.left = oMask_x;
		rsrc.top = oMask_y;
		rsrc.right = oMask_x + width;
		rsrc.bottom = oMask_y + height;

#if 0
if (pMask && pMask->pDrawable)
 fprintf(stderr, "%s: src %d,%d,%d,%d %d,%d %u (%x)\n",
  __FUNCTION__, pMask->pDrawable->x, pMask->pDrawable->y,
  pMask->pDrawable->x + pMask->pDrawable->width, pMask->pDrawable->y + pMask->pDrawable->height,
  xMask, yMask, vMask->pict_format, pMask->format);
#endif

		if (!vivante_blend(vivante, &clipTemp,
				   &vivante_composite_op[PictOpInReverse],
				   vTemp, &rdst,
				   vMask, &rsrc,
				   1))
			goto failed;

		vSrc = vTemp;
		xSrc = 0;
		ySrc = 0;
	}

	rc = vivante_accel_final_blend(vivante, &final_op,
				       oDst_x, oDst_y, &region,
				       pDst, vDst, xDst, yDst,
				       pSrc, vSrc, xSrc, ySrc);
	RegionUninit(&region);
	if (!rc)
		goto failed;

	goto done;

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

 done:
	if (pPixTemp) {
		ScreenPtr pScreen = pPixTemp->drawable.pScreen;
		pScreen->DestroyPixmap(pPixTemp);
	}
	return TRUE;
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
