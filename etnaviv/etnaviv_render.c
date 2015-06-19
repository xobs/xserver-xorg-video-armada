/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <etnaviv/state_2d.xml.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#ifdef RENDER

#include "mipict.h"
#include "fbpict.h"

#include "glyph_assemble.h"
#include "glyph_cache.h"
#include "glyph_extents.h"
#include "pictureutil.h"
#include "prefetch.h"
#include "unaccel.h"

#include "etnaviv_accel.h"
#include "etnaviv_render.h"
#include "etnaviv_utils.h"
#include "etnaviv_compat.h"

static struct etnaviv_format etnaviv_pict_format(PictFormatShort format,
	Bool force)
{
	switch (format) {
#define DE_FORMAT_UNKNOWN UNKNOWN_FORMAT
#define C(pf,vf,af,sw) case PICT_##pf: \
	return (struct etnaviv_format){ \
			.format = force ? DE_FORMAT_##af : DE_FORMAT_##vf, \
			.swizzle = DE_SWIZZLE_##sw, \
		}

	C(a8r8g8b8, 	A8R8G8B8,	A8R8G8B8,	ARGB);
	C(x8r8g8b8, 	X8R8G8B8,	A8R8G8B8,	ARGB);
	C(a8b8g8r8, 	A8R8G8B8,	A8R8G8B8,	ABGR);
	C(x8b8g8r8, 	X8R8G8B8,	A8R8G8B8,	ABGR);
	C(b8g8r8a8, 	A8R8G8B8,	A8R8G8B8,	BGRA);
	C(b8g8r8x8, 	X8R8G8B8,	A8R8G8B8,	BGRA);
	C(r5g6b5,	R5G6B5,		UNKNOWN,	ARGB);
	C(b5g6r5,	R5G6B5,		UNKNOWN,	ABGR);
	C(a1r5g5b5, 	A1R5G5B5,	A1R5G5B5,	ARGB);
	C(x1r5g5b5, 	X1R5G5B5,	A1R5G5B5,	ARGB);
	C(a1b5g5r5, 	A1R5G5B5,	A1R5G5B5,	ABGR);
	C(x1b5g5r5, 	X1R5G5B5,	A1R5G5B5,	ABGR);
	C(a4r4g4b4, 	A4R4G4B4,	A4R4G4B4,	ARGB);
	C(x4r4g4b4, 	X4R4G4B4,	A4R4G4B4,	ARGB);
	C(a4b4g4r4, 	A4R4G4B4,	A4R4G4B4,	ABGR);
	C(x4b4g4r4, 	X4R4G4B4,	A4R4G4B4,	ABGR);
	C(a8,		A8,		A8,		ARGB);
	C(c8,		INDEX8,		INDEX8,		ARGB);

/* The remainder we don't support */
//	C(r8g8b8,	R8G8B8,		UNKNOWN,	ARGB);
//	C(b8g8r8,	R8G8B8,		UNKNOWN,	ABGR);
//	C(r3g3b2,	R3G3B2,		UNKNOWN);
//	C(b2g3r3,	UNKNOWN,	UNKNOWN);
//	C(a2r2g2b2, 	A2R2G2B2,	A2R2G2B2);
//	C(a2b2g2r2, 	UNKNOWN,	A2R2G2B2);
//	C(g8,		L8,		UNKNOWN);
//	C(x4a4,		UNKNOWN,	UNKNOWN);
//	C(x4c4,		UNKNOWN,	UNKNOWN); /* same value as c8 */
//	C(x4g4,		UNKNOWN,	UNKNOWN); /* same value as g8 */
//	C(a4,		A4,		A4);
//	C(r1g2b1,	UNKNOWN,	UNKNOWN);
//	C(b1g2r1,	UNKNOWN,	UNKNOWN);
//	C(a1r1g1b1, 	UNKNOWN,	UNKNOWN);
//	C(a1b1g1r1, 	UNKNOWN,	UNKNOWN);
//	C(c4,		INDEX4,		UNKNOWN);
//	C(g4,		L4,		UNKNOWN);
//	C(a1,		A1,		A1);
//	C(g1,		L1,		UNKNOWN);
	default:
		break;
	}
	return (struct etnaviv_format){ .format = UNKNOWN_FORMAT, .swizzle = 0 };
#undef C
}

