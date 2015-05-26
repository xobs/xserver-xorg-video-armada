/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <armada_bufmgr.h>

#include "armada_accel.h"
#include "common_drm_dri2.h"

#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "compat-api.h"

#include <etnaviv/etna.h>
#include <etnaviv/state_2d.xml.h>

#include "cpu_access.h"
#include "fbutil.h"
#include "gal_extension.h"
#include "glyph_cache.h"
#include "mark.h"
#include "pixmaputil.h"
#include "unaccel.h"

#include "etnaviv_accel.h"
#include "etnaviv_dri2.h"
#include "etnaviv_utils.h"
#include "etnaviv_xv.h"

etnaviv_Key etnaviv_pixmap_index;
etnaviv_Key etnaviv_screen_index;
int etnaviv_private_index = -1;

enum {
	OPTION_DRI,
};

const OptionInfoRec etnaviv_options[] = {
	{ OPTION_DRI,		"DRI",		OPTV_BOOLEAN, {0}, TRUE },
	{ -1,			NULL,		OPTV_NONE,    {0}, FALSE }
};

static void etnaviv_free_vpix(struct etnaviv *etnaviv,
	struct etnaviv_pixmap *vPix)
{
	if (vPix->etna_bo) {
		struct etna_bo *etna_bo = vPix->etna_bo;

		if (!vPix->bo && vPix->state & ST_CPU_RW)
			etna_bo_cpu_fini(etna_bo);
		etna_bo_del(etnaviv->conn, etna_bo, NULL);
	}
	if (vPix->bo)
		drm_armada_bo_put(vPix->bo);
	free(vPix);
}

void etnaviv_free_busy_vpix(struct etnaviv *etnaviv)
{
	struct etnaviv_pixmap *i, *n;

	xorg_list_for_each_entry_safe(i, n, &etnaviv->busy_free_list, busy_node) {
		if (i->batch_state == B_NONE) {
			xorg_list_del(&i->busy_node);
			etnaviv_free_vpix(etnaviv, i);
		}
	}
}

void etnaviv_finish_fences(struct etnaviv *etnaviv, uint32_t fence)
{
	struct etnaviv_pixmap *i, *n;
	int ret;

	xorg_list_for_each_entry_safe(i, n, &etnaviv->fence_head, batch_node) {
		assert(i->batch_state == B_FENCED);
		if (VIV_FENCE_BEFORE(fence, i->fence)) {
			fence = i->fence;
			ret = viv_fence_finish(etnaviv->conn, fence, 0);
			if (ret != VIV_STATUS_OK)
				break;
			etnaviv->last_fence = fence;
		}
		xorg_list_del(&i->batch_node);
		i->batch_state = B_NONE;
	}
}

static CARD32 etnaviv_cache_expire(OsTimerPtr timer, CARD32 time, pointer arg)
{
	return 0;
}

static void etnaviv_free_pixmap(PixmapPtr pixmap)
{
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	if (vPix) {
		struct etnaviv *etnaviv;

		etnaviv = etnaviv_get_screen_priv(pixmap->drawable.pScreen);

		switch (vPix->batch_state) {
		case B_NONE:
			/*
			 * The pixmap may be only on the CPU, or it may be on
			 * the GPU but we have already seen a commit+stall.
			 * We can just free this pixmap.
			 */
			etnaviv_free_vpix(etnaviv, vPix);
			break;

		case B_FENCED:
			/*
			 * The pixmap is part of a batch of submitted GPU
			 * operations.  Check whether it has completed.
			 */
			if (VIV_FENCE_BEFORE_EQ(vPix->fence, etnaviv->last_fence)) {
				xorg_list_del(&vPix->batch_node);
				etnaviv_free_vpix(etnaviv, vPix);
				break;
			}

		case B_PENDING:
			/*
			 * The pixmap is part of a batch of unsubmitted GPU
			 * operations.  Place it on the busy_free_list.
			 */
			xorg_list_append(&vPix->busy_node, &etnaviv->busy_free_list);
			vPix->free_time = currentTime.milliseconds;
			break;
		}
	}
}

/*
 * We are about to respond to a client.  Ensure that all pending rendering
 * is flushed to the GPU prior to the response being delivered.
 */
