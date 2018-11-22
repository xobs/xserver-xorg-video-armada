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
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include "boxutil.h"
#include "pixmaputil.h"
#include "prefetch.h"
#include "unaccel.h"
#include "utils.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"
#include "etnaviv_render.h"
#include "etnaviv_utils.h"

#include <etnaviv/etna.h>
#include <etnaviv/state.xml.h>
#include <etnaviv/state_2d.xml.h>
#include "etnaviv_compat.h"

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

void etnaviv_batch_start(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op)
{
	if (op->src.pixmap)
		etnaviv_batch_add(etnaviv, op->src.pixmap);

	etnaviv_batch_add(etnaviv, op->dst.pixmap);

	etnaviv_de_start(etnaviv, op);
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

static Bool etnaviv_init_dst_drawable(struct etnaviv *etnaviv,
	struct etnaviv_de_op *op, DrawablePtr pDrawable)
{
	op->dst.pixmap = etnaviv_drawable_offset(pDrawable, &op->dst.offset);
	if (!op->dst.pixmap)
		return FALSE;

	if (!etnaviv_dst_format_valid(etnaviv, op->dst.pixmap->format))
		return FALSE;

	if (!etnaviv_map_gpu(etnaviv, op->dst.pixmap, GPU_ACCESS_RW))
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

	if (!etnaviv_src_format_valid(etnaviv, op->src.pixmap->format) ||
	    !etnaviv_dst_format_valid(etnaviv, op->dst.pixmap->format))
		return FALSE;

	if (!etnaviv_map_gpu(etnaviv, op->dst.pixmap, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, op->src.pixmap, GPU_ACCESS_RO))
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

	if (!etnaviv_src_format_valid(etnaviv, op->src.pixmap->format))
		return FALSE;

	if (!etnaviv_map_gpu(etnaviv, op->src.pixmap, GPU_ACCESS_RO))
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
	op->src_origin_mode = SRC_ORIGIN_NONE;
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

	if (b != boxes) {
		etnaviv_batch_start(etnaviv, &op);
		etnaviv_de_op(etnaviv, &op, boxes, b - boxes);
		etnaviv_de_end(etnaviv);
	}

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
	BoxRec extent, *clip;

	if (!nBox)
		return;

	if (etnaviv->force_fallback)
		goto fallback;

	if (!etnaviv_init_dstsrc_drawable(etnaviv, &op, pDst, pSrc))
		goto fallback;

	/* Include the copy delta on the source */
	op.src.offset.x += dx - op.dst.offset.x;
	op.src.offset.y += dy - op.dst.offset.y;
	op.src_origin_mode = SRC_ORIGIN_RELATIVE;

	/* Calculate the overall extent */
	extent.x1 = max_t(short, pDst->x, pSrc->x - dx);
	extent.y1 = max_t(short, pDst->y, pSrc->y - dy);
	extent.x2 = min_t(short, pDst->x + pDst->width,
				 pSrc->x + pSrc->width - dx);
	extent.y2 = min_t(short, pDst->y + pDst->height,
				 pSrc->y + pSrc->height - dy);

	if (pGC) {
		clip = RegionExtents(fbGetCompositeClip(pGC));
		if (__box_intersect(&extent, &extent, clip))
			return;
	} else {
		if (extent.x1 < 0)
			extent.x1 = 0;
		if (extent.y1 < 0)
			extent.y1 = 0;
	}

	op.blend_op = NULL;
	op.clip = &extent;
	op.rop = etnaviv_copy_rop[pGC ? pGC->alu : GXcopy];
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_blit_clipped(etnaviv, &op, pBox, nBox);
	etnaviv_de_end(etnaviv);

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

		etnaviv_batch_start(etnaviv, &op);
		etnaviv_de_op(etnaviv, &op, RegionRects(&region),
			      RegionNumRects(&region));
		etnaviv_de_end(etnaviv);
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
			etnaviv_batch_start(etnaviv, &op);
			etnaviv_de_op(etnaviv, &op, boxes, b - boxes);
			etnaviv_de_end(etnaviv);
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
			etnaviv_batch_start(etnaviv, &op);
			etnaviv_de_op(etnaviv, &op, boxes, b - boxes);
			etnaviv_de_end(etnaviv);
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

	etnaviv_batch_start(etnaviv, &op);

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
				etnaviv_de_op(etnaviv, &op, boxes, nb);
				nb = 0;
			}
		}
	}
	if (nb)
		etnaviv_de_op(etnaviv, &op, boxes, nb);
	etnaviv_de_end(etnaviv);

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
	op.src_origin_mode = SRC_ORIGIN_NONE;
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
			xPoint tile_origin;
			int dst_y, height, tile_y;

			op.clip = pBox;

			etnaviv_batch_start(etnaviv, &op);

			dst_y = pBox->y1;
			height = pBox->y2 - dst_y;
			modulus(dst_y - tile_off_y, tile_h, tile_y);

			tile_origin.y = tile_y;

			while (height > 0) {
				int dst_x, width, tile_x, h;

				dst_x = pBox->x1;
				width = pBox->x2 - dst_x;
				modulus(dst_x - tile_off_x, tile_w, tile_x);

				tile_origin.x = tile_x;

				h = tile_h - tile_origin.y;
				if (h > height)
					h = height;
				height -= h;

				while (width > 0) {
					BoxRec dst;
					int w;

					w = tile_w - tile_origin.x;
					if (w > width)
						w = width;
					width -= w;

					dst.x1 = dst_x;
					dst.x2 = dst_x + w;
					dst.y1 = dst_y;
					dst.y2 = dst_y + h;
					etnaviv_de_op_src_origin(etnaviv, &op,
								 tile_origin,
								 &dst);

					dst_x += w;
					tile_origin.x = 0;
				}
				dst_y += h;
				tile_origin.y = 0;
			}

			etnaviv_de_end(etnaviv);

			pBox++;
		}
	}

	RegionUninit(rects);
	RegionDestroy(rects);

	return TRUE;
}

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
	etnaviv->batch_de_high_watermark = MAX_BATCH_SIZE - BATCH_WA_FLUSH_SIZE;

	/*
	 * GC320 at least seems to have a problem with corruption of
	 * consecutive operations.
	 */
	if (etnaviv->conn->chip.chip_model == chipModel_GC320) {
		struct etnaviv_format fmt = { .format = DE_FORMAT_A1R5G5B5 };
		xPoint offset = { 0, -1 };
		struct etna_bo *bo;

		bo = etna_bo_new(etnaviv->conn, 4096, DRM_ETNA_GEM_TYPE_BMP);
		etnaviv->gc320_etna_bo = bo;
		etnaviv->gc320_wa_src = INIT_BLIT_BO(bo, 64, fmt, offset);
		etnaviv->gc320_wa_dst = INIT_BLIT_BO(bo, 64, fmt, ZERO_OFFSET);

		/* reserve some additional batch space */
		etnaviv->batch_de_high_watermark -= BATCH_WA_GC320_SIZE;

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