#ifdef DEBUG_BLEND
static void etnaviv_debug_blend_op(const char *func,
	CARD8 op, CARD16 width, CARD16 height,
	PicturePtr pSrc, INT16 xSrc, INT16 ySrc,
	PicturePtr pMask, INT16 xMask, INT16 yMask,
	PicturePtr pDst, INT16 xDst, INT16 yDst)
{
	char src_buf[80], mask_buf[80], dst_buf[80];

	fprintf(stderr,
		"%s: op 0x%02x %ux%u\n"
		"  src  %s\n"
		"  mask %s\n"
		"  dst  %s\n",
		func, op, width, height,
		picture_desc(pSrc, src_buf, sizeof(src_buf)),
		picture_desc(pMask, mask_buf, sizeof(mask_buf)),
		picture_desc(pDst, dst_buf, sizeof(dst_buf)));
}
#endif

/*
 * For a rectangle described by (wxh+x+y) on the picture's drawable,
 * determine whether the picture repeat flag is meaningful.  The
 * rectangle must have had the transformation applied.
 */
static Bool picture_needs_repeat(PicturePtr pPict, int x, int y,
	unsigned w, unsigned h)
{
	DrawablePtr pDrawable;

	if (!pPict->repeat)
		return FALSE;

	pDrawable = pPict->pDrawable;
	if (!pDrawable)
		return TRUE;

	if (pPict->filter != PictFilterConvolution &&
	    (pDrawable->width > 1 || pDrawable->height > 1) &&
	    drawable_contains(pDrawable, x, y, w, h))
		return FALSE;

	return TRUE;
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

	if (!etnaviv_map_gpu(etnaviv, vPix, GPU_ACCESS_RW))
		return FALSE;

	op.dst = INIT_BLIT_PIX(vPix, vPix->pict_format, ZERO_OFFSET);

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, clip, 1);
	etnaviv_de_end(etnaviv);

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
		.src_origin_mode = SRC_ORIGIN_RELATIVE,
		.rop = 0xcc,
		.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT,
		.brush = FALSE,
	};

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	op.src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_offset);
	op.dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, pBox, nBox);
	etnaviv_de_end(etnaviv);

	return TRUE;
}

static void etnaviv_set_format(struct etnaviv_pixmap *vpix, PicturePtr pict)
{
	vpix->pict_format = etnaviv_pict_format(pict->format, FALSE);
	vpix->pict_format.tile = vpix->format.tile;
}

static struct etnaviv_pixmap *etnaviv_get_scratch_argb(ScreenPtr pScreen,
	PixmapPtr *ppPixmap, unsigned int width, unsigned int height)
{
	struct etnaviv_pixmap *vpix;
	PixmapPtr pixmap;

	if (*ppPixmap)
		return etnaviv_get_pixmap_priv(*ppPixmap);

	pixmap = pScreen->CreatePixmap(pScreen, width, height, 32,
				       CREATE_PIXMAP_USAGE_GPU);
	if (!pixmap)
		return NULL;

	vpix = etnaviv_get_pixmap_priv(pixmap);
	vpix->pict_format = etnaviv_pict_format(PICT_a8r8g8b8, FALSE);

	*ppPixmap = pixmap;

	return vpix;
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

static Bool etnaviv_composite_to_pixmap(CARD8 op, PicturePtr pSrc,
	PicturePtr pMask, PixmapPtr pPix, INT16 xSrc, INT16 ySrc,
	INT16 xMask, INT16 yMask, CARD16 width, CARD16 height)
{
	DrawablePtr pDrawable = &pPix->drawable;
	ScreenPtr pScreen = pPix->drawable.pScreen;
	PictFormatPtr f;
	PicturePtr dest;
	int err;

	f = PictureMatchFormat(pScreen, 32, PICT_a8r8g8b8);
	if (!f)
		return FALSE;

	dest = CreatePicture(0, pDrawable, f, 0, 0, serverClient, &err);
	if (!dest)
		return FALSE;
	ValidatePicture(dest);

	unaccel_Composite(op, pSrc, pMask, dest, xSrc, ySrc, xMask, yMask,
			  0, 0, width, height);

	FreePicture(dest, 0);

	return TRUE;
}

/*
 * Acquire the source. If we're filling a solid surface, force it to have
 * alpha; it may be used in combination with a mask.  Otherwise, we ask
 * for the plain source format, with or without alpha, and convert later
 * when copying.  If force_vtemp is set, we ensure that the source is in
 * our temporary pixmap.
 */
static struct etnaviv_pixmap *etnaviv_acquire_src(ScreenPtr pScreen,
	PicturePtr pict, const BoxRec *clip, PixmapPtr *ppPixTemp,
	xPoint *src_topleft, Bool force_vtemp)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vSrc, *vTemp;
	DrawablePtr drawable;
	uint32_t colour;
	xPoint src_offset;
	int tx, ty;

	if (etnaviv_pict_solid_argb(pict, &colour)) {
		vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
						 clip->x2, clip->y2);
		if (!vTemp)
			return NULL;

		if (!etnaviv_fill_single(etnaviv, vTemp, clip, colour))
			return NULL;

		src_topleft->x = 0;
		src_topleft->y = 0;
		return vTemp;
	}

	drawable = pict->pDrawable;
	vSrc = etnaviv_drawable_offset(drawable, &src_offset);
	if (!vSrc)
		goto fallback;

	etnaviv_set_format(vSrc, pict);
	if (!etnaviv_src_format_valid(etnaviv, vSrc->pict_format))
		goto fallback;

	if (!transform_is_integer_translation(pict->transform, &tx, &ty))
		goto fallback;

	if (picture_needs_repeat(pict, src_topleft->x + tx, src_topleft->y + ty,
				 clip->x2, clip->y2))
		goto fallback;

	src_topleft->x += drawable->x + src_offset.x + tx;
	src_topleft->y += drawable->y + src_offset.y + ty;
	if (force_vtemp)
		goto copy_to_vtemp;

	return vSrc;

