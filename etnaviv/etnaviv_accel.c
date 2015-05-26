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

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include "boxutil.h"
#include "glyph_assemble.h"
#include "glyph_cache.h"
#include "glyph_extents.h"
#include "pixmaputil.h"
#include "prefetch.h"
#include "unaccel.h"
#include "utils.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"
#include "etnaviv_utils.h"

#include <etnaviv/etna.h>
#include <etnaviv/state.xml.h>
#include <etnaviv/state_2d.xml.h>
#include "etnaviv_compat.h"

static inline uint32_t scale16(uint32_t val, int bits)
{
	val <<= (16 - bits);
	while (bits < 16) {
		val |= val >> bits;
		bits <<= 1;
	}
	return val >> 8;
}

void etnaviv_batch_wait_commit(struct etnaviv *etnaviv,
	struct etnaviv_pixmap *vPix)
{
	int ret;

	switch (vPix->batch_state) {
	case B_NONE:
		return;

	case B_PENDING:
		etnaviv_commit(etnaviv, TRUE, NULL);
		break;

	case B_FENCED:
		if (VIV_FENCE_BEFORE_EQ(vPix->fence, etnaviv->last_fence)) {
			/* The pixmap has already completed. */
			xorg_list_del(&vPix->batch_node);
			vPix->batch_state = B_NONE;
			break;
		}

		/*
		 * The pixmap is part of a batch which has been
		 * submitted, so we must wait for the batch to complete.
		 */
		ret = viv_fence_finish(etnaviv->conn, vPix->fence,
				       VIV_WAIT_INDEFINITE);
		if (ret != VIV_STATUS_OK)
			etnaviv_error(etnaviv, "fence finish", ret);

		etnaviv_finish_fences(etnaviv, vPix->fence);
		break;
	}
}

static void etnaviv_batch_add(struct etnaviv *etnaviv,
	struct etnaviv_pixmap *vPix)
{
	switch (vPix->batch_state) {
	case B_PENDING:
		break;
	case B_FENCED:
		xorg_list_del(&vPix->batch_node);
	case B_NONE:
		xorg_list_append(&vPix->batch_node, &etnaviv->batch_head);
		vPix->batch_state = B_PENDING;
		break;
	}
}


Bool gal_prepare_gpu(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix,
	enum gpu_access access)
{
#ifdef DEBUG_CHECK_DRAWABLE_USE
	if (vPix->in_use) {
		fprintf(stderr, "Trying to accelerate: %p %p %u\n",
				vPix,
				vPix->etna_bo ? (void *)vPix->etna_bo :
						(void *)vPix->bo,
				vPix->in_use);
		return FALSE;
	}
#endif

	if (!etnaviv_map_gpu(etnaviv, vPix, access))
		return FALSE;

	return TRUE;
}

static void etnaviv_blit_complete(struct etnaviv *etnaviv)
{
	etnaviv_de_end(etnaviv);
}

static void etnaviv_blit_start(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op)
{
	if (op->src.pixmap)
		etnaviv_batch_add(etnaviv, op->src.pixmap);

	etnaviv_batch_add(etnaviv, op->dst.pixmap);

	etnaviv_de_start(etnaviv, op);
}

static void etnaviv_blit(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op, const BoxRec *pBox, size_t nBox)
{
	while (nBox) {
		size_t n = nBox;

		if (n > VIVANTE_MAX_2D_RECTS)
			n = VIVANTE_MAX_2D_RECTS;

		etnaviv_de_op(etnaviv, op, pBox, n);

		pBox += n;
		nBox -= n;
	}
}

static void etnaviv_blit_clipped(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, const BoxRec *pbox, size_t nbox)
{
	const BoxRec *clip = op->clip;
	BoxRec boxes[VIVANTE_MAX_2D_RECTS];
	size_t n;

	for (n = 0; nbox; nbox--, pbox++) {
		if (__box_intersect(&boxes[n], clip, pbox))
			continue;

		if (++n >= VIVANTE_MAX_2D_RECTS) {
			etnaviv_de_op(etnaviv, op, boxes, n);
			n = 0;
		}
	}

	if (n)
		etnaviv_de_op(etnaviv, op, boxes, n);
}

static void etnaviv_blit_srcdst(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op,
	int src_x, int src_y, int dst_x, int dst_y, int width, int height)
{
	BoxRec box;

	op->src.offset.x = src_x - (dst_x + op->dst.offset.x);
	op->src.offset.y = src_y - (dst_y + op->dst.offset.y);

	box.x1 = dst_x;
	box.y1 = dst_y;
	box.x2 = dst_x + width;
	box.y2 = dst_y + height;

	etnaviv_blit_start(etnaviv, op);
	etnaviv_blit(etnaviv, op, &box, 1);
	etnaviv_blit_complete(etnaviv);
}

static Bool etnaviv_init_dst_drawable(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, DrawablePtr pDrawable)
{
	op->dst.pixmap = etnaviv_drawable_offset(pDrawable, &op->dst.offset);
	if (!op->dst.pixmap)
		return FALSE;

	if (!gal_prepare_gpu(etnaviv, op->dst.pixmap, GPU_ACCESS_RW))
		return FALSE;

	op->dst.bo = op->dst.pixmap->etna_bo;
	op->dst.pitch = op->dst.pixmap->pitch;
	op->dst.format = op->dst.pixmap->format;

	return TRUE;
}

static Bool etnaviv_init_dstsrc_drawable(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, DrawablePtr pDst, DrawablePtr pSrc)
{
	op->dst.pixmap = etnaviv_drawable_offset(pDst, &op->dst.offset);
	op->src.pixmap = etnaviv_drawable_offset(pSrc, &op->src.offset);
	if (!op->dst.pixmap || !op->src.pixmap)
		return FALSE;

	if (!gal_prepare_gpu(etnaviv, op->dst.pixmap, GPU_ACCESS_RW) ||
	    !gal_prepare_gpu(etnaviv, op->src.pixmap, GPU_ACCESS_RO))
		return FALSE;

	op->dst.bo = op->dst.pixmap->etna_bo;
	op->dst.pitch = op->dst.pixmap->pitch;
	op->dst.format = op->dst.pixmap->format;
	op->src.bo = op->src.pixmap->etna_bo;
	op->src.pitch = op->src.pixmap->pitch;
	op->src.format = op->src.pixmap->format;

	return TRUE;
}

