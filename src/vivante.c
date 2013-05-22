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
#include <unistd.h>

#include <armada_bufmgr.h>

#include "armada_drm.h"

#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include <gc_hal.h>

#include "vivante.h"
#include "vivante_accel.h"
#include "vivante_dri2.h"
#include "vivante_unaccel.h"
#include "vivante_utils.h"

vivante_Key vivante_pixmap_index;
vivante_Key vivante_screen_index;

static Bool vivante_map_bo_to_gpu(struct vivante *vivante,
	struct drm_armada_bo *bo, void **info, uint32_t *handle)
{
	*handle = drm_armada_bo_phys(bo);
	return TRUE;
}

void vivante_free_pixmap(PixmapPtr pixmap)
{
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);
	struct vivante *vivante;

	if (vPix) {
		vivante = vivante_get_screen_priv(pixmap->drawable.pScreen);
		vivante_batch_wait_commit(vivante, vPix);
		if (vPix->owner == GPU)
			vivante_unmap_gpu(vivante, vPix);
		drm_armada_bo_put(vPix->bo);
		free(vPix);
	}
}

void vivante_set_pixmap_bo(PixmapPtr pixmap, struct drm_armada_bo *bo)
{
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);
	struct vivante *vivante = vivante_get_screen_priv(pixmap->drawable.pScreen);

	if (!vPix && !bo)
		return;

	if (vPix) {
		if (vPix->bo == bo)
			return;

		vivante_free_pixmap(pixmap);
		vPix = NULL;
	}

	if (bo) {
		gceSURF_FORMAT format;

		if (bo->pitch != pixmap->devKind) {
			xf86DrvMsg(vivante->scrnIndex, X_ERROR,
				   "%s: bo pitch %u and pixmap pitch %u mismatch\n",
				   __FUNCTION__, bo->pitch, pixmap->devKind);
			goto fail;
		}

		/*
		 * This is an imprecise conversion to the Vivante GAL format.
		 * One important thing here is that Pixmaps don't have an
		 * alpha channel.
		 */
		switch (pixmap->drawable.bitsPerPixel) {
		case 16:
			if (pixmap->drawable.depth == 15)
				format = gcvSURF_X1R5G5B5;
			else
				format = gcvSURF_R5G6B5;
			break;
		case 32:
			format = gcvSURF_X8R8G8B8;
			break;
		default:
			goto fail;
		}

		vPix = calloc(1, sizeof *vPix);
		if (!vPix)
			goto fail;

		vPix->bo = bo;
		vPix->width = pixmap->drawable.width;
		vPix->height = pixmap->drawable.height;
		vPix->pitch = bo->pitch;
		vPix->handle = -1;
		vPix->format = format;
		vPix->owner = NONE;

		/*
		 * If this is not a SHMEM bo, then we need to map it
		 * for the GPU.  As it will not be a fully cached mapping,
		 * we can keep this mapped.
		 */
		if (bo->type != DRM_ARMADA_BO_SHMEM) {
			if (!vivante_map_bo_to_gpu(vivante, bo, &vPix->info,
						   &vPix->handle)) {
				free(vPix);
				goto fail;
			}
		}

#ifdef DEBUG_PIXMAP
		dbg("Pixmap %p: vPix=%p bo=%p\n", pixmap, vPix, bo);
#endif
		drm_armada_bo_get(bo);
	}

 fail:
	vivante_set_pixmap_priv(pixmap, vPix);
}


/* Determine whether this GC can be accelerated */
static Bool vivante_GC_can_accel(GCPtr pGC, DrawablePtr pDrawable)
{
	unsigned long fullmask = FbFullMask(pDrawable->depth);

	/* Must be full-planes */
	return !pGC || (pGC->planemask & fullmask) == fullmask;
}