fallback:
	vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
					 clip->x2, clip->y2);
	if (!vTemp)
		return NULL;

	if (!etnaviv_composite_to_pixmap(PictOpSrc, pict, NULL, *ppPixTemp,
					 src_topleft->x, src_topleft->y,
					 0, 0, clip->x2, clip->y2))
		return NULL;

	src_topleft->x = 0;
	src_topleft->y = 0;
	return vTemp;

copy_to_vtemp:
	vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
					 clip->x2, clip->y2);
	if (!vTemp)
		return NULL;

	if (!etnaviv_blend(etnaviv, clip, NULL, vTemp, vSrc, clip, 1,
			   *src_topleft, ZERO_OFFSET))
		return NULL;

	src_topleft->x = 0;
	src_topleft->y = 0;
	return vTemp;
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
 * backing pixmap.
 */
static Bool etnaviv_compute_composite_region(RegionPtr region,
	PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	if (pSrc->pDrawable) {
		xSrc += pSrc->pDrawable->x;
		ySrc += pSrc->pDrawable->y;
	}

	if (pMask && pMask->pDrawable) {
		xMask += pMask->pDrawable->x;
		yMask += pMask->pDrawable->y;
	}

	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	return miComputeCompositeRegion(region, pSrc, pMask, pDst,
					xSrc, ySrc, xMask, yMask,
					xDst, yDst, width, height);
}

static Bool etnaviv_Composite_Clear(PicturePtr pDst, struct etnaviv_de_op *op)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst;
	xPoint dst_offset;

	vDst = etnaviv_drawable_offset(pDst->pDrawable, &dst_offset);

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW))
		return FALSE;

	op->src = INIT_BLIT_PIX(vDst, vDst->pict_format, ZERO_OFFSET);
	op->dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	return TRUE;
}