static Bool etnaviv_init_src_pixmap(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, PixmapPtr pix)
{
	op->src.pixmap = etnaviv_get_pixmap_priv(pix);
	if (!op->src.pixmap)
		return FALSE;

	if (!gal_prepare_gpu(etnaviv, op->src.pixmap, GPU_ACCESS_RO))
		return FALSE;

	op->src.bo = op->src.pixmap->etna_bo;
	op->src.pitch = op->src.pixmap->pitch;
	op->src.format = op->src.pixmap->format;
	op->src.offset = ZERO_OFFSET;

	return TRUE;
}

void etnaviv_commit(struct etnaviv *etnaviv, Bool stall, uint32_t *fence)
{
	struct etna_ctx *ctx = etnaviv->ctx;
	struct etnaviv_pixmap *i, *n;
	uint32_t tmp_fence;
	int ret;

	if (!fence && stall)
		fence = &tmp_fence;

	ret = etna_flush(ctx, fence);
	if (ret) {
		etnaviv_error(etnaviv, "etna_flush", ret);
		return;
	}

	if (stall) {
		ret = viv_fence_finish(etnaviv->conn, *fence,
				       VIV_WAIT_INDEFINITE);
		if (ret != VIV_STATUS_OK)
			etnaviv_error(etnaviv, "fence finish", ret);

		/*
		 * After these operations are committed with a stall, the
		 * pixmaps on the batch head will no longer be in-use by
		 * the GPU.
		 */
		xorg_list_for_each_entry_safe(i, n, &etnaviv->batch_head,
					      batch_node) {
			xorg_list_del(&i->batch_node);
			i->batch_state = B_NONE;
		}

		/*
		 * Reap the previously submitted pixmaps, now that we know
		 * that a fence has completed.
		 */
		etnaviv->last_fence = *fence;
		etnaviv_finish_fences(etnaviv, *fence);
		etnaviv_free_busy_vpix(etnaviv);
	} else if (fence) {
		uint32_t fence_val = *fence;

		/*
		 * After these operations have been committed, we assign
		 * a fence to them, and place them on the ordered list
		 * of fenced pixmaps.
		 */
		xorg_list_for_each_entry_safe(i, n, &etnaviv->batch_head,
					      batch_node) {
			xorg_list_del(&i->batch_node);
			xorg_list_append(&i->batch_node, &etnaviv->fence_head);
			i->batch_state = B_FENCED;
			i->fence = fence_val;
		}
	}
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
static const uint8_t etnaviv_fill_rop[] = {
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

static uint32_t etnaviv_fg_col(struct etnaviv *etnaviv, GCPtr pGC)
{
	uint32_t pixel, colour;

	if (pGC->fillStyle == FillTiled)
		pixel = pGC->tileIsPixel ? pGC->tile.pixel :
			get_first_pixel(&pGC->tile.pixmap->drawable);
	else
		pixel = pGC->fgPixel;

	/* With PE1.0, this is the pixel value, but PE2.0, it must be ARGB */
	if (!VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
		return pixel;

	/*
	 * These match the 2D drawable formats for non-composite ops.
	 * The aim here is to generate an A8R8G8B8 format colour which
	 * results in a destination pixel value of 'pixel'.
	 */
	switch (pGC->depth) {
	case 15: /* A1R5G5B5 */
		colour = (pixel & 0x8000 ? 0xff000000 : 0) |
			 scale16((pixel & 0x7c00) >> 10, 5) << 16 |
			 scale16((pixel & 0x03e0) >> 5, 5) << 8 |
			 scale16((pixel & 0x001f), 5);
		break;
	case 16: /* R5G6B5 */
		colour = 0xff000000 |
			 scale16((pixel & 0xf800) >> 11, 5) << 16 |
			 scale16((pixel & 0x07e0) >> 5, 6) << 8 |
			 scale16((pixel & 0x001f), 5);
		break;
	case 24: /* A8R8G8B8 */
	default:
		colour = pixel;
		break;
	}

	return colour;
}

static void etnaviv_init_fill(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, GCPtr pGC)
{
	op->src = INIT_BLIT_NULL;
	op->blend_op = NULL;
	op->rop = etnaviv_fill_rop[pGC->alu];
	op->brush = TRUE;
	op->fg_colour = etnaviv_fg_col(etnaviv, pGC);
}

static const uint8_t etnaviv_copy_rop[] = {
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

Bool etnaviv_accel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n,
	DDXPointPtr ppt, int *pwidth, int fSorted)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	RegionPtr clip = fbGetCompositeClip(pGC);
	int nclip;
	BoxRec *boxes, *b;
	size_t sz;

	assert(pGC->miTranslate);

	if (RegionNumRects(clip) == 0)
		return TRUE;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable))
		return FALSE;

	etnaviv_init_fill(etnaviv, &op, pGC);
	op.clip = RegionExtents(clip);
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_LINE;

	nclip = RegionNumRects(clip);
	sz = sizeof(BoxRec) * n * nclip;

	/* If we overflow, fallback.  We could do better here. */
	if (sz / nclip != sizeof(BoxRec) * n)
		return FALSE;

	boxes = malloc(sz);
	if (!boxes)
		return FALSE;

	prefetch(ppt);
	prefetch(ppt + 8);
	prefetch(pwidth);
	prefetch(pwidth + 8);

	b = boxes;

	while (n--) {
		BoxPtr pBox;
		int nBox, x, y, w;

		prefetch(ppt + 16);
		prefetch(pwidth + 16);

		nBox = nclip;
		pBox = RegionRects(clip);

		y = ppt->y;
		x = ppt->x;
		w = *pwidth++;

		do {
			if (pBox->y1 <= y && pBox->y2 > y) {
				int l, r;

				l = x;
				if (l < pBox->x1)
					l = pBox->x1;
				r = x + w;
				if (r > pBox->x2)
					r = pBox->x2;

				if (l < r) {
					b->x1 = l;
					b->y1 = y;
					b->x2 = r;
					b->y2 = y;
					b++;
				}
			}
			pBox++;
		} while (--nBox);
		ppt++;
	}

	etnaviv_blit_start(etnaviv, &op);
	etnaviv_blit(etnaviv, &op, boxes, b - boxes);
	etnaviv_blit_complete(etnaviv);

	free(boxes);

	return TRUE;
}