static void etnaviv_flush_callback(CallbackListPtr *list, pointer user_data,
	pointer call_data)
{
	ScrnInfoPtr pScrn = user_data;
	struct etnaviv *etnaviv = pScrn->privates[etnaviv_private_index].ptr;
	uint32_t fence;

	if (pScrn->vtSema && !xorg_list_is_empty(&etnaviv->batch_head))
		etnaviv_commit(etnaviv, FALSE, &fence);
}

static struct etnaviv_pixmap *etnaviv_alloc_pixmap(PixmapPtr pixmap,
	struct etnaviv_format fmt)
{
	struct etnaviv_pixmap *vpix;

	vpix = calloc(1, sizeof *vpix);
	if (vpix) {
		vpix->width = pixmap->drawable.width;
		vpix->height = pixmap->drawable.height;
		vpix->pitch = pixmap->devKind;
		vpix->format = fmt;
	}
	return vpix;
}


/* Determine whether this GC and target Drawable can be accelerated */
static Bool etnaviv_GC_can_accel(GCPtr pGC, DrawablePtr pDrawable)
{
	PixmapPtr pixmap = drawable_pixmap(pDrawable);
	if (!etnaviv_get_pixmap_priv(pixmap))
		return FALSE;

	/* Must be full-planes */
	return !pGC || fb_full_planemask(pDrawable, pGC->planemask);
}

static Bool etnaviv_GCfill_can_accel(GCPtr pGC, DrawablePtr pDrawable)
{
	switch (pGC->fillStyle) {
	case FillSolid:
		return TRUE;

	case FillTiled:
		/* Single pixel tiles are just solid colours */
		if (pGC->tileIsPixel)
			return TRUE;

		/* If the tile pixmap is a single pixel, it's also a solid fill */
		if (pGC->tile.pixmap->drawable.width == 1 &&
		    pGC->tile.pixmap->drawable.height == 1)
			return TRUE;

		/* In theory, we could do !tileIsPixel as well, which means
		 * copying the tile (possibly) multiple times to the drawable.
		 * This is something we should do, especially if the size of
		 * the tile matches the size of the drawable and the tile
		 * offsets are zero (iow, it's a plain copy.)
		 */
		return FALSE;

	default:
		return FALSE;
	}
}


static void
etnaviv_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n, DDXPointPtr ppt,
	int *pwidth, int fSorted)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv->force_fallback ||
	    !etnaviv_GCfill_can_accel(pGC, pDrawable) ||
	    !etnaviv_accel_FillSpans(pDrawable, pGC, n, ppt, pwidth, fSorted))
		unaccel_FillSpans(pDrawable, pGC, n, ppt, pwidth, fSorted);
}

static void
etnaviv_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth, int x, int y,
	int w, int h, int leftPad, int format, char *bits)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv->force_fallback ||
	    !etnaviv_accel_PutImage(pDrawable, pGC, depth, x, y, w, h, leftPad,
				    format, bits))
		unaccel_PutImage(pDrawable, pGC, depth, x, y, w, h, leftPad,
					 format, bits);
}

static RegionPtr
etnaviv_CopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	int srcx, int srcy, int w, int h, int dstx, int dsty)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDst));

	if (etnaviv->force_fallback)
		return unaccel_CopyArea(pSrc, pDst, pGC, srcx, srcy, w, h,
					dstx, dsty);

	return miDoCopy(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty,
			etnaviv_accel_CopyNtoN, 0, NULL);
}

static void
etnaviv_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt,
	DDXPointPtr ppt)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv->force_fallback ||
	    !etnaviv_GCfill_can_accel(pGC, pDrawable) ||
	    !etnaviv_accel_PolyPoint(pDrawable, pGC, mode, npt, ppt))
		unaccel_PolyPoint(pDrawable, pGC, mode, npt, ppt);
}

static void
etnaviv_PolyLines(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt,
	DDXPointPtr ppt)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv->force_fallback ||
	    pGC->lineWidth != 0 || pGC->lineStyle != LineSolid ||
	    pGC->fillStyle != FillSolid ||
	    !etnaviv_accel_PolyLines(pDrawable, pGC, mode, npt, ppt))
		unaccel_PolyLines(pDrawable, pGC, mode, npt, ppt);
}