static int etnaviv_accel_composite_srconly(PicturePtr pSrc, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xDst, INT16 yDst,
	struct etnaviv_de_op *final_op, struct etnaviv_blend_op *final_blend,
	RegionPtr region, PixmapPtr *ppPixTemp)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst, *vSrc;
	BoxRec clip_temp;
	xPoint src_topleft, dst_offset;

	if (pSrc->alphaMap)
		return FALSE;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		return FALSE;

	src_topleft.x = xSrc;
	src_topleft.y = ySrc;

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	/*
	 * Compute the temporary image clipping box, which is the
	 * clipping region extents without the destination offset.
	 */
	clip_temp = *RegionExtents(region);
	clip_temp.x1 -= xDst;
	clip_temp.y1 -= yDst;
	clip_temp.x2 -= xDst;
	clip_temp.y2 -= yDst;

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This may or may not be the temporary image,
	 * and vSrc->pict_format describes its format, including whether the
	 * alpha channel is valid.
	 */
	vSrc = etnaviv_acquire_src(pScreen, pSrc, &clip_temp, ppPixTemp,
				   &src_topleft, FALSE);
	if (!vSrc)
		return FALSE;

	/*
	 * Apply the same work-around for a non-alpha source as for
	 * a non-alpha destination.
	 */
	if (etnaviv_blend_src_alpha_normal(final_blend) &&
	    etnaviv_workaround_nonalpha(vSrc)) {
		final_blend->alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
		final_blend->src_alpha = 255;
	}

	vDst = etnaviv_drawable_offset(pDst->pDrawable, &dst_offset);

	src_topleft.x -= xDst + dst_offset.x;
	src_topleft.y -= yDst + dst_offset.y;

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	final_op->src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_topleft);
	final_op->dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	return TRUE;
}

static int etnaviv_accel_composite_masked(PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, struct etnaviv_de_op *final_op,
	struct etnaviv_blend_op *final_blend, RegionPtr region,
	PixmapPtr *ppPixTemp
#ifdef DEBUG_BLEND
	, CARD8 op
#endif
	)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst, *vSrc, *vMask, *vTemp;
	struct etnaviv_blend_op mask_op;
	BoxRec clip_temp;
	xPoint src_topleft, dst_offset, mask_offset;

	src_topleft.x = xSrc;
	src_topleft.y = ySrc;
	mask_offset.x = xMask;
	mask_offset.y = yMask;

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	/*
	 * Compute the temporary image clipping box, which is the
	 * clipping region extents without the destination offset.
	 */
	clip_temp = *RegionExtents(region);
	clip_temp.x1 -= xDst;
	clip_temp.y1 -= yDst;
	clip_temp.x2 -= xDst;
	clip_temp.y2 -= yDst;

	/* Get a temporary pixmap. */
	vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
					 clip_temp.x2, clip_temp.y2);
	if (!vTemp)
		return FALSE;

	if (pSrc->alphaMap || pMask->alphaMap)
		goto fallback;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		goto fallback;

	mask_op = etnaviv_composite_op[PictOpInReverse];

	if (pMask->componentAlpha) {
		/* Only PE2.0 can do component alpha blends. */
		if (!VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
			goto fallback;

		/* Adjust the mask blend (InReverse) to perform the blend. */
		mask_op.alpha_mode =
			VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL |
			VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL |
			VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ZERO) |
			VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_COLOR);
	}

	if (pMask->pDrawable) {
		int tx, ty;

		if (!transform_is_integer_translation(pMask->transform, &tx, &ty))
			goto fallback;

		mask_offset.x += tx;
		mask_offset.y += ty;

		/* We don't handle mask repeats (yet) */
		if (picture_needs_repeat(pMask, mask_offset.x, mask_offset.y,
					 clip_temp.x2, clip_temp.y2))
			goto fallback;

		mask_offset.x += pMask->pDrawable->x;
		mask_offset.y += pMask->pDrawable->y;
	} else {
		goto fallback;
	}

	/*
	 * Check whether the mask has a etna bo backing it.  If not,
	 * fallback to software for the mask operation.
	 */
	vMask = etnaviv_drawable_offset(pMask->pDrawable, &mask_offset);
	if (!vMask)
		goto fallback;

	etnaviv_set_format(vMask, pMask);

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This will always be the temporary image,
	 * which will always have alpha - which is required for the final
	 * blend.
	 */
	vSrc = etnaviv_acquire_src(pScreen, pSrc, &clip_temp, ppPixTemp,
				   &src_topleft, TRUE);
	if (!vSrc)
		goto fallback;

#ifdef DEBUG_BLEND
	etnaviv_batch_wait_commit(etnaviv, vSrc);
	etnaviv_batch_wait_commit(etnaviv, vMask);
	dump_vPix(etnaviv, vSrc, 1, "A-ISRC%2.2x-%p", op, pSrc);
	dump_vPix(etnaviv, vMask, 1, "A-MASK%2.2x-%p", op, pMask);
#endif

	/*
	 * Blend the source (in the temporary pixmap) with the mask
	 * via a InReverse op.
	 */
	if (!etnaviv_blend(etnaviv, &clip_temp, &mask_op, vSrc, vMask,
			   &clip_temp, 1, mask_offset, ZERO_OFFSET))
		return FALSE;