Bool etnaviv_accel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits)
{
	ScreenPtr pScreen = pDrawable->pScreen;
	struct etnaviv_pixmap *vPix;
	PixmapPtr pPix, pTemp;
	GCPtr gc;

	if (format != ZPixmap)
		return FALSE;

	pPix = drawable_pixmap(pDrawable);
	vPix = etnaviv_get_pixmap_priv(pPix);
	if (!(vPix->state & ST_GPU_RW))
		return FALSE;

	pTemp = pScreen->CreatePixmap(pScreen, w, h, pPix->drawable.depth,
				      CREATE_PIXMAP_USAGE_GPU);
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

	pGC->ops->CopyArea(&pTemp->drawable, pDrawable, pGC,
			   0, 0, w, h, x, y);
	pScreen->DestroyPixmap(pTemp);
	return TRUE;
}

Bool etnaviv_accel_GetImage(DrawablePtr pDrawable, int x, int y, int w, int h,
	unsigned int format, unsigned long planeMask, char *d)
{
	ScreenPtr pScreen = pDrawable->pScreen;
	struct etnaviv_pixmap *vPix;
	PixmapPtr pPix, pTemp;
	GCPtr gc;
	xPoint src_offset;

	pPix = drawable_pixmap_offset(pDrawable, &src_offset);
	vPix = etnaviv_get_pixmap_priv(pPix);
	if (!vPix || !(vPix->state & ST_GPU_R))
		return FALSE;

	x += pDrawable->x + src_offset.x;
	y += pDrawable->y + src_offset.y;

	pTemp = pScreen->CreatePixmap(pScreen, w, h, pPix->drawable.depth,
				      CREATE_PIXMAP_USAGE_GPU);
	if (!pTemp)
		return FALSE;

	/*
	 * Copy to the temporary pixmap first using the GPU so that the
	 * source pixmap stays on the GPU.
	 */
	gc = GetScratchGC(pTemp->drawable.depth, pScreen);
	if (!gc) {
		pScreen->DestroyPixmap(pTemp);
		return FALSE;
	}

	ValidateGC(&pTemp->drawable, gc);
	gc->ops->CopyArea(&pPix->drawable, &pTemp->drawable, gc,
			  x, y, w, h, 0, 0);
	FreeScratchGC(gc);

	unaccel_GetImage(&pTemp->drawable, 0, 0, w, h, format, planeMask, d);

	pScreen->DestroyPixmap(pTemp);
	return TRUE;
}

void etnaviv_accel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, BoxPtr pBox, int nBox, int dx, int dy, Bool reverse,
	Bool upsidedown, Pixel bitPlane, void *closure)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pScreen);
	struct etnaviv_de_op op;
	BoxRec extent;

	if (!nBox)
		return;

	if (etnaviv->force_fallback)
		goto fallback;

	if (!etnaviv_init_dstsrc_drawable(etnaviv, &op, pDst, pSrc))
		goto fallback;

	/* Include the copy delta on the source */
	op.src.offset.x += dx - op.dst.offset.x;
	op.src.offset.y += dy - op.dst.offset.y;

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

	op.blend_op = NULL;
	op.clip = &extent;
	op.rop = etnaviv_copy_rop[pGC ? pGC->alu : GXcopy];
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	etnaviv_blit_start(etnaviv, &op);
	etnaviv_blit_clipped(etnaviv, &op, pBox, nBox);
	etnaviv_blit_complete(etnaviv);

	return;

 fallback:
	unaccel_CopyNtoN(pSrc, pDst, pGC, pBox, nBox, dx, dy, reverse,
		upsidedown, bitPlane, closure);
}

Bool etnaviv_accel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	BoxPtr pBox;
	RegionRec region;
	int i;
	Bool overlap;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable))
		return FALSE;

	etnaviv_init_fill(etnaviv, &op, pGC);
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;

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

	if (RegionNumRects(&region)) {
		op.clip = RegionExtents(&region);

		etnaviv_blit_start(etnaviv, &op);
		etnaviv_blit(etnaviv, &op, RegionRects(&region),
			     RegionNumRects(&region));
		etnaviv_blit_complete(etnaviv);
	}

	RegionUninit(&region);

	return TRUE;
}

Bool etnaviv_accel_PolyLines(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	RegionPtr clip = fbGetCompositeClip(pGC);
	const BoxRec *box;
	int nclip, i;
	BoxRec *boxes, *b;
	xSegment seg;

	assert(pGC->miTranslate);

	if (RegionNumRects(clip) == 0)
		return TRUE;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable))
		return FALSE;

	etnaviv_init_fill(etnaviv, &op, pGC);
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_LINE;

	boxes = malloc(sizeof(BoxRec) * npt);
	if (!boxes)
		return FALSE;

	nclip = RegionNumRects(clip);
	for (box = RegionRects(clip); nclip; nclip--, box++) {
		seg.x1 = ppt[0].x;
		seg.y1 = ppt[0].y;

		for (b = boxes, i = 1; i < npt; i++) {
			seg.x2 = ppt[i].x;
			seg.y2 = ppt[i].y;

			if (mode == CoordModePrevious) {
				seg.x2 += seg.x1;
				seg.y2 += seg.y1;
			}

			if (seg.x1 != seg.x2 && seg.y1 != seg.y2) {
				free(boxes);
				return FALSE;
			}

			/* We have to add the drawable position into the offset */
			seg.x1 += pDrawable->x;
			seg.x2 += pDrawable->x;
			seg.y1 += pDrawable->y;
			seg.y2 += pDrawable->y;

			if (!box_intersect_line_rough(box, &seg))
				continue;

			if (i == npt - 1 && pGC->capStyle != CapNotLast) {
				if (seg.x1 < seg.x2)
					seg.x2 += 1;
				else if (seg.x1 > seg.x2)
					seg.x2 -= 1;
				if (seg.y1 < seg.y2)
					seg.y2 += 1;
				else if (seg.y1 > seg.y2)
					seg.y2 -= 1;
			}

			b->x1 = seg.x1;
			b->y1 = seg.y1;
			b->x2 = seg.x2;
			b->y2 = seg.y2;
			b++;

			seg.x1 = ppt[i].x;
			seg.y1 = ppt[i].y;
		}

		if (b != boxes) {
			op.clip = box;
			etnaviv_blit_start(etnaviv, &op);
			etnaviv_blit(etnaviv, &op, boxes, b - boxes);
			etnaviv_blit_complete(etnaviv);
		}
	}

	free(boxes);

	return FALSE;
}