static void
etnaviv_PolySegment(DrawablePtr pDrawable, GCPtr pGC, int nseg, xSegment *pSeg)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv->force_fallback ||
	    pGC->lineWidth != 0 || pGC->lineStyle != LineSolid ||
	    pGC->fillStyle != FillSolid ||
	    !etnaviv_accel_PolySegment(pDrawable, pGC, nseg, pSeg))
		unaccel_PolySegment(pDrawable, pGC, nseg, pSeg);
}

static void
etnaviv_PolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrect,
	xRectangle * prect)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);
	PixmapPtr pPix = drawable_pixmap(pDrawable);

	if (etnaviv->force_fallback ||
	    (pPix->drawable.width == 1 && pPix->drawable.height == 1))
		goto fallback;

	assert(etnaviv_GC_can_accel(pGC, pDrawable));

	if (etnaviv_GCfill_can_accel(pGC, pDrawable)) {
		if (etnaviv_accel_PolyFillRectSolid(pDrawable, pGC, nrect, prect))
			return;
	} else if (pGC->fillStyle == FillTiled) {
		if (etnaviv_accel_PolyFillRectTiled(pDrawable, pGC, nrect, prect))
			return;
	}

 fallback:
	unaccel_PolyFillRect(pDrawable, pGC, nrect, prect);
}

static GCOps etnaviv_GCOps = {
	etnaviv_FillSpans,
	unaccel_SetSpans,
	etnaviv_PutImage,
	etnaviv_CopyArea,
	unaccel_CopyPlane,
	etnaviv_PolyPoint,
	etnaviv_PolyLines,
	etnaviv_PolySegment,
	miPolyRectangle,
	miPolyArc,
	miFillPolygon,
	etnaviv_PolyFillRect,
	miPolyFillArc,
	miPolyText8,
	miPolyText16,
	miImageText8,
	miImageText16,
	unaccel_ImageGlyphBlt,
	unaccel_PolyGlyphBlt,
	unaccel_PushPixels
};

static GCOps etnaviv_unaccel_GCOps = {
	unaccel_FillSpans,
	unaccel_SetSpans,
	unaccel_PutImage,
	unaccel_CopyArea,
	unaccel_CopyPlane,
	unaccel_PolyPoint,
	unaccel_PolyLines,
	unaccel_PolySegment,
	miPolyRectangle,
	miPolyArc,
	miFillPolygon,
	unaccel_PolyFillRect,
	miPolyFillArc,
	miPolyText8,
	miPolyText16,
	miImageText8,
	miImageText16,
	unaccel_ImageGlyphBlt,
	unaccel_PolyGlyphBlt,
	unaccel_PushPixels
};

static void
etnaviv_ValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr pDrawable)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

#ifdef FB_24_32BIT
	if (changes & GCTile && fbGetRotatedPixmap(pGC)) {
		pGC->pScreen->DestroyPixmap(fbGetRotatedPixmap(pGC));
		fbGetRotatedPixmap(pGC) = NULL;
	}
	if (pGC->fillStyle == FillTiled) {
		PixmapPtr pOldTile = pGC->tile.pixmap;
		PixmapPtr pNewTile;

		if (pOldTile->drawable.bitsPerPixel != pDrawable->bitsPerPixel) {
			pNewTile = fbGetRotatedPixmap(pGC);
			if (!pNewTile || pNewTile->drawable.bitsPerPixel != pDrawable->bitsPerPixel) {
				if (pNewTile)
					pGC->pScreen->DestroyPixmap(pNewTile);
				prepare_cpu_drawable(&pOldTile->drawable, CPU_ACCESS_RO);
				pNewTile = fb24_32ReformatTile(pOldTile, pDrawable->bitsPerPixel);
				finish_cpu_drawable(&pOldTile->drawable, CPU_ACCESS_RO);
			}
			if (pNewTile) {
				fbGetRotatedPixmap(pGC) = pOldTile;
				pGC->tile.pixmap = pNewTile;
				changes |= GCTile;
			}
		}
	}