static Bool vivante_GCfill_can_accel(GCPtr pGC, DrawablePtr pDrawable)
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
vivante_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n, DDXPointPtr ppt,
	int *pwidth, int fSorted)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

	assert(vivante_GC_can_accel(pGC, pDrawable));

	if (vivante->force_fallback ||
	    !vivante_GCfill_can_accel(pGC, pDrawable) ||
	    !vivante_accel_FillSpans(pDrawable, pGC, n, ppt, pwidth, fSorted))
		vivante_unaccel_FillSpans(pDrawable, pGC, n, ppt, pwidth, fSorted);
}

static void
vivante_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth, int x, int y,
	int w, int h, int leftPad, int format, char *bits)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

	assert(vivante_GC_can_accel(pGC, pDrawable));

	if (vivante->force_fallback ||
	    !vivante_accel_PutImage(pDrawable, pGC, depth, x, y, w, h, leftPad,
				    format, bits))
		vivante_unaccel_PutImage(pDrawable, pGC, depth, x, y, w, h, leftPad,
					 format, bits);
}

static RegionPtr
vivante_CopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	int srcx, int srcy, int w, int h, int dstx, int dsty)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pScreen);

	assert(vivante_GC_can_accel(pGC, pDst));

	if (vivante->force_fallback)
		return vivante_unaccel_CopyArea(pSrc, pDst, pGC, srcx, srcy, w, h,
										dstx, dsty);

	return miDoCopy(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty,
			vivante_accel_CopyNtoN, 0, NULL);
}

static void
vivante_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt,
	DDXPointPtr ppt)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

	assert(vivante_GC_can_accel(pGC, pDrawable));

	if (vivante->force_fallback ||
	    !vivante_GCfill_can_accel(pGC, pDrawable) ||
	    !vivante_accel_PolyPoint(pDrawable, pGC, mode, npt, ppt))
		vivante_unaccel_PolyPoint(pDrawable, pGC, mode, npt, ppt);
}

static void
vivante_PolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrect,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

	if (vivante->force_fallback)
		goto fallback;

	assert(vivante_GC_can_accel(pGC, pDrawable));

	if (vivante_GCfill_can_accel(pGC, pDrawable)) {
		if (vivante_accel_PolyFillRectSolid(pDrawable, pGC, nrect, prect))
			return;
	} else if (pGC->fillStyle == FillTiled) {
		if (vivante_accel_PolyFillRectTiled(pDrawable, pGC, nrect, prect))
			return;
	}

 fallback:
	vivante_unaccel_PolyFillRect(pDrawable, pGC, nrect, prect);
}

static GCOps vivante_GCOps = {
	vivante_FillSpans,
	vivante_unaccel_SetSpans,
	vivante_PutImage,
	vivante_CopyArea,
	vivante_unaccel_CopyPlane,
	vivante_PolyPoint,
	vivante_unaccel_PolyLines,
	vivante_unaccel_PolySegment,
	miPolyRectangle,
	miPolyArc,
	miFillPolygon,
	vivante_PolyFillRect,
	miPolyFillArc,
	miPolyText8,
	miPolyText16,
	miImageText8,
	miImageText16,
	vivante_unaccel_ImageGlyphBlt,
	vivante_unaccel_PolyGlyphBlt,
	vivante_unaccel_PushPixels
};

static GCOps vivante_unaccel_GCOps = {
	vivante_unaccel_FillSpans,
	vivante_unaccel_SetSpans,
	vivante_unaccel_PutImage,
	vivante_unaccel_CopyArea,
	vivante_unaccel_CopyPlane,
	vivante_unaccel_PolyPoint,
	vivante_unaccel_PolyLines,
	vivante_unaccel_PolySegment,
	miPolyRectangle,
	miPolyArc,
	miFillPolygon,
	vivante_unaccel_PolyFillRect,
	miPolyFillArc,
	miPolyText8,
	miPolyText16,
	miImageText8,
	miImageText16,
	vivante_unaccel_ImageGlyphBlt,
	vivante_unaccel_PolyGlyphBlt,
	vivante_unaccel_PushPixels
};