Bool etnaviv_accel_PolySegment(DrawablePtr pDrawable, GCPtr pGC, int nseg,
	xSegment *pSeg)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	RegionPtr clip = fbGetCompositeClip(pGC);
	const BoxRec *box;
	int nclip, i;
	BoxRec *boxes, *b;
	bool last;

	assert(pGC->miTranslate);

	if (RegionNumRects(clip) == 0)
		return TRUE;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable))
		return FALSE;

	etnaviv_init_fill(etnaviv, &op, pGC);
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_LINE;

	last = pGC->capStyle != CapNotLast;

	boxes = malloc(sizeof(BoxRec) * nseg * (1 + last));
	if (!boxes)
		return FALSE;

	nclip = RegionNumRects(clip);
	for (box = RegionRects(clip); nclip; nclip--, box++) {
		for (b = boxes, i = 0; i < nseg; i++) {
			xSegment seg = pSeg[i];

			/* We have to add the drawable position into the offset */
			seg.x1 += pDrawable->x;
			seg.x2 += pDrawable->x;
			seg.y1 += pDrawable->y;
			seg.y2 += pDrawable->y;

			if (!box_intersect_line_rough(box, &seg))
				continue;

			b->x1 = seg.x1;
			b->y1 = seg.y1;
			b->x2 = seg.x2;
			b->y2 = seg.y2;
			b++;

			if (last &&
			    seg.x2 >= box->x1 && seg.x2 < box->x2 &&
			    seg.y2 >= box->y1 && seg.y2 < box->y2) {
				/*
				 * Draw a one pixel long line to light the
				 * last pixel on the line, but only if the
				 * point is not off the edge.
				 */
				b->x1 = seg.x2;
				b->y1 = seg.y2;
				b->x2 = seg.x2 + 1;
				b->y2 = seg.y2;
				b++;
			}
		}

		if (b != boxes) {
			op.clip = box;
			etnaviv_blit_start(etnaviv, &op);
			etnaviv_blit(etnaviv, &op, boxes, b - boxes);
			etnaviv_blit_complete(etnaviv);
		}
	}

	free(boxes);

	return TRUE;
}

Bool etnaviv_accel_PolyFillRectSolid(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	RegionPtr clip = fbGetCompositeClip(pGC);
	BoxRec boxes[VIVANTE_MAX_2D_RECTS], *box;
	int nclip, nb, chunk;

	if (RegionNumRects(clip) == 0)
		return TRUE;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable))
		return FALSE;

	prefetch(prect);
	prefetch(prect + 4);

	etnaviv_init_fill(etnaviv, &op, pGC);
	op.clip = RegionExtents(clip);
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;

	etnaviv_blit_start(etnaviv, &op);

	chunk = VIVANTE_MAX_2D_RECTS;
	nb = 0;
	while (n--) {
		BoxRec full_rect;

		prefetch (prect + 8);

		full_rect.x1 = prect->x + pDrawable->x;
		full_rect.y1 = prect->y + pDrawable->y;
		full_rect.x2 = full_rect.x1 + prect->width;
		full_rect.y2 = full_rect.y1 + prect->height;

		prect++;

		for (box = RegionRects(clip), nclip = RegionNumRects(clip);
		     nclip; nclip--, box++) {
			if (__box_intersect(&boxes[nb], &full_rect, box))
				continue;

			if (++nb >= chunk) {
				etnaviv_blit(etnaviv, &op, boxes, nb);
				nb = 0;
			}
		}
	}
	if (nb)
		etnaviv_blit(etnaviv, &op, boxes, nb);
	etnaviv_blit_complete(etnaviv);

	return TRUE;
}

Bool etnaviv_accel_PolyFillRectTiled(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	struct etnaviv_de_op op;
	PixmapPtr pTile = pGC->tile.pixmap;
	RegionPtr rects;
	int nbox;

	if (!etnaviv_init_dst_drawable(etnaviv, &op, pDrawable) ||
	    !etnaviv_init_src_pixmap(etnaviv, &op, pTile))
		return FALSE;

	op.blend_op = NULL;
	op.rop = etnaviv_copy_rop[pGC ? pGC->alu : GXcopy];
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	/* Convert the rectangles to a region */
	rects = RegionFromRects(n, prect, CT_UNSORTED);

	/* Translate them for the drawable position */
	RegionTranslate(rects, pDrawable->x, pDrawable->y);

	/* Intersect them with the clipping region */
	RegionIntersect(rects, rects, fbGetCompositeClip(pGC));

	nbox = RegionNumRects(rects);
	if (nbox) {
		int tile_w, tile_h, tile_off_x, tile_off_y;
		BoxPtr pBox;

		/* Calculate the tile offset from the rect coords */
		tile_off_x = pDrawable->x + pGC->patOrg.x;
		tile_off_y = pDrawable->y + pGC->patOrg.y;

		tile_w = pTile->drawable.width;
		tile_h = pTile->drawable.height;

		pBox = RegionRects(rects);
		while (nbox--) {
			int dst_y, height, tile_y;

			op.clip = pBox;

			dst_y = pBox->y1;
			height = pBox->y2 - dst_y;
			modulus(dst_y - tile_off_y, tile_h, tile_y);

			while (height > 0) {
				int dst_x, width, tile_x, h;

				dst_x = pBox->x1;
				width = pBox->x2 - dst_x;
				modulus(dst_x - tile_off_x, tile_w, tile_x);

				h = tile_h - tile_y;
				if (h > height)
					h = height;
				height -= h;

				while (width > 0) {
					int w;

					w = tile_w - tile_x;
					if (w > width)
						w = width;
					width -= w;

					etnaviv_blit_srcdst(etnaviv, &op,
							    tile_x, tile_y,
							    dst_x, dst_y,
							    w, h);

					dst_x += w;
					tile_x = 0;
				}
				dst_y += h;
				tile_y = 0;
			}
			pBox++;
		}
	}

	RegionUninit(rects);
	RegionDestroy(rects);

	return TRUE;
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

static const struct etnaviv_blend_op etnaviv_composite_op[] = {
#define OP(op,s,d) \
	[PictOp##op] = { \
		.alpha_mode = \
			VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL | \
			VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL | \
			VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_##s) | \
			VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_##d), \
	}
	OP(Clear,       ZERO,     ZERO),
	OP(Src,         ONE,      ZERO),
	OP(Dst,         ZERO,     ONE),
	OP(Over,        ONE,      INVERSED),
	OP(OverReverse, INVERSED, ONE),
	OP(In,          NORMAL,   ZERO),
	OP(InReverse,   ZERO,     NORMAL),
	OP(Out,         INVERSED, ZERO),
	OP(OutReverse,  ZERO,     INVERSED),
	OP(Atop,        NORMAL,   INVERSED),
	OP(AtopReverse, INVERSED, NORMAL),
	OP(Xor,         INVERSED, INVERSED),
	OP(Add,         ONE,      ONE),
#undef OP
};

static Bool etnaviv_op_uses_source_alpha(struct etnaviv_blend_op *op)
{
	unsigned src;

	src = op->alpha_mode & VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE__MASK;

	if (src == VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ZERO) ||
	    src == VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ONE))
		return FALSE;

	return TRUE;
}