#endif
	if (changes & GCTile) {
		if (!pGC->tileIsPixel &&
		    FbEvenTile(pGC->tile.pixmap->drawable.width *
			       pDrawable->bitsPerPixel)) {
			prepare_cpu_drawable(&pGC->tile.pixmap->drawable, CPU_ACCESS_RW);
			fbPadPixmap(pGC->tile.pixmap);
			finish_cpu_drawable(&pGC->tile.pixmap->drawable, CPU_ACCESS_RW);
		}
		/* mask out gctile changes now that we've done the work */
		changes &= ~GCTile;
	}
	if (changes & GCStipple && pGC->stipple) {
		prepare_cpu_drawable(&pGC->stipple->drawable, CPU_ACCESS_RW);
		fbValidateGC(pGC, changes, pDrawable);
		finish_cpu_drawable(&pGC->stipple->drawable, CPU_ACCESS_RW);
	} else {
		fbValidateGC(pGC, changes, pDrawable);
	}

	/*
	 * Select the GC ops depending on whether we have any
	 * chance to accelerate with this GC.
	 */
	if (!etnaviv->force_fallback && etnaviv_GC_can_accel(pGC, pDrawable))
		pGC->ops = &etnaviv_GCOps;
	else
		pGC->ops = &etnaviv_unaccel_GCOps;
}

static GCFuncs etnaviv_GCFuncs = {
	etnaviv_ValidateGC,
	miChangeGC,
	miCopyGC,
	miDestroyGC,
	miChangeClip,
	miDestroyClip,
	miCopyClip
};


static Bool etnaviv_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif
	PixmapPtr pixmap;

	DeleteCallback(&FlushCallback, etnaviv_flush_callback, pScrn);

#ifdef RENDER
	/* Restore the Pointers */
	ps->Composite = etnaviv->Composite;
	ps->Glyphs = etnaviv->Glyphs;
	ps->UnrealizeGlyph = etnaviv->UnrealizeGlyph;
	ps->Triangles = etnaviv->Triangles;
	ps->Trapezoids = etnaviv->Trapezoids;
	ps->AddTriangles = etnaviv->AddTriangles;
	ps->AddTraps = etnaviv->AddTraps;
#endif

	pScreen->CloseScreen = etnaviv->CloseScreen;
	pScreen->GetImage = etnaviv->GetImage;
	pScreen->GetSpans = etnaviv->GetSpans;
	pScreen->ChangeWindowAttributes = etnaviv->ChangeWindowAttributes;
	pScreen->CopyWindow = etnaviv->CopyWindow;
	pScreen->CreatePixmap = etnaviv->CreatePixmap;
	pScreen->DestroyPixmap = etnaviv->DestroyPixmap;
	pScreen->CreateGC = etnaviv->CreateGC;
	pScreen->BitmapToRegion = etnaviv->BitmapToRegion;
	pScreen->BlockHandler = etnaviv->BlockHandler;

#ifdef HAVE_DRI2
	etnaviv_dri2_CloseScreen(CLOSE_SCREEN_ARGS);
#endif

	pixmap = pScreen->GetScreenPixmap(pScreen);
	etnaviv_free_pixmap(pixmap);
	etnaviv_set_pixmap_priv(pixmap, NULL);

	etnaviv_accel_shutdown(etnaviv);

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

static void
etnaviv_GetImage(DrawablePtr pDrawable, int x, int y, int w, int h,
	unsigned int format, unsigned long planeMask, char *d)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

	if (etnaviv->force_fallback ||
	    !etnaviv_accel_GetImage(pDrawable, x, y, w, h, format, planeMask,
				    d))
		unaccel_GetImage(pDrawable, x, y, w, h, format, planeMask, d);
}

static void
etnaviv_CopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
	PixmapPtr pPixmap = pWin->drawable.pScreen->GetWindowPixmap(pWin);
	RegionRec rgnDst;
	int dx, dy;

	dx = ptOldOrg.x - pWin->drawable.x;
	dy = ptOldOrg.y - pWin->drawable.y;
	RegionTranslate(prgnSrc, -dx, -dy);
	RegionInit(&rgnDst, NullBox, 0);
	RegionIntersect(&rgnDst, &pWin->borderClip, prgnSrc);

#ifdef COMPOSITE
	if (pPixmap->screen_x || pPixmap->screen_y)
		RegionTranslate(&rgnDst, -pPixmap->screen_x,
				-pPixmap->screen_y);
#endif

	miCopyRegion(&pPixmap->drawable, &pPixmap->drawable, NULL,
		     &rgnDst, dx, dy, etnaviv_accel_CopyNtoN, 0, NULL);

	RegionUninit(&rgnDst);
}

