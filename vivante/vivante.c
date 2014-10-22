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

#include "armada_accel.h"

#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "compat-api.h"

#include <gc_hal.h>

#include "cpu_access.h"
#include "fbutil.h"
#include "gal_extension.h"
#include "mark.h"
#include "pixmaputil.h"
#include "unaccel.h"

#include "vivante.h"
#include "vivante_accel.h"
#include "vivante_dri2.h"
#include "vivante_utils.h"

vivante_Key vivante_pixmap_index;
vivante_Key vivante_screen_index;

void vivante_free_pixmap(PixmapPtr pixmap)
{
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);

	if (vPix) {
		struct vivante *vivante;

		vivante = vivante_get_screen_priv(pixmap->drawable.pScreen);
		vivante_batch_wait_commit(vivante, vPix);
		if (!vPix->bo) {
			vivante_unmap_from_gpu(vivante, vPix->info,
					       vPix->handle);
		} else {
			if (vPix->owner == GPU)
				vivante_unmap_gpu(vivante, vPix);
			drm_armada_bo_put(vPix->bo);
		}
		free(vPix);
	}
}

static struct vivante_pixmap *vivante_alloc_pixmap(PixmapPtr pixmap,
	gceSURF_FORMAT fmt)
{
	struct vivante_pixmap *vpix;

	vpix = calloc(1, sizeof *vpix);
	if (vpix) {
		vpix->width = pixmap->drawable.width;
		vpix->height = pixmap->drawable.height;
		vpix->pitch = pixmap->devKind;
		vpix->format = fmt;
		vpix->handle = -1;
	}
	return vpix;
}

/* Determine whether this GC can be accelerated */
static Bool vivante_GC_can_accel(GCPtr pGC, DrawablePtr pDrawable)
{
	/* Must be full-planes */
	return !pGC || fb_full_planemask(pDrawable, pGC->planemask);
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
		unaccel_FillSpans(pDrawable, pGC, n, ppt, pwidth, fSorted);
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
		unaccel_PutImage(pDrawable, pGC, depth, x, y, w, h, leftPad,
					 format, bits);
}

static RegionPtr
vivante_CopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	int srcx, int srcy, int w, int h, int dstx, int dsty)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pScreen);

	assert(vivante_GC_can_accel(pGC, pDst));

	if (vivante->force_fallback)
		return unaccel_CopyArea(pSrc, pDst, pGC, srcx, srcy, w, h,
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
		unaccel_PolyPoint(pDrawable, pGC, mode, npt, ppt);
}

static void
vivante_PolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrect,
	xRectangle * prect)
{
	struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);
	PixmapPtr pPix = drawable_pixmap(pDrawable);

	if (vivante->force_fallback ||
	    (pPix->drawable.width == 1 && pPix->drawable.height == 1))
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
	unaccel_PolyFillRect(pDrawable, pGC, nrect, prect);
}

static GCOps vivante_GCOps = {
	vivante_FillSpans,
	unaccel_SetSpans,
	vivante_PutImage,
	vivante_CopyArea,
	unaccel_CopyPlane,
	vivante_PolyPoint,
	unaccel_PolyLines,
	unaccel_PolySegment,
	miPolyRectangle,
	miPolyArc,
	miFillPolygon,
	vivante_PolyFillRect,
	miPolyFillArc,
	miPolyText8,
	miPolyText16,
	miImageText8,
	miImageText16,
	unaccel_ImageGlyphBlt,
	unaccel_PolyGlyphBlt,
	unaccel_PushPixels
};

static GCOps vivante_unaccel_GCOps = {
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


static Bool vivante_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif
	PixmapPtr pixmap;

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

#ifdef HAVE_DRI2
	vivante_dri2_CloseScreen(CLOSE_SCREEN_ARGS);
#endif

	pixmap = pScreen->GetScreenPixmap(pScreen);
	vivante_free_pixmap(pixmap);
	vivante_set_pixmap_priv(pixmap, NULL);

#ifdef VIVANTE_BATCH
	vivante_unmap_from_gpu(vivante, vivante->batch_info,
			       vivante->batch_handle);
#endif

	vivante_accel_shutdown(vivante);

#ifdef VIVANTE_BATCH
	drm_armada_bo_put(vivante->batch_bo);
#endif

	free(vivante);

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
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

#ifdef COMPOSITE
	if (pPixmap->screen_x || pPixmap->screen_y)
		RegionTranslate(&rgnDst, -pPixmap->screen_x,
				-pPixmap->screen_y);
#endif

	miCopyRegion(&pPixmap->drawable, &pPixmap->drawable, NULL,
		     &rgnDst, dx, dy, vivante_accel_CopyNtoN, 0, NULL);

	RegionUninit(&rgnDst);
}