static Bool etnaviv_blend_src_alpha_normal(struct etnaviv_blend_op *op)
{
	return (op->alpha_mode &
		VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE__MASK) ==
		VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL;
}

static Bool etnaviv_fill_single(struct etnaviv *etnaviv,
	struct etnaviv_pixmap *vPix, const BoxRec *clip, uint32_t colour)
{
	struct etnaviv_de_op op = {
		.clip = clip,
		.rop = 0xf0,
		.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT,
		.brush = TRUE,
		.fg_colour = colour,
	};

	if (!gal_prepare_gpu(etnaviv, vPix, GPU_ACCESS_RW))
		return FALSE;

	op.dst = INIT_BLIT_PIX(vPix, vPix->pict_format, ZERO_OFFSET);

	etnaviv_blit_start(etnaviv, &op);
	etnaviv_blit(etnaviv, &op, clip, 1);
	etnaviv_blit_complete(etnaviv);

	return TRUE;
}

static Bool etnaviv_blend(struct etnaviv *etnaviv, const BoxRec *clip,
	const struct etnaviv_blend_op *blend,
	struct etnaviv_pixmap *vDst, struct etnaviv_pixmap *vSrc,
	const BoxRec *pBox, unsigned nBox, xPoint src_offset,
	xPoint dst_offset)
{
	struct etnaviv_de_op op = {
		.blend_op = blend,
		.clip = clip,
		.rop = 0xcc,
		.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT,
		.brush = FALSE,
	};

	if (!gal_prepare_gpu(etnaviv, vDst, GPU_ACCESS_RW) ||
	    !gal_prepare_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	op.src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_offset);
	op.dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	etnaviv_blit_start(etnaviv, &op);
	etnaviv_blit(etnaviv, &op, pBox, nBox);
	etnaviv_blit_complete(etnaviv);

	return TRUE;
}

static void etnaviv_set_format(struct etnaviv_pixmap *vpix, PicturePtr pict)
{
	vpix->pict_format = etnaviv_pict_format(pict->format, FALSE);
	vpix->pict_format.tile = vpix->format.tile;
}