#ifdef HAVE_DRI2
Bool etnaviv_pixmap_flink(PixmapPtr pixmap, uint32_t *name)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pixmap->drawable.pScreen);
	struct etnaviv_pixmap *vpix = etnaviv_get_pixmap_priv(pixmap);
	Bool ret = FALSE;

	if (!vpix)
		return FALSE;

	if (vpix->name) {
		*name = vpix->name;
		ret = TRUE;
	} else if (vpix->bo && !drm_armada_bo_flink(vpix->bo, name)) {
		ret = TRUE;
	} else {
		struct drm_gem_flink flink = {
			.handle = etna_bo_handle(vpix->etna_bo),
		};

		if (!drmIoctl(etnaviv->conn->fd, DRM_IOCTL_GEM_FLINK, &flink)) {
			*name = flink.name;
			ret = TRUE;
		}
	}

	return ret;
}
#endif

static Bool etnaviv_alloc_armada_bo(ScreenPtr pScreen, struct etnaviv *etnaviv,
	PixmapPtr pixmap, int w, int h, struct etnaviv_format fmt,
	unsigned usage_hint)
{
	struct etnaviv_pixmap *vpix;
	struct drm_armada_bo *bo;
	unsigned pitch, bpp = pixmap->drawable.bitsPerPixel;

#ifndef HAVE_DRM_ARMADA_BO_CREATE_SIZE
	bo = drm_armada_bo_create(etnaviv->bufmgr, w, h, bpp);
	if (!bo) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: failed to allocate armada bo for %dx%d %dbpp\n",
			   w, h, bpp);
		return FALSE;
	}

	pitch = bo->pitch;
#else
	unsigned size;

	if (usage_hint & CREATE_PIXMAP_USAGE_TILE) {
		pitch = etnaviv_tile_pitch(w, bpp);
		size = pitch * etnaviv_tile_height(h);
		fmt.tile = 1;
	} else {
		pitch = etnaviv_pitch(w, bpp);
		size = pitch * h;
	}

	size = ALIGN(size, 4096);

	bo = drm_armada_bo_create_size(etnaviv->bufmgr, size);
	if (!bo) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: failed to allocate armada bo for %dx%d %dbpp\n",
			   w, h, bpp);
		return FALSE;
	}
#endif

	if (drm_armada_bo_map(bo))
		goto free_bo;

	/*
	 * Do not store our data pointer in the pixmap - only do so (via
	 * prepare_cpu_drawable()) when required to directly access the
	 * pixmap.  This provides us a way to validate that we do not have
	 * any spurious unchecked accesses to the pixmap data while the GPU
	 * has ownership of the pixmap.
	 */
	pScreen->ModifyPixmapHeader(pixmap, w, h, 0, 0, pitch, NULL);

	vpix = etnaviv_alloc_pixmap(pixmap, fmt);
	if (!vpix)
		goto free_bo;

	vpix->bo = bo;

	etnaviv_set_pixmap_priv(pixmap, vpix);

#ifdef DEBUG_PIXMAP
	dbg("Pixmap %p: vPix=%p armada_bo=%p format=%u/%u/%u\n",
	    pixmap, vPix, bo, fmt.format, fmt.swizzle, fmt.tile);
#endif

	return TRUE;

 free_bo:
	drm_armada_bo_put(bo);
	return FALSE;
}

static Bool etnaviv_alloc_etna_bo(ScreenPtr pScreen, struct etnaviv *etnaviv,
	PixmapPtr pixmap, int w, int h, struct etnaviv_format fmt,
	unsigned usage_hint)
{
	struct etnaviv_pixmap *vpix;
	struct etna_bo *etna_bo;
	unsigned pitch, size, bpp = pixmap->drawable.bitsPerPixel;

	if (usage_hint & CREATE_PIXMAP_USAGE_TILE) {
		pitch = etnaviv_tile_pitch(w, bpp);
		size = pitch * etnaviv_tile_height(h);
		fmt.tile = 1;
	} else {
		pitch = etnaviv_pitch(w, bpp);
		size = pitch * h;
	}

	etna_bo = etna_bo_new(etnaviv->conn, size,
			DRM_ETNA_GEM_TYPE_BMP | DRM_ETNA_GEM_CACHE_WBACK);
	if (!etna_bo) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: failed to allocate bo for %dx%d %dbpp\n",
			   w, h, bpp);
		return FALSE;
	}

	/*
	 * Do not store our data pointer in the pixmap - only do so (via
	 * prepare_cpu_drawable()) when required to directly access the
	 * pixmap.  This provides us a way to validate that we do not have
	 * any spurious unchecked accesses to the pixmap data while the GPU
	 * has ownership of the pixmap.
	 */
	pScreen->ModifyPixmapHeader(pixmap, w, h, 0, 0, pitch, NULL);

	vpix = etnaviv_alloc_pixmap(pixmap, fmt);
	if (!vpix)
		goto free_bo;

	vpix->etna_bo = etna_bo;

	etnaviv_set_pixmap_priv(pixmap, vpix);