static PixmapPtr
vivante_CreatePixmap(ScreenPtr pScreen, int w, int h, int depth, unsigned usage)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_pixmap *vpix;
	struct drm_armada_bo *bo;
	gceSURF_FORMAT fmt;
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

	/* Create the appropriate format for this pixmap */
	switch (pixmap->drawable.bitsPerPixel) {
	case 16:
		if (pixmap->drawable.depth == 15)
			fmt = gcvSURF_A1R5G5B5;
		else
			fmt = gcvSURF_R5G6B5;
		break;
	case 32:
		fmt = gcvSURF_A8R8G8B8;
		break;
	default:
		goto fallback_free_pix;
	}

	bpp = pixmap->drawable.bitsPerPixel;

	bo = drm_armada_bo_create(vivante->bufmgr, w, h, bpp);
	if (!bo)
		goto fallback_free_pix;

	ret = drm_armada_bo_map(bo);
	if (ret)
		goto fallback_free_bo;

	/*
	 * Do not store our data pointer in the pixmap - only do so (via
	 * prepare_cpu_drawable()) when required to directly access the
	 * pixmap.  This provides us a way to validate that we do not have
	 * any spurious unchecked accesses to the pixmap data while the GPU
	 * has ownership of the pixmap.
	 */
	pScreen->ModifyPixmapHeader(pixmap, w, h, 0, 0, bo->pitch, NULL);

	vpix = vivante_alloc_pixmap(pixmap, fmt);
	if (!vpix)
		goto fallback_free_bo;

	vpix->bo = bo;
	
#ifdef DEBUG_PIXMAP
	dbg("Pixmap %p: vPix=%p bo=%p\n", pixmap, vpix, bo);
#endif

	vivante_set_pixmap_priv(pixmap, vpix);
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
vivante_BlockHandler(BLOCKHANDLER_ARGS_DECL)
{
	SCREEN_PTR(arg);
	struct vivante *vivante = vivante_get_screen_priv(pScreen);

	if (vivante->need_commit)
		vivante_commit(vivante, FALSE);

	mark_flush();

	pScreen->BlockHandler = vivante->BlockHandler;
	pScreen->BlockHandler(BLOCKHANDLER_ARGS);
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
	Bool ret;

	if (!vivante->force_fallback) {
		unsigned src_repeat, mask_repeat;

		src_repeat = pSrc->repeat;
		if (pMask)
			mask_repeat = pMask->repeat;

		ret = vivante_accel_Composite(op, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height);
		pSrc->repeat = src_repeat;
		if (pMask)
			pMask->repeat = mask_repeat;

		if (ret)
			return;
	}
	unaccel_Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
			  xMask, yMask, xDst, yDst, width, height);
}
#endif

Bool vivante_ScreenInit(ScreenPtr pScreen, struct drm_armada_bufmgr *mgr)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
#ifdef RENDER
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);
#endif
	struct vivante *vivante;

	/* We must have a buffer manager */
	if (!mgr)
		return FALSE;

	if (!vivante_CreateKey(&vivante_pixmap_index, PRIVATE_PIXMAP) ||
	    !vivante_CreateKey(&vivante_screen_index, PRIVATE_SCREEN))
		return FALSE;

	vivante = calloc(1, sizeof *vivante);
	if (!vivante)
		return FALSE;

	vivante->drm_fd = GET_DRM_INFO(pScrn)->fd;
	vivante->scrnIndex = pScrn->scrnIndex;
	vivante->bufmgr = mgr;

#ifdef VIVANTE_BATCH
	xorg_list_init(&vivante->batch_list);
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
#endif

	if (!vivante_accel_init(vivante))
		goto fail;

#ifdef VIVANTE_BATCH
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
#endif

	vivante_set_screen_priv(pScreen, vivante);

#ifdef HAVE_DRI2
	if (!vivante_dri2_ScreenInit(pScreen))
		goto fail;