static Bool etnaviv_pict_solid_argb(PicturePtr pict, uint32_t *col)
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
static struct etnaviv_pixmap *etnaviv_acquire_src(struct etnaviv *etnaviv,
	PicturePtr pict, const BoxRec *clip,
	PixmapPtr pix, struct etnaviv_pixmap *vTemp, xPoint *src_topleft)
{
	struct etnaviv_pixmap *vSrc;
	DrawablePtr drawable;
	uint32_t colour;
	xPoint src_offset;
	int tx, ty;

	if (etnaviv_pict_solid_argb(pict, &colour)) {
		src_topleft->x = 0;
		src_topleft->y = 0;
		if (!etnaviv_fill_single(etnaviv, vTemp, clip, colour))
			return NULL;

		return vTemp;
	}

	drawable = pict->pDrawable;
	vSrc = etnaviv_drawable_offset(drawable, &src_offset);
	if (!vSrc)
		return NULL;

	etnaviv_set_format(vSrc, pict);
	if (!pict->repeat &&
	    transform_is_integer_translation(pict->transform, &tx, &ty) &&
	    etnaviv_src_format_valid(etnaviv, vSrc->pict_format)) {
		src_topleft->x += drawable->x + src_offset.x + tx;
		src_topleft->y += drawable->y + src_offset.y + ty;
	} else {
		PictFormatPtr f;
		PicturePtr dest;
		int err;
		int x = src_topleft->x;
		int y = src_topleft->y;
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

static int etnaviv_accel_final_blend(struct etnaviv *etnaviv,
	const struct etnaviv_blend_op *blend,
	xPoint dst_offset, RegionPtr region,
	PicturePtr pDst, struct etnaviv_pixmap *vDst,
	PicturePtr pSrc, struct etnaviv_pixmap *vSrc, xPoint src_offset)
{
	int rc;

#if 0
	fprintf(stderr, "%s: dst %d,%d,%d,%d %u (%x) bo %p\n",
		__FUNCTION__, pDst->pDrawable->x, pDst->pDrawable->y,
		pDst->pDrawable->x + pDst->pDrawable->width,
		pDst->pDrawable->y + pDst->pDrawable->height,
		vDst->pict_format, pDst->format,
		vDst->etna_bo ? vDst->etna_bo : vDst->bo);
	etnaviv_batch_wait_commit(etnaviv, vSrc);
	dump_vPix(buf, etnaviv, vSrc, 1, "A-FSRC%02.2x-%p", op, pSrc);
	dump_vPix(buf, etnaviv, vDst, 1, "A-FDST%02.2x-%p", op, pDst);
#endif

	rc = etnaviv_blend(etnaviv, RegionExtents(region), blend, vDst, vSrc,
			   RegionRects(region), RegionNumRects(region),
			   src_offset, dst_offset);

#if 0
	etnaviv_batch_wait_commit(etnaviv, vDst);
	dump_vPix(buf, etnaviv, vDst, PICT_FORMAT_A(pDst->format) != 0,
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
static Bool etnaviv_workaround_nonalpha(struct etnaviv_pixmap *vpix)
{
	switch (vpix->pict_format.format) {
	case DE_FORMAT_X4R4G4B4:
		vpix->pict_format.format = DE_FORMAT_A4R4G4B4;
		return TRUE;
	case DE_FORMAT_X1R5G5B5:
		vpix->pict_format.format = DE_FORMAT_A1R5G5B5;
		return TRUE;
	case DE_FORMAT_X8R8G8B8:
		vpix->pict_format.format = DE_FORMAT_A8R8G8B8;
		return TRUE;
	case DE_FORMAT_R5G6B5:
		return TRUE;
	}
	return FALSE;
}

/*
 * Compute the regions (in destination pixmap coordinates) which need to
 * be composited.  Each picture's pCompositeClip includes the drawable
 * position, so each position must be adjusted for its position on the
 * backing pixmap.  We also need to apply the translation too.
 */
static int etnaviv_compute_composite_region(RegionPtr region,
	PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	int tx, ty;

	if (pSrc->pDrawable) {
		if (!transform_is_integer_translation(pSrc->transform, &tx, &ty))
			return -1;

		xSrc += pSrc->pDrawable->x + tx;
		ySrc += pSrc->pDrawable->y + ty;
	}

	if (pMask && pMask->pDrawable) {
		if (!transform_is_integer_translation(pMask->transform, &tx, &ty))
			return -1;

		xMask += pMask->pDrawable->x + tx;
		yMask += pMask->pDrawable->y + ty;
	}

	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	return miComputeCompositeRegion(region, pSrc, pMask, pDst,
					xSrc, ySrc, xMask, yMask,
					xDst, yDst, width, height);
}

/* Perform the simple PictOpClear operation. */
static Bool etnaviv_Composite_Clear(PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst;
	RegionRec region;
	xPoint src_topleft, dst_offset;
	int rc;

	vDst = etnaviv_drawable_offset(pDst->pDrawable, &dst_offset);
	if (!vDst)
		return FALSE;

	etnaviv_set_format(vDst, pDst);
	etnaviv_workaround_nonalpha(vDst);
	if (!etnaviv_dst_format_valid(etnaviv, vDst->pict_format))
		return FALSE;

	rc = etnaviv_compute_composite_region(&region, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height);
	if (rc < 1)
		return rc ? FALSE : TRUE;

	src_topleft.x = 0;
	src_topleft.y = 0;

	rc = etnaviv_accel_final_blend(etnaviv,
				       &etnaviv_composite_op[PictOpClear],
				       dst_offset, &region,
				       pDst, vDst,
				       pSrc, vDst, src_topleft);
	RegionUninit(&region);

	return rc ? TRUE : FALSE;
}

int etnaviv_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst, *vSrc, *vMask, *vTemp = NULL;
	struct etnaviv_blend_op final_op, mask_op;
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
		return etnaviv_Composite_Clear(pSrc, pMask, pDst,
					       xSrc, ySrc, xMask, yMask,
					       xDst, yDst, width, height);

	/* If we can't do the op, there's no point going any further */
	if (op >= ARRAY_SIZE(etnaviv_composite_op))
		return FALSE;

	if (pSrc->alphaMap || (pMask && pMask->alphaMap))
		return FALSE;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		return FALSE;

	/* The destination pixmap must have a bo */
	vDst = etnaviv_drawable_offset(pDst->pDrawable, &dst_offset);
	if (!vDst)
		return FALSE;

	etnaviv_set_format(vDst, pDst);
	if (!etnaviv_dst_format_valid(etnaviv, vDst->pict_format))
		return FALSE;

	final_op = etnaviv_composite_op[op];

	if (etnaviv_workaround_nonalpha(vDst)) {
		final_op.alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_GLOBAL;
		final_op.dst_alpha = 255;

		/*
		 * PE1.0 hardware contains a bug with destinations
		 * of RGB565, which force src.A to one.
		 */
		if (vDst->pict_format.format == DE_FORMAT_R5G6B5 &&
		    !VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20) &&
		    etnaviv_op_uses_source_alpha(&final_op))
			return FALSE;
	}

	/* Remove repeat on source or mask if useless */
	adjust_repeat(pSrc, xSrc, ySrc, width, height);

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
	 */
	rc = etnaviv_compute_composite_region(&region, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height);
	if (rc < 1)
		return rc ? FALSE : TRUE;

	if (pMask) {
		uint32_t colour;

		mask_op = etnaviv_composite_op[PictOpInReverse];

		if (VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20)) {
			/*
			 * PE2.0 can do component alpha blends.  Adjust
			 * the mask blend (InReverse) to perform the blend.
			 */
			mask_op.alpha_mode =
				VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL |
				VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL |
				VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ZERO) |
				VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_COLOR);
		} else if (pMask->componentAlpha) {
			/* No support for component alpha blending on PE1.0 */
			return FALSE;
		}

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
		    etnaviv_pict_solid_argb(pMask, &colour)) {
			uint32_t src_alpha_mode;

			/* Convert the colour to A8 */
			colour >>= 24;

			/*
			 * With global scaled alpha and a non-alpha source,
			 * the GPU appears to buggily read and use the X bits
			 * as source alpha.  Work around this by using global
			 * source alpha instead for this case.
			 */
			if (PICT_FORMAT_A(pSrc->format))
				src_alpha_mode = VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_SCALED;
			else
				src_alpha_mode = VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;

			final_op.alpha_mode = src_alpha_mode |
				VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_GLOBAL |
				VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_NORMAL) |
				VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_INVERSED);
			final_op.src_alpha =
			final_op.dst_alpha = colour;
			pMask = NULL;
		} else if (pMask->pDrawable) {
			int tx, ty;

			adjust_repeat(pMask, xMask, yMask, width, height);

			/* We don't handle mask repeats (yet) */
			if (pMask->repeat)
				return FALSE;

			transform_is_integer_translation(pMask->transform, &tx, &ty);

			xMask += pMask->pDrawable->x + tx;
			yMask += pMask->pDrawable->y + ty;
		} else {
			return FALSE;
		}
	}

#if 0
fprintf(stderr, "%s: i: op 0x%02x src=%p,%d,%d mask=%p,%d,%d dst=%p,%d,%d %ux%u\n",
	__FUNCTION__, op,  pSrc, xSrc, ySrc,  pMask, xMask, yMask,
	pDst, xDst, yDst,  width, height);
#endif

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

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
					 32, CREATE_PIXMAP_USAGE_GPU);
	if (!pPixTemp)
		goto failed;

	vTemp = etnaviv_get_pixmap_priv(pPixTemp);
	vTemp->pict_format = etnaviv_pict_format(PICT_a8r8g8b8, FALSE);

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This may or may not be the temporary image,
	 * and vSrc->pict_format describes its format, including whether the
	 * alpha channel is valid.
	 */
	vSrc = etnaviv_acquire_src(etnaviv, pSrc, &clip_temp,
				   pPixTemp, vTemp, &src_topleft);
	if (!vSrc)
		goto failed;

	/*
	 * Apply the same work-around for a non-alpha source as for
	 * a non-alpha destination.
	 */
	if (!pMask && vSrc != vTemp &&
	    etnaviv_blend_src_alpha_normal(&final_op) &&
	    etnaviv_workaround_nonalpha(vSrc)) {
		final_op.alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
		final_op.src_alpha = 255;
	}