#ifdef DEBUG_PIXMAP
	dbg("Pixmap %p: vPix=%p etna_bo=%p format=%u/%u/%u\n",
	    pixmap, vPix, etna_bo, fmt.format, fmt.swizzle, fmt.tile);
#endif

	return TRUE;

 free_bo:
	etna_bo_del(etnaviv->conn, etna_bo, NULL);
	return FALSE;
}

static PixmapPtr etnaviv_CreatePixmap(ScreenPtr pScreen, int w, int h,
	int depth, unsigned usage_hint)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_format fmt = { .swizzle = DE_SWIZZLE_ARGB, };
	PixmapPtr pixmap;

	if (w > 32768 || h > 32768)
		return NullPixmap;

	if (depth == 1 || etnaviv->force_fallback)
		goto fallback;

	if (usage_hint == CREATE_PIXMAP_USAGE_GLYPH_PICTURE &&
	    w <= 32 && h <= 32)
		goto fallback;

	pixmap = etnaviv->CreatePixmap(pScreen, 0, 0, depth, usage_hint);
	if (pixmap == NullPixmap || w == 0 || h == 0)
		return pixmap;

	/* Create the appropriate format for this pixmap */
	switch (pixmap->drawable.bitsPerPixel) {
	case 8:
		if (usage_hint & CREATE_PIXMAP_USAGE_GPU) {
			fmt.format = DE_FORMAT_A8;
			break;
		}
		goto fallback_free_pix;

	case 16:
		if (pixmap->drawable.depth == 15)
			fmt.format = DE_FORMAT_A1R5G5B5;
		else
			fmt.format = DE_FORMAT_R5G6B5;
		break;

	case 32:
		fmt.format = DE_FORMAT_A8R8G8B8;
		break;

	default:
		goto fallback_free_pix;
	}

	if (etnaviv->bufmgr) {
		if (!etnaviv_alloc_armada_bo(pScreen, etnaviv, pixmap,
					     w, h, fmt, usage_hint))
			goto fallback_free_pix;
	} else {
		if (!etnaviv_alloc_etna_bo(pScreen, etnaviv, pixmap,
					   w, h, fmt, usage_hint))
			goto fallback_free_pix;
	}
	goto out;

 fallback_free_pix:
	etnaviv->DestroyPixmap(pixmap);
 fallback:
	/* GPU pixmaps must fail rather than fall back */
	if (usage_hint & CREATE_PIXMAP_USAGE_GPU)
		return NULL;

	pixmap = etnaviv->CreatePixmap(pScreen, w, h, depth, usage_hint);

 out:
#ifdef DEBUG_PIXMAP
	dbg("Created pixmap %p %dx%d %d %d %x\n",
	    pixmap, w, h, depth, pixmap->drawable.bitsPerPixel, usage);
#endif

	return pixmap;
}

static Bool etnaviv_DestroyPixmap(PixmapPtr pixmap)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pixmap->drawable.pScreen);
	if (pixmap->refcnt == 1) {
#ifdef DEBUG_PIXMAP
		dbg("Destroying pixmap %p\n", pixmap);
#endif
		etnaviv_free_pixmap(pixmap);
		etnaviv_set_pixmap_priv(pixmap, NULL);
	}
	return etnaviv->DestroyPixmap(pixmap);
}

static Bool etnaviv_CreateGC(GCPtr pGC)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pGC->pScreen);
	Bool ret;

	ret = etnaviv->CreateGC(pGC);
	if (ret)
		pGC->funcs = &etnaviv_GCFuncs;

	return ret;
}