#endif

	vivante->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = vivante_CloseScreen;
	vivante->GetImage = pScreen->GetImage;
	pScreen->GetImage = unaccel_GetImage;
	vivante->GetSpans = pScreen->GetSpans;
	pScreen->GetSpans = unaccel_GetSpans;
	vivante->ChangeWindowAttributes = pScreen->ChangeWindowAttributes;
	pScreen->ChangeWindowAttributes = unaccel_ChangeWindowAttributes;
	vivante->CopyWindow = pScreen->CopyWindow;
	pScreen->CopyWindow = vivante_CopyWindow;
	vivante->CreatePixmap = pScreen->CreatePixmap;
	pScreen->CreatePixmap = vivante_CreatePixmap;
	vivante->DestroyPixmap = pScreen->DestroyPixmap;
	pScreen->DestroyPixmap = vivante_DestroyPixmap;
	vivante->CreateGC = pScreen->CreateGC;
	pScreen->CreateGC = vivante_CreateGC;
	vivante->BitmapToRegion = pScreen->BitmapToRegion;
	pScreen->BitmapToRegion = unaccel_BitmapToRegion;
	vivante->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = vivante_BlockHandler;

#ifdef RENDER
	vivante->Composite = ps->Composite;
	ps->Composite = vivante_Composite;
	vivante->Glyphs = ps->Glyphs;
	ps->Glyphs = unaccel_Glyphs;
	vivante->UnrealizeGlyph = ps->UnrealizeGlyph;
	vivante->Triangles = ps->Triangles;
	ps->Triangles = unaccel_Triangles;
	vivante->Trapezoids = ps->Trapezoids;
	ps->Trapezoids = unaccel_Trapezoids;
	vivante->AddTriangles = ps->AddTriangles;
	ps->AddTriangles = unaccel_AddTriangles;
	vivante->AddTraps = ps->AddTraps;
	ps->AddTraps = unaccel_AddTraps;
#endif

	return TRUE;

fail:
#ifdef VIVANTE_BATCH
	if (vivante->batch_info)
		vivante_unmap_from_gpu(vivante, vivante->batch_info,
				       vivante->batch_handle);
#endif
	vivante_accel_shutdown(vivante);
#ifdef VIVANTE_BATCH
	if (vivante->batch_bo)
		drm_armada_bo_put(vivante->batch_bo);
#endif
	free(vivante);
	return FALSE;
}

static Bool vivante_import_dmabuf(ScreenPtr pScreen, PixmapPtr pPixmap, int fd)
{
	struct vivante *vivante = vivante_get_screen_priv(pPixmap->drawable.pScreen);
	struct vivante_pixmap *vpix = vivante_get_pixmap_priv(pPixmap);
	gceSURF_FORMAT format;

	if (vpix)
		vivante_free_pixmap(pPixmap);
	vpix = NULL;

	/*
	 * This is an imprecise conversion to the Vivante GAL format.
	 * Although pixmaps in X generally don't have an alpha channel,
	 * we must set the format to include the alpha channel to
	 * ensure that the GPU copies all the bits.
	 */
	switch (pPixmap->drawable.bitsPerPixel) {
	case 16:
		if (pPixmap->drawable.depth == 15)
			format = gcvSURF_A1R5G5B5;
		else
			format = gcvSURF_R5G6B5;
		break;
	case 32:
		format = gcvSURF_A8R8G8B8;
		break;
	default:
		goto fail;
	}

	vpix = vivante_alloc_pixmap(pPixmap, format);
	if (!vpix)
		return FALSE;

	if (!vivante_map_dmabuf(vivante, fd, vpix)) {
		free(vpix);
		vpix = NULL;
	}

 fail:
	vivante_set_pixmap_priv(pPixmap, vpix);

	return TRUE;
}

static const struct armada_accel_ops accel_ops = {
	.screen_init	= vivante_ScreenInit,
	.import_dmabuf	= vivante_import_dmabuf,
	.free_pixmap	= vivante_free_pixmap,
};

_X_EXPORT Bool accel_module_init(const struct armada_accel_ops **ops)
{
	*ops = &accel_ops;

	return TRUE;
}

static XF86ModuleVersionInfo vivante_version = {
	.modname = "Vivante GPU driver",
	.vendor = MODULEVENDORSTRING,
	._modinfo1_ = MODINFOSTRING1,
	._modinfo2_ = MODINFOSTRING2,
	.xf86version = XORG_VERSION_CURRENT,
	.majorversion = PACKAGE_VERSION_MAJOR,
	.minorversion = PACKAGE_VERSION_MINOR,
	.patchlevel = PACKAGE_VERSION_PATCHLEVEL,
	.abiclass = ABI_CLASS_ANSIC,
	.abiversion = ABI_ANSIC_VERSION,
	.moduleclass = MOD_CLASS_NONE,
	.checksum = { 0, 0, 0, 0 },
};

_X_EXPORT XF86ModuleData vivante_gpuModuleData = {
	.vers = &vivante_version,
};