static void
vivante_ValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr pDrawable)
{
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
				vivante_prepare_drawable(&pOldTile->drawable, ACCESS_RO);
				pNewTile = fb24_32ReformatTile(pOldTile, pDrawable->bitsPerPixel);
				vivante_finish_drawable(&pOldTile->drawable, ACCESS_RO);
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
			vivante_prepare_drawable(&pGC->tile.pixmap->drawable, ACCESS_RW);
			fbPadPixmap(pGC->tile.pixmap);
			vivante_finish_drawable(&pGC->tile.pixmap->drawable, ACCESS_RW);
		}
		/* mask out gctile changes now that we've done the work */
		changes &= ~GCTile;
	}
	if (changes & GCStipple && pGC->stipple) {
		vivante_prepare_drawable(&pGC->stipple->drawable, ACCESS_RW);
		fbValidateGC(pGC, changes, pDrawable);
		vivante_finish_drawable(&pGC->stipple->drawable, ACCESS_RW);
	} else {
		fbValidateGC(pGC, changes, pDrawable);
	}
	/*
	 * Select the GC ops depending on whether we have any
	 * chance to accelerate with this GC.
	 */
	pGC->ops = vivante_GC_can_accel(pGC, pDrawable)
				 ? &vivante_GCOps : &vivante_unaccel_GCOps;
}

static GCFuncs vivante_GCFuncs = {
	vivante_ValidateGC,
	miChangeGC,
	miCopyGC,
	miDestroyGC,
	miChangeClip,
	miDestroyClip,
	miCopyClip
};


static Bool vivante_CloseScreen(int scrnIndex, ScreenPtr pScreen)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif

#ifdef HAVE_DRI2
	vivante_dri2_CloseScreen(scrnIndex, pScreen);
#endif

#ifdef RENDER
	/* Restore the Pointers */
	ps->Composite = vivante->Composite;
	ps->Glyphs = vivante->Glyphs;
	ps->UnrealizeGlyph = vivante->UnrealizeGlyph;
	ps->Triangles = vivante->Triangles;
	ps->Trapezoids = vivante->Trapezoids;
	ps->AddTriangles = vivante->AddTriangles;
	ps->AddTraps = vivante->AddTraps;
#endif

	pScreen->CloseScreen = vivante->CloseScreen;
	pScreen->GetImage = vivante->GetImage;
	pScreen->GetSpans = vivante->GetSpans;
	pScreen->ChangeWindowAttributes = vivante->ChangeWindowAttributes;
	pScreen->CopyWindow = vivante->CopyWindow;
	pScreen->CreatePixmap = vivante->CreatePixmap;
	pScreen->DestroyPixmap = vivante->DestroyPixmap;
	pScreen->CreateGC = vivante->CreateGC;
	pScreen->BitmapToRegion = vivante->BitmapToRegion;
	pScreen->BlockHandler = vivante->BlockHandler;

	vivante_accel_shutdown(vivante);

	drm_armada_bo_put(vivante->batch_bo);

	free(vivante);

	return pScreen->CloseScreen(scrnIndex, pScreen);
}

static void
vivante_CopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
	PixmapPtr pPixmap = pWin->drawable.pScreen->GetWindowPixmap(pWin);
	RegionRec rgnDst;
	int dx, dy;

	dx = ptOldOrg.x - pWin->drawable.x;
	dy = ptOldOrg.y - pWin->drawable.y;
	RegionTranslate(prgnSrc, -dx, -dy);
	RegionInit(&rgnDst, NullBox, 0);
	RegionIntersect(&rgnDst, &pWin->borderClip, prgnSrc);

	miCopyRegion(&pPixmap->drawable, &pPixmap->drawable, NULL,
		     &rgnDst, dx, dy, vivante_accel_CopyNtoN, 0, NULL);

	RegionUninit(&rgnDst);
}