/* Commit any pending GPU operations */
static void etnaviv_BlockHandler(BLOCKHANDLER_ARGS_DECL)
{
	SCREEN_PTR(arg);
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	uint32_t fence;

	if (!xorg_list_is_empty(&etnaviv->batch_head))
		etnaviv_commit(etnaviv, FALSE, &fence);

	mark_flush();

	pScreen->BlockHandler = etnaviv->BlockHandler;
	pScreen->BlockHandler(BLOCKHANDLER_ARGS);
	etnaviv->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = etnaviv_BlockHandler;

	/*
	 * Check for any completed fences.  If the fence numberspace
	 * wraps, it can allow an idle pixmap to become "active" again.
	 * This prevents that occuring.
	 */
	if (!xorg_list_is_empty(&etnaviv->fence_head))
		etnaviv_finish_fences(etnaviv, etnaviv->last_fence);

	/*
	 * And now try to expire any remaining busy-free pixmaps
	 */
	if (!xorg_list_is_empty(&etnaviv->busy_free_list)) {
		UpdateCurrentTimeIf();
		etnaviv_free_busy_vpix(etnaviv);
		if (!xorg_list_is_empty(&etnaviv->busy_free_list)) {
			etnaviv->cache_timer = TimerSet(etnaviv->cache_timer,
							0, 500,
							etnaviv_cache_expire,
							etnaviv);
		}
	}
}

#ifdef RENDER
static void
etnaviv_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst,
	CARD16 width, CARD16 height)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pDrawable->pScreen);
	Bool ret;

	if (!etnaviv->force_fallback) {
		unsigned src_repeat = pSrc->repeat;

		ret = etnaviv_accel_Composite(op, pSrc, pMask, pDst,
					      xSrc, ySrc,
					      xMask, yMask,
					      xDst, yDst, width, height);
		pSrc->repeat = src_repeat;
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
#endif

static Bool etnaviv_pre_init(ScrnInfoPtr pScrn, int drm_fd)
{
	struct etnaviv *etnaviv;
	OptionInfoPtr options;

	etnaviv = calloc(1, sizeof *etnaviv);
	if (!etnaviv)
		return FALSE;

	options = malloc(sizeof(etnaviv_options));
	if (!options) {
		free(etnaviv);
		return FALSE;
	}

	memcpy(options, etnaviv_options, sizeof(etnaviv_options));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, options);

#ifdef HAVE_DRI2
	etnaviv->dri2_enabled = xf86ReturnOptValBool(options, OPTION_DRI, TRUE);
#endif

	etnaviv->scrnIndex = pScrn->scrnIndex;

	if (etnaviv_private_index == -1)
		etnaviv_private_index = xf86AllocateScrnInfoPrivateIndex();

	pScrn->privates[etnaviv_private_index].ptr = etnaviv;

	free(options);

	return TRUE;
}

static Bool etnaviv_ScreenInit(ScreenPtr pScreen, struct drm_armada_bufmgr *mgr)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif
	struct etnaviv *etnaviv = pScrn->privates[etnaviv_private_index].ptr;

	if (!etnaviv_CreateKey(&etnaviv_pixmap_index, PRIVATE_PIXMAP) ||
	    !etnaviv_CreateKey(&etnaviv_screen_index, PRIVATE_SCREEN))
		return FALSE;

	etnaviv->bufmgr = mgr;

	if (!etnaviv_accel_init(etnaviv))
		goto fail_accel;

	xorg_list_init(&etnaviv->batch_head);
	xorg_list_init(&etnaviv->fence_head);
	xorg_list_init(&etnaviv->busy_free_list);

	etnaviv_set_screen_priv(pScreen, etnaviv);

	if (!AddCallback(&FlushCallback, etnaviv_flush_callback, pScrn)) {
		etnaviv_accel_shutdown(etnaviv);
		goto fail_accel;
	}

#ifdef HAVE_DRI2
	if (!etnaviv->dri2_enabled) {
		xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
			   "direct rendering: disabled\n");
	} else {
		const char *name;
		drmVersionPtr version;
		int dri_fd = -1;

		/*
		 * Use drmGetVersion() to check whether the etnaviv fd
		 * is a DRM fd.
		 */
		version = drmGetVersion(etnaviv->conn->fd);
		if (version) {
			drmFreeVersion(version);

			/* etnadrm fd, etnadrm buffer management */
			dri_fd = etnaviv->conn->fd;
			name = "etnadrm";
		} else if (mgr) {
			/* armada fd, armada buffer management */
			dri_fd = GET_DRM_INFO(pScrn)->fd;
			etnaviv->dri2_armada = TRUE;
			name = "etnaviv";
		}

		if (dri_fd == -1) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "direct rendering: unusuable devices\n");
		} else if (!etnaviv_dri2_ScreenInit(pScreen, dri_fd, name)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "direct rendering: failed\n");
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "direct rendering: DRI2 enabled\n");
		}
	}