//etnaviv_batch_wait_commit(etnaviv, vSrc);
//dump_vPix(buf, etnaviv, vSrc, 1, "A-ISRC%02.2x-%p", op, pSrc);

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

		vMask = etnaviv_drawable_offset(pMask->pDrawable, &mask_offset);
		if (!vMask)
			goto failed;

		etnaviv_set_format(vMask, pMask);

		mask_offset.x += xMask;
		mask_offset.y += yMask;
		temp_offset.x = 0;
		temp_offset.y = 0;
//dump_vPix(buf, etnaviv, vMask, 1, "A-MASK%02.2x-%p", op, pMask);

		if (vTemp != vSrc) {
			/*
			 * Copy Source to Temp.
			 * The source may not have alpha, but we need the
			 * temporary pixmap to have alpha.  Try to convert
			 * while copying.  (If this doesn't work, use OR
			 * in the brush with maximum alpha value.)
			 */
			if (!etnaviv_blend(etnaviv, &clip_temp, NULL,
					   vTemp, vSrc, &clip_temp, 1,
					   src_topleft, temp_offset))
				goto failed;
//etnaviv_batch_wait_commit(etnaviv, vTemp);
//dump_vPix(buf, etnaviv, vTemp, 1, "A-TMSK%02.2x-%p", op, pMask);
		}

#if 0
if (pMask && pMask->pDrawable)
 fprintf(stderr, "%s: src %d,%d,%d,%d %d,%d %u (%x)\n",
  __FUNCTION__, pMask->pDrawable->x, pMask->pDrawable->y,
  pMask->pDrawable->x + pMask->pDrawable->width, pMask->pDrawable->y + pMask->pDrawable->height,
  xMask, yMask, vMask->pict_format, pMask->format);
#endif

		if (!etnaviv_blend(etnaviv, &clip_temp, &mask_op,
				   vTemp, vMask, &clip_temp, 1,
				   mask_offset, temp_offset))
			goto failed;

		vSrc = vTemp;
		src_topleft = temp_offset;
	}

	src_topleft.x -= xDst + dst_offset.x;
	src_topleft.y -= yDst + dst_offset.y;

	rc = etnaviv_accel_final_blend(etnaviv, &final_op,
				       dst_offset, &region,
				       pDst, vDst,
				       pSrc, vSrc, src_topleft);
	RegionUninit(&region);
	if (pPixTemp) {
		ScreenPtr pScreen = pPixTemp->drawable.pScreen;
		pScreen->DestroyPixmap(pPixTemp);
	}
	return !!rc;

 failed:
	RegionUninit(&region);
	if (pPixTemp) {
		ScreenPtr pScreen = pPixTemp->drawable.pScreen;
		pScreen->DestroyPixmap(pPixTemp);
	}
	return FALSE;
}

Bool etnaviv_accel_Glyphs(CARD8 final_op, PicturePtr pSrc, PicturePtr pDst,
	PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int nlist,
	GlyphListPtr list, GlyphPtr *glyphs)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vMask;
	struct etnaviv_de_op op;
	PixmapPtr pMaskPixmap;
	PicturePtr pMask, pCurrent;
	BoxRec extents, box;
	CARD32 alpha;
	int width, height, x, y, n, error;
	struct glyph_render *gr, *grp;

	if (!maskFormat)
		return FALSE;

	n = glyphs_assemble(pScreen, &gr, &extents, nlist, list, glyphs);
	if (n == -1)
		return FALSE;
	if (n == 0)
		return TRUE;

	width = extents.x2 - extents.x1;
	height = extents.y2 - extents.y1;

	pMaskPixmap = pScreen->CreatePixmap(pScreen, width, height,
					    maskFormat->depth,
					    CREATE_PIXMAP_USAGE_GPU);
	if (!pMaskPixmap) {
		free(gr);
		return FALSE;
	}

	alpha = NeedsComponent(maskFormat->format);
	pMask = CreatePicture(0, &pMaskPixmap->drawable, maskFormat,
			      CPComponentAlpha, &alpha, serverClient, &error);
	if (!pMask)
		goto destroy_pixmap;

	/* Drop our reference to the mask pixmap */
	pScreen->DestroyPixmap(pMaskPixmap);

	vMask = etnaviv_get_pixmap_priv(pMaskPixmap);
	/* Clear the mask to transparent */
	etnaviv_set_format(vMask, pMask);
	box.x1 = box.y1 = 0;
	box.x2 = width;
	box.y2 = height;
	if (!etnaviv_fill_single(etnaviv, vMask, &box, 0))
		goto destroy_picture;

	op.dst = INIT_BLIT_PIX(vMask, vMask->pict_format, ZERO_OFFSET);
	op.blend_op = &etnaviv_composite_op[PictOpAdd];
	op.clip = &box;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	pCurrent = NULL;
	for (grp = gr; grp < gr + n; grp++) {
		if (pCurrent != grp->picture) {
			PixmapPtr pPix = drawable_pixmap(grp->picture->pDrawable);
			struct etnaviv_pixmap *v = etnaviv_get_pixmap_priv(pPix);

			if (!gal_prepare_gpu(etnaviv, v, GPU_ACCESS_RO))
				goto destroy_picture;

			prefetch(grp);

			op.src = INIT_BLIT_PIX(v, v->pict_format, ZERO_OFFSET);

			pCurrent = grp->picture;
		}

		prefetch(grp + 1);

		etnaviv_blit_srcdst(etnaviv, &op,
				    grp->glyph_pos.x, grp->glyph_pos.y,
				    grp->dest_x, grp->dest_y,
				    grp->width, grp->height);
	}

	x = extents.x1;
	y = extents.y1;

	/*
	 * x,y correspond to the top/left corner of the glyphs.
	 * list->xOff,list->yOff correspond to the baseline.  The passed
	 * xSrc/ySrc also correspond to this point.  So, we need to adjust
	 * the source for the top/left corner of the glyphs to be rendered.
	 */
	xSrc += x - list->xOff;
	ySrc += y - list->yOff;

	CompositePicture(final_op, pSrc, pMask, pDst, xSrc, ySrc, 0, 0, x, y,
			 width, height);

	FreePicture(pMask, 0);
	return TRUE;