static PixmapPtr
vivante_CreatePixmap(ScreenPtr pScreen, int w, int h, int depth, unsigned usage)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct drm_armada_bo *bo;
	PixmapPtr pixmap;
	int ret, bpp;

	if (w > 32768 || h > 32768)
		return NullPixmap;

	if (depth == 1 || vivante->force_fallback)
		goto fallback;

	if (usage == CREATE_PIXMAP_USAGE_GLYPH_PICTURE && w <= 32 && h <= 32)
		goto fallback;

	pixmap = vivante->CreatePixmap(pScreen, 0, 0, depth, usage);
	if (pixmap == NullPixmap || w == 0 || h == 0)
		return pixmap;

	bpp = pixmap->drawable.bitsPerPixel;
	if (bpp != 16 && bpp != 32)
		goto fallback_free_pix;

	bo = drm_armada_bo_create(vivante->bufmgr, w, h, bpp);
	if (!bo)
		goto fallback_free_pix;

	ret = drm_armada_bo_map(bo);
	if (ret)
		goto fallback_free_bo;

	/*
	 * Do not store our data pointer in the pixmap - only do so (via
	 * vivante_prepare_drawable()) when required to directly access the
	 * pixmap.  This provides us a way to validate that we do not have
	 * any spurious unchecked accesses to the pixmap data while the GPU
	 * has ownership of the pixmap.
	 */
	pScreen->ModifyPixmapHeader(pixmap, w, h, 0, 0, bo->pitch, NULL);

	vivante_set_pixmap_bo(pixmap, bo);
	if (!vivante_get_pixmap_priv(pixmap))
		goto fallback_free_bo;

	drm_armada_bo_put(bo);
	goto out;

 fallback_free_bo:
	drm_armada_bo_put(bo);
 fallback_free_pix:
	vivante->DestroyPixmap(pixmap);
 fallback:
	pixmap = vivante->CreatePixmap(pScreen, w, h, depth, usage);

 out:
#ifdef DEBUG_PIXMAP
	dbg("Created pixmap %p %dx%d %d %d %x\n",
	    pixmap, w, h, depth, pixmap->drawable.bitsPerPixel, usage);
#endif

	return pixmap;
}

static Bool vivante_DestroyPixmap(PixmapPtr pixmap)
{
	struct vivante *vivante = vivante_get_screen_priv(pixmap->drawable.pScreen);
	if (pixmap->refcnt == 1) {
#ifdef DEBUG_PIXMAP
		dbg("Destroying pixmap %p\n", pixmap);
#endif
		vivante_free_pixmap(pixmap);
		vivante_set_pixmap_priv(pixmap, NULL);
	}
	return vivante->DestroyPixmap(pixmap);
}

static Bool vivante_CreateGC(GCPtr pGC)
{
	struct vivante *vivante = vivante_get_screen_priv(pGC->pScreen);
	Bool ret;

	ret = vivante->CreateGC(pGC);
	if (ret)
		pGC->funcs = &vivante_GCFuncs;

	return ret;
}

/* Commit any pending GPU operations */
static void
vivante_BlockHandler(int scrn, pointer data, pointer timeout, pointer readmask)
{
	ScreenPtr pScreen = screenInfo.screens[scrn];
	struct vivante *vivante = vivante_get_screen_priv(pScreen);

	if (vivante->need_commit)
		vivante_commit(vivante, FALSE);

	pScreen->BlockHandler = vivante->BlockHandler;
	pScreen->BlockHandler(scrn, data, timeout, readmask);
	vivante->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = vivante_BlockHandler;
}

#ifdef RENDER
static void
vivante_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst,
	CARD16 width, CARD16 height)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pDrawable->pScreen);

	if (vivante->force_fallback ||
	    !vivante_accel_Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
				     xMask, yMask, xDst, yDst, width, height))
		vivante_unaccel_Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
					  xMask, yMask, xDst, yDst, width, height);
}
#endif