#endif

	etnaviv->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = etnaviv_CloseScreen;
	etnaviv->GetImage = pScreen->GetImage;
	pScreen->GetImage = etnaviv_GetImage;
	etnaviv->GetSpans = pScreen->GetSpans;
	pScreen->GetSpans = unaccel_GetSpans;
	etnaviv->ChangeWindowAttributes = pScreen->ChangeWindowAttributes;
	pScreen->ChangeWindowAttributes = unaccel_ChangeWindowAttributes;
	etnaviv->CopyWindow = pScreen->CopyWindow;
	pScreen->CopyWindow = etnaviv_CopyWindow;
	etnaviv->CreatePixmap = pScreen->CreatePixmap;
	pScreen->CreatePixmap = etnaviv_CreatePixmap;
	etnaviv->DestroyPixmap = pScreen->DestroyPixmap;
	pScreen->DestroyPixmap = etnaviv_DestroyPixmap;
	etnaviv->CreateGC = pScreen->CreateGC;
	pScreen->CreateGC = etnaviv_CreateGC;
	etnaviv->BitmapToRegion = pScreen->BitmapToRegion;
	pScreen->BitmapToRegion = unaccel_BitmapToRegion;
	etnaviv->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = etnaviv_BlockHandler;

#ifdef RENDER
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
#endif
	return TRUE;

fail_accel:
	free(etnaviv);
	return FALSE;
}

/* Scanout pixmaps are never tiled. */
static Bool etnaviv_import_dmabuf(ScreenPtr pScreen, PixmapPtr pPixmap, int fd)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_format fmt = { .swizzle = DE_SWIZZLE_ARGB, };
	struct etnaviv_pixmap *vpix = etnaviv_get_pixmap_priv(pPixmap);
	struct etna_bo *bo;

	if (vpix) {
		etnaviv_free_pixmap(pPixmap);
		etnaviv_set_pixmap_priv(pPixmap, NULL);
	}

	switch (pPixmap->drawable.bitsPerPixel) {
	case 16:
		if (pPixmap->drawable.depth == 15)
			fmt.format = DE_FORMAT_A1R5G5B5;
		else
			fmt.format = DE_FORMAT_R5G6B5;
		break;

	case 32:
		fmt.format = DE_FORMAT_A8R8G8B8;
		break;

	default:
		return TRUE;
	}

	bo = etna_bo_from_dmabuf(etnaviv->conn, fd, PROT_READ | PROT_WRITE);
	if (!bo) {
		xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
			   "etnaviv: gpu dmabuf map failed: %s\n",
			   strerror(errno));
		return FALSE;
	}

	vpix = etnaviv_alloc_pixmap(pPixmap, fmt);
	if (!vpix) {
		etna_bo_del(etnaviv->conn, bo, NULL);
		return FALSE;
	}

	vpix->etna_bo = bo;

	/*
	 * Pixmaps imported via dmabuf are write-combining, so don't
	 * need CPU cache state tracking.  We still need to track
	 * whether we have operations outstanding on the GPU.
	 */
	vpix->state |= ST_DMABUF;

	etnaviv_set_pixmap_priv(pPixmap, vpix);

#ifdef DEBUG_PIXMAP
	dbg("Pixmap %p: vPix=%p etna_bo=%p format=%u/%u/%u\n",
	    pixmap, vPix, bo, fmt.format, fmt.swizzle, fmt.tile);
#endif

	return TRUE;
}

static void etnaviv_attach_name(ScreenPtr pScreen, PixmapPtr pPixmap,
	uint32_t name)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pPixmap);

	/* If we are using our KMS DRM for buffer management, save its name */
	if (etnaviv->dri2_armada && vPix)
		vPix->name = name;
}

const struct armada_accel_ops etnaviv_ops = {
	.pre_init	= etnaviv_pre_init,
	.screen_init	= etnaviv_ScreenInit,
	.import_dmabuf	= etnaviv_import_dmabuf,
	.attach_name	= etnaviv_attach_name,
	.free_pixmap	= etnaviv_free_pixmap,
	.xv_init	= etnaviv_xv_init,
};