finish:
	vDst = etnaviv_drawable_offset(pDst->pDrawable, &dst_offset);

	src_topleft.x = -(xDst + dst_offset.x);
	src_topleft.y = -(yDst + dst_offset.y);

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	final_op->src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_topleft);
	final_op->dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	return TRUE;

fallback:
	/* Do the (src IN mask) in software instead */
	if (!etnaviv_composite_to_pixmap(PictOpSrc, pSrc, pMask, *ppPixTemp,
					 xSrc, ySrc, xMask, yMask,
					 clip_temp.x2, clip_temp.y2))
		return FALSE;

	vSrc = vTemp;
	goto finish;
}

/*
 * Handle cases where we can reduce a (s IN m) OP d operation to
 * a simpler s OP' d operation, possibly modifying OP' to use the
 * GPU global alpha features.
 */
static Bool etnaviv_accel_reduce_mask(struct etnaviv_blend_op *final_blend,
	CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst)
{
	uint32_t colour;

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
	if (op == PictOpOver &&
	    !pMask->componentAlpha &&
	    !PICT_FORMAT_A(pDst->format) &&
	    etnaviv_pict_solid_argb(pMask, &colour)) {
		uint32_t src_alpha_mode;

		/* Convert the colour to A8 */
		colour >>= 24;

		final_blend->src_alpha =
		final_blend->dst_alpha = colour;

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

		final_blend->alpha_mode = src_alpha_mode |
			VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_GLOBAL |
			VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_NORMAL) |
			VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_INVERSED);

		return TRUE;
	}

	return FALSE;
}

/*
 * A composite operation is: (pSrc IN pMask) OP pDst.  We always try
 * to perform an on-GPU "OP" where possible, which is handled by the
 * function below.  The source for this operation is determined by
 * sub-functions.
 */
static int etnaviv_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst;
	struct etnaviv_blend_op final_blend;
	struct etnaviv_de_op final_op;
	PixmapPtr pPixTemp = NULL;
	RegionRec region;
	int rc;

#ifdef DEBUG_BLEND
	etnaviv_debug_blend_op(__FUNCTION__, op, width, height,
			       pSrc, xSrc, ySrc,
			       pMask, xMask, yMask,
			       pDst, xDst, yDst);