Bool vivante_ScreenInit(ScreenPtr pScreen, struct drm_armada_bufmgr *mgr)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	int drm_fd = GET_DRM_INFO(pScrn)->fd;
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif
	struct vivante *vivante;

	if (!vivante_CreateKey(&vivante_pixmap_index, PRIVATE_PIXMAP) ||
	    !vivante_CreateKey(&vivante_screen_index, PRIVATE_SCREEN))
		return FALSE;

	vivante = calloc(1, sizeof *vivante);
	if (!vivante)
		return FALSE;

	vivante->scrnIndex = pScrn->scrnIndex;
	list_init(&vivante->batch_list);
	vivante->bufmgr = mgr;
	vivante->batch_bo = drm_armada_bo_dumb_create(mgr, 64, 64, 32);
	if (!vivante->batch_bo) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to create batch bo: %s\n",
			   strerror(errno));
		goto fail;
	}

	if (drm_armada_bo_map(vivante->batch_bo)) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to map batch bo: %s\n",
			   strerror(errno));
		goto fail;
	}

	if (!vivante_accel_init(vivante))
		goto fail;

	if (!vivante_map_bo_to_gpu(vivante, vivante->batch_bo,
				   &vivante->batch_info,
				   &vivante->batch_handle))
		goto fail;

	vivante->batch_ptr = vivante->batch_bo->ptr;
	vivante->batch_idx_max = vivante->batch_bo->size / sizeof(uint32_t);

//	xf86DrvMsg(vivante->scrnIndex, X_INFO,
//		   "vivante: created batch at v%p p0x%08x max idx %u\n",
//		   vivante->batch_ptr, vivante->batch_handle,
//		   vivante->batch_idx_max);

	vivante_set_screen_priv(pScreen, vivante);

#ifdef HAVE_DRI2
	if (!vivante_dri2_ScreenInit(pScreen, drm_fd))
		goto fail;
#endif

	vivante->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = vivante_CloseScreen;
	vivante->GetImage = pScreen->GetImage;
	pScreen->GetImage = vivante_unaccel_GetImage;
	vivante->GetSpans = pScreen->GetSpans;
	pScreen->GetSpans = vivante_unaccel_GetSpans;
	vivante->ChangeWindowAttributes = pScreen->ChangeWindowAttributes;
	pScreen->ChangeWindowAttributes = vivante_unaccel_ChangeWindowAttributes;
	vivante->CopyWindow = pScreen->CopyWindow;
	pScreen->CopyWindow = vivante_CopyWindow;
	vivante->CreatePixmap = pScreen->CreatePixmap;
	pScreen->CreatePixmap = vivante_CreatePixmap;
	vivante->DestroyPixmap = pScreen->DestroyPixmap;
	pScreen->DestroyPixmap = vivante_DestroyPixmap;
	vivante->CreateGC = pScreen->CreateGC;
	pScreen->CreateGC = vivante_CreateGC;
	vivante->BitmapToRegion = pScreen->BitmapToRegion;
	pScreen->BitmapToRegion = vivante_unaccel_BitmapToRegion;
	vivante->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = vivante_BlockHandler;

#ifdef RENDER
	vivante->Composite = ps->Composite;
	ps->Composite = vivante_Composite;
	vivante->Glyphs = ps->Glyphs;
	ps->Glyphs = vivante_unaccel_Glyphs;
	vivante->UnrealizeGlyph = ps->UnrealizeGlyph;
	vivante->Triangles = ps->Triangles;
	ps->Triangles = vivante_unaccel_Triangles;
	vivante->Trapezoids = ps->Trapezoids;
	ps->Trapezoids = vivante_unaccel_Trapezoids;
	vivante->AddTriangles = ps->AddTriangles;
	ps->AddTriangles = vivante_unaccel_AddTriangles;
	vivante->AddTraps = ps->AddTraps;
	ps->AddTraps = vivante_unaccel_AddTraps;
#endif

	return TRUE;

fail:
	vivante_accel_shutdown(vivante);
	if (vivante->batch_bo)
		drm_armada_bo_put(vivante->batch_bo);
	free(vivante);
	return FALSE;
}