destroy_picture:
	FreePicture(pMask, 0);
	return FALSE;

destroy_pixmap:
	pScreen->DestroyPixmap(pMaskPixmap);
	free(gr);
	return FALSE;
}

void etnaviv_accel_glyph_upload(ScreenPtr pScreen, PicturePtr pDst,
	GlyphPtr pGlyph, PicturePtr pSrc, unsigned x, unsigned y)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PixmapPtr src_pix = drawable_pixmap(pSrc->pDrawable);
	PixmapPtr dst_pix = drawable_pixmap(pDst->pDrawable);
	struct etnaviv_pixmap *vdst = etnaviv_get_pixmap_priv(dst_pix);
	struct etnaviv_de_op op;
	unsigned width = pGlyph->info.width;
	unsigned height = pGlyph->info.height;
	unsigned old_pitch = src_pix->devKind;
	unsigned i, pitch = ALIGN(old_pitch, 16);
	struct etna_bo *usr = NULL;
	BoxRec box;
	xPoint src_offset, dst_offset = { 0, };
	struct etnaviv_pixmap *vpix;
	void *b = NULL;

	src_offset.x = -x;
	src_offset.y = -y;

	vpix = etnaviv_get_pixmap_priv(src_pix);
	if (vpix) {
		etnaviv_set_format(vpix, pSrc);
		op.src = INIT_BLIT_PIX(vpix, vpix->pict_format, src_offset);
	} else {
		char *buf, *src = src_pix->devPrivate.ptr;
		size_t size, align = maxt(VIVANTE_ALIGN_MASK, getpagesize());

		size = pitch * height + align - 1;
		size &= ~(align - 1);

		if (posix_memalign(&b, align, size))
			return;

		for (i = 0, buf = b; i < height; i++, buf += pitch)
			memcpy(buf, src + old_pitch * i, old_pitch);

		usr = etna_bo_from_usermem_prot(etnaviv->conn, b, size, PROT_READ);
		if (!usr) {
			xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
				   "etnaviv: %s: etna_bo_from_usermem_prot(ptr=%p, size=%zu) failed: %s\n",
				   __FUNCTION__, b, size, strerror(errno));
			free(b);
			return;
		}

		op.src = INIT_BLIT_BO(usr, pitch,
				      etnaviv_pict_format(pSrc->format, FALSE),
				      src_offset);
	}

	box.x1 = x;
	box.y1 = y;
	box.x2 = x + width;
	box.y2 = y + height;

	etnaviv_set_format(vdst, pDst);

	if (!gal_prepare_gpu(etnaviv, vdst, GPU_ACCESS_RW))
		goto unmap;

	op.dst = INIT_BLIT_PIX(vdst, vdst->pict_format, dst_offset);
	op.blend_op = NULL;
	op.clip = &box;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	etnaviv_blit_start(etnaviv, &op);
	etnaviv_blit(etnaviv, &op, &box, 1);
	etnaviv_blit_complete(etnaviv);
	etnaviv_batch_wait_commit(etnaviv, vdst);

 unmap:
	if (usr)
		etna_bo_del(etnaviv->conn, usr, NULL);
	if (b)
		free(b);
}
#endif

Bool etnaviv_accel_init(struct etnaviv *etnaviv)
{
	Bool pe20;
	int ret;

	ret = viv_open(VIV_HW_2D, &etnaviv->conn);
	if (ret) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: unable to open: %s\n",
			   ret == -1 ? strerror(errno) : etnaviv_strerror(ret));
		return FALSE;
	}

	pe20 = VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20);

	xf86DrvMsg(etnaviv->scrnIndex, X_PROBED,
		   "Vivante GC%x GPU revision %x (etnaviv) 2d PE%s\n",
		   etnaviv->conn->chip.chip_model,
		   etnaviv->conn->chip.chip_revision,
		   pe20 ? "2.0" : "1.0");

	if (!VIV_FEATURE(etnaviv->conn, chipFeatures, PIPE_2D)) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "No 2D support\n");
		viv_close(etnaviv->conn);
		return FALSE;
	}

	ret = etna_create(etnaviv->conn, &etnaviv->ctx);
	if (ret != ETNA_OK) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: unable to create context: %s\n",
			   ret == -1 ? strerror(errno) : etnaviv_strerror(ret));
		viv_close(etnaviv->conn);
		return FALSE;
	}

	etna_set_pipe(etnaviv->ctx, ETNA_PIPE_2D);

	/*
	 * The high watermark is the index in our batch buffer at which
	 * we dump the queued operation over to the command buffers.
	 * We need room for a flush, semaphore, stall, and 20 NOPs
	 * (46 words.)
	 */
	etnaviv->batch_de_high_watermark = MAX_BATCH_SIZE - (6 + 20 * 2);

	/*
	 * GC320 at least seems to have a problem with corruption of
	 * consecutive operations.
	 */
	if (etnaviv->conn->chip.chip_model == chipModel_GC320) {
		etnaviv->gc320_etna_bo = etna_bo_new(etnaviv->conn, 4096,
						     DRM_ETNA_GEM_TYPE_BMP);
		/* reserve some additional batch space */
		etnaviv->batch_de_high_watermark -= 22;

		etnaviv_enable_bugfix(etnaviv, BUGFIX_SINGLE_BITBLT_DRAW_OP);
	}

	return TRUE;
}

void etnaviv_accel_shutdown(struct etnaviv *etnaviv)
{
	struct etnaviv_pixmap *i, *n;

	TimerFree(etnaviv->cache_timer);
	etna_finish(etnaviv->ctx);
	xorg_list_for_each_entry_safe(i, n, &etnaviv->batch_head,
				      batch_node) {
		xorg_list_del(&i->batch_node);
		i->batch_state = B_NONE;
	}
	xorg_list_for_each_entry_safe(i, n, &etnaviv->fence_head,
				      batch_node) {
		xorg_list_del(&i->batch_node);
		i->batch_state = B_NONE;
	}
	etnaviv_free_busy_vpix(etnaviv);

	if (etnaviv->gc320_etna_bo)
		etna_bo_del(etnaviv->conn, etnaviv->gc320_etna_bo, NULL);

	etna_free(etnaviv->ctx);
	viv_close(etnaviv->conn);
}