#endif

	/* If the destination has an alpha map, fallback */
	if (pDst->alphaMap)
		return FALSE;

	/* If we can't do the op, there's no point going any further */
	if (op >= ARRAY_SIZE(etnaviv_composite_op))
		return FALSE;

	/* The destination pixmap must have a bo */
	vDst = etnaviv_drawable(pDst->pDrawable);
	if (!vDst)
		return FALSE;

	etnaviv_set_format(vDst, pDst);

	/* ... and the destination format must be supported */
	if (!etnaviv_dst_format_valid(etnaviv, vDst->pict_format))
		return FALSE;

	final_blend = etnaviv_composite_op[op];

	/*
	 * Apply the workaround for non-alpha destination.  The test order
	 * is important here: we only need the full workaround for non-
	 * PictOpClear operations, but we still need the format adjustment.
	 */
	if (etnaviv_workaround_nonalpha(vDst) && op != PictOpClear) {
		/*
		 * Destination alpha channel subsitution - this needs
		 * to happen before we modify the final blend for any
		 * optimisations, which may change the destination alpha
		 * value, such as in etnaviv_accel_reduce_mask().
		 */
		final_blend.alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_GLOBAL;
		final_blend.dst_alpha = 255;

		/*
		 * PE1.0 hardware contains a bug with destinations
		 * of RGB565, which force src.A to one.
		 */
		if (vDst->pict_format.format == DE_FORMAT_R5G6B5 &&
		    !VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20) &&
		    etnaviv_op_uses_source_alpha(&final_blend))
			return FALSE;
	}

	/*
	 * Compute the composite region from the source, mask and
	 * destination positions on their backing pixmaps.  The
	 * transformation is not applied at this stage.
	 */
	if (!etnaviv_compute_composite_region(&region, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height))
		return TRUE;

	miCompositeSourceValidate(pSrc);
	if (pMask)
		miCompositeSourceValidate(pMask);

	if (op == PictOpClear) {
		/* Short-circuit for PictOpClear */
		rc = etnaviv_Composite_Clear(pDst, &final_op);
	} else if (!pMask || etnaviv_accel_reduce_mask(&final_blend, op,
						       pSrc, pMask, pDst)) {
		rc = etnaviv_accel_composite_srconly(pSrc, pDst,
						     xSrc, ySrc,
						     xDst, yDst,
						     &final_op, &final_blend,
						     &region, &pPixTemp);
	} else {
		rc = etnaviv_accel_composite_masked(pSrc, pMask, pDst,
						    xSrc, ySrc, xMask, yMask,
						    xDst, yDst,
						    &final_op, &final_blend,
						    &region, &pPixTemp
#ifdef DEBUG_BLEND
						    , op
#endif
						    );
	}

	/*
	 * If we were successful with the previous step(s), complete
	 * the composite operation with the final accelerated blend op.
	 * The above functions will have done the necessary setup for
	 * this step.
	 */
	if (rc) {
		final_op.clip = RegionExtents(&region);
		final_op.blend_op = &final_blend;
		final_op.src_origin_mode = SRC_ORIGIN_RELATIVE;
		final_op.rop = 0xcc;
		final_op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
		final_op.brush = FALSE;

#ifdef DEBUG_BLEND
		etnaviv_batch_wait_commit(etnaviv, final_op.src.pixmap);
		dump_vPix(etnaviv, final_op.src.pixmap, 1,
			  "A-FSRC%2.2x-%p", op, pSrc);
		dump_vPix(etnaviv, final_op.dst.pixmap, 1,
			  "A-FDST%2.2x-%p", op, pDst);
#endif

		etnaviv_batch_start(etnaviv, &final_op);
		etnaviv_de_op(etnaviv, &final_op, RegionRects(&region),
			      RegionNumRects(&region));
		etnaviv_de_end(etnaviv);

#ifdef DEBUG_BLEND
		etnaviv_batch_wait_commit(etnaviv, final_op.dst.pixmap);
		dump_vPix(etnaviv, final_op.dst.pixmap,
			  PICT_FORMAT_A(pDst->format) != 0,
			  "A-DEST%2.2x-%p", op, pDst);
#endif
	}

	/* Destroy any temporary pixmap we may have allocated */
	if (pPixTemp)
		pScreen->DestroyPixmap(pPixTemp);

	RegionUninit(&region);

	return rc;
}

static Bool etnaviv_accel_Glyphs(CARD8 final_op, PicturePtr pSrc,
	PicturePtr pDst, PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc,
	int nlist, GlyphListPtr list, GlyphPtr *glyphs)
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
	if (!pMaskPixmap)
		goto destroy_gr;

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
	op.src_origin_mode = SRC_ORIGIN_NONE;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	pCurrent = NULL;
	for (grp = gr; grp < gr + n; grp++) {
		if (pCurrent != grp->picture) {
			PixmapPtr pPix = drawable_pixmap(grp->picture->pDrawable);
			struct etnaviv_pixmap *v = etnaviv_get_pixmap_priv(pPix);

			if (!etnaviv_map_gpu(etnaviv, v, GPU_ACCESS_RO))
				goto destroy_picture;

			if (pCurrent)
				etnaviv_de_end(etnaviv);

			prefetch(grp);

			op.src = INIT_BLIT_PIX(v, v->pict_format, ZERO_OFFSET);

			pCurrent = grp->picture;

			etnaviv_batch_start(etnaviv, &op);
		}

		prefetch(grp + 1);

		etnaviv_de_op_src_origin(etnaviv, &op, grp->glyph_pos,
					 &grp->dest_box);
	}
	etnaviv_de_end(etnaviv);

	free(gr);

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
	free(gr);
	return FALSE;

destroy_pixmap:
	pScreen->DestroyPixmap(pMaskPixmap);
destroy_gr:
	free(gr);
	return FALSE;
}

static void etnaviv_accel_glyph_upload(ScreenPtr pScreen, PicturePtr pDst,
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
		struct etnaviv_usermem_node *unode;
		char *buf, *src = src_pix->devPrivate.ptr;
		size_t size, align = maxt(VIVANTE_ALIGN_MASK, getpagesize());

		unode = malloc(sizeof(*unode));
		if (!unode)
			return;

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

		/* vdst will not go away while the server is running */
		unode->dst = vdst;
		unode->bo = usr;
		unode->mem = b;

		/* Add this to the list of usermem nodes to be freed */
		etnaviv_add_freemem(etnaviv, unode);

		op.src = INIT_BLIT_BO(usr, pitch,
				      etnaviv_pict_format(pSrc->format, FALSE),
				      src_offset);
	}

	box.x1 = x;
	box.y1 = y;
	box.x2 = x + width;
	box.y2 = y + height;

	etnaviv_set_format(vdst, pDst);

	if (!etnaviv_map_gpu(etnaviv, vdst, GPU_ACCESS_RW))
		return;

	op.dst = INIT_BLIT_PIX(vdst, vdst->pict_format, dst_offset);
	op.blend_op = NULL;
	op.clip = &box;
	op.src_origin_mode = SRC_ORIGIN_RELATIVE;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, &box, 1);
	etnaviv_de_end(etnaviv);
}

static void
etnaviv_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst,
	CARD16 width, CARD16 height)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pDrawable->pScreen);
	Bool ret;

	if (!etnaviv->force_fallback) {
		ret = etnaviv_accel_Composite(op, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height);
		if (ret)
			return;
	}
	unaccel_Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
			  xMask, yMask, xDst, yDst, width, height);
}

static void etnaviv_Glyphs(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
	PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int nlist,
	GlyphListPtr list, GlyphPtr * glyphs)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pDrawable->pScreen);

	if (etnaviv->force_fallback ||
	    !etnaviv_accel_Glyphs(op, pSrc, pDst, maskFormat,
				  xSrc, ySrc, nlist, list, glyphs))
		unaccel_Glyphs(op, pSrc, pDst, maskFormat,
			       xSrc, ySrc, nlist, list, glyphs);
}

static const unsigned glyph_formats[] = {
	PICT_a8r8g8b8,
	PICT_a8,
};

static Bool etnaviv_CreateScreenResources(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	Bool ret;

	pScreen->CreateScreenResources = etnaviv->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	if (ret) {
		size_t num = 1;

		/*
		 * If the 2D engine can do A8 targets, then enable
		 * PICT_a8 for glyph cache acceleration.
		 */
		if (VIV_FEATURE(etnaviv->conn, chipMinorFeatures0,
				2D_A8_TARGET)) {
			xf86DrvMsg(etnaviv->scrnIndex, X_INFO,
				   "etnaviv: A8 target supported\n");
			num = 2;
		} else {
			xf86DrvMsg(etnaviv->scrnIndex, X_INFO,
				   "etnaviv: A8 target not supported\n");
		}

		ret = glyph_cache_init(pScreen, etnaviv_accel_glyph_upload,
				       glyph_formats, num,
				       /* CREATE_PIXMAP_USAGE_TILE | */
				       CREATE_PIXMAP_USAGE_GPU);
	}
	return ret;
}

void etnaviv_render_screen_init(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);

	if (!etnaviv->force_fallback) {
		etnaviv->CreateScreenResources = pScreen->CreateScreenResources;
		pScreen->CreateScreenResources = etnaviv_CreateScreenResources;
	}

	etnaviv->Composite = ps->Composite;
	ps->Composite = etnaviv_Composite;
	etnaviv->Glyphs = ps->Glyphs;
	ps->Glyphs = etnaviv_Glyphs;
	etnaviv->UnrealizeGlyph = ps->UnrealizeGlyph;
	etnaviv->Triangles = ps->Triangles;
	ps->Triangles = unaccel_Triangles;
	etnaviv->Trapezoids = ps->Trapezoids;
	ps->Trapezoids = unaccel_Trapezoids;
	etnaviv->AddTriangles = ps->AddTriangles;
	ps->AddTriangles = unaccel_AddTriangles;
	etnaviv->AddTraps = ps->AddTraps;
	ps->AddTraps = unaccel_AddTraps;
}

void etnaviv_render_close_screen(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);

	/* Restore the Pointers */
	ps->Composite = etnaviv->Composite;
	ps->Glyphs = etnaviv->Glyphs;
	ps->UnrealizeGlyph = etnaviv->UnrealizeGlyph;
	ps->Triangles = etnaviv->Triangles;
	ps->Trapezoids = etnaviv->Trapezoids;
	ps->AddTriangles = etnaviv->AddTriangles;
	ps->AddTraps = etnaviv->AddTraps;
}
#endif
