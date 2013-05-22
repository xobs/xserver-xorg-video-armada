/*
 * Copyright (C) 2012-2013 Russell King.
 *
 * Based in part on code from the Intel Xorg driver.
 *
 * Unaccelerated drawing functions for Vivante GPU.  These prepare
 * access to the drawable prior to passing on the call to the Xorg
 * Server's fb layer (or pixman layer.)
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fb.h"
#include "fbpict.h"

#include "vivante_unaccel.h"
#include "vivante_utils.h"

static void vivante_prepare_gc(GCPtr pGC)
{
	if (pGC->stipple)
		vivante_prepare_drawable(&pGC->stipple->drawable, ACCESS_RO);
	if (pGC->fillStyle == FillTiled)
		vivante_prepare_drawable(&pGC->tile.pixmap->drawable, ACCESS_RO);
}

static void vivante_finish_gc(GCPtr pGC)
{
	if (pGC->fillStyle == FillTiled)
		vivante_finish_drawable(&pGC->tile.pixmap->drawable, ACCESS_RO);
	if (pGC->stipple)
		vivante_finish_drawable(&pGC->stipple->drawable, ACCESS_RO);
}

static void vivante_prepare_window(WindowPtr pWin)
{
	if (pWin->backgroundState == BackgroundPixmap)
		vivante_prepare_drawable(&pWin->background.pixmap->drawable, ACCESS_RO);
	if (!pWin->borderIsPixel)
		vivante_prepare_drawable(&pWin->border.pixmap->drawable, ACCESS_RO);
}

static void vivante_finish_window(WindowPtr pWin)
{
	if (!pWin->borderIsPixel)
		vivante_finish_drawable(&pWin->border.pixmap->drawable, ACCESS_RO);
	if (pWin->backgroundState == BackgroundPixmap)
		vivante_finish_drawable(&pWin->background.pixmap->drawable, ACCESS_RO);
}

void vivante_unaccel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int nspans,
	DDXPointPtr ppt, int *pwidth, int fSorted)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbFillSpans(pDrawable, pGC, nspans, ppt, pwidth, fSorted);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_SetSpans(DrawablePtr pDrawable, GCPtr pGC, char *psrc,
	DDXPointPtr ppt, int *pwidth, int nspans, int fSorted)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbSetSpans(pDrawable, pGC, psrc, ppt, pwidth, nspans, fSorted);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbPutImage(pDrawable, pGC, depth, x, y, w, h, leftPad, format, bits);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

RegionPtr vivante_unaccel_CopyArea(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, int srcx, int srcy, int w, int h, int dstx, int dsty)
{
	RegionPtr ret;

	vivante_prepare_drawable(pDst, ACCESS_RW);
	vivante_prepare_drawable(pSrc, ACCESS_RO);
	ret = fbCopyArea(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty);
	vivante_finish_drawable(pSrc, ACCESS_RO);
	vivante_finish_drawable(pDst, ACCESS_RW);

	return ret;
}

RegionPtr vivante_unaccel_CopyPlane(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, int srcx, int srcy, int w, int h, int dstx, int dsty,
	unsigned long bitPlane)
{
	RegionPtr ret;

	vivante_prepare_drawable(pDst, ACCESS_RW);
	vivante_prepare_drawable(pSrc, ACCESS_RO);
	ret = fbCopyPlane(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty, bitPlane);
	vivante_finish_drawable(pSrc, ACCESS_RO);
	vivante_finish_drawable(pDst, ACCESS_RW);

	return ret;
}

void vivante_unaccel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr pptInit)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	fbPolyPoint(pDrawable, pGC, mode, npt, pptInit);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_PolyLines(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	if (pGC->lineWidth == 0) {
		vivante_prepare_drawable(pDrawable, ACCESS_RW);
		vivante_prepare_gc(pGC);
		fbPolyLine(pDrawable, pGC, mode, npt, ppt);
		vivante_finish_gc(pGC);
		vivante_finish_drawable(pDrawable, ACCESS_RW);
	} else {
		fbPolyLine(pDrawable, pGC, mode, npt, ppt);
	}
}

void vivante_unaccel_PolySegment(DrawablePtr pDrawable, GCPtr pGC,
	int nsegInit, xSegment * pSegInit)
{
	if (pGC->lineWidth == 0) {
		vivante_prepare_drawable(pDrawable, ACCESS_RW);
		vivante_prepare_gc(pGC);
		fbPolySegment(pDrawable, pGC, nsegInit, pSegInit);
		vivante_finish_gc(pGC);
		vivante_finish_drawable(pDrawable, ACCESS_RW);
	} else {
		fbPolySegment(pDrawable, pGC, nsegInit, pSegInit);
	}
}

void vivante_unaccel_PolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrect,
	xRectangle * prect)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbPolyFillRect(pDrawable, pGC, nrect, prect);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_ImageGlyphBlt(DrawablePtr pDrawable, GCPtr pGC,
	int x, int y, unsigned int nglyph, CharInfoPtr * ppci, pointer pglyphBase)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbImageGlyphBlt(pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_PolyGlyphBlt(DrawablePtr pDrawable, GCPtr pGC,
	int x, int y, unsigned int nglyph, CharInfoPtr * ppci, pointer pglyphBase)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_gc(pGC);
	fbPolyGlyphBlt(pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

void vivante_unaccel_PushPixels(GCPtr pGC, PixmapPtr pBitmap,
	DrawablePtr pDrawable, int w, int h, int x, int y)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RW);
	vivante_prepare_drawable(&pBitmap->drawable, ACCESS_RO);
	vivante_prepare_gc(pGC);
	fbPushPixels(pGC, pBitmap, pDrawable, w, h, x, y);
	vivante_finish_gc(pGC);
	vivante_finish_drawable(&pBitmap->drawable, ACCESS_RO);
	vivante_finish_drawable(pDrawable, ACCESS_RW);
}

/* Non-GC ops */

void vivante_unaccel_GetSpans(DrawablePtr pDrawable, int wMax, DDXPointPtr ppt,
	int *pwidth, int nspans, char *pdstStart)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RO);
	fbGetSpans(pDrawable, wMax, ppt, pwidth, nspans, pdstStart);
	vivante_finish_drawable(pDrawable, ACCESS_RO);
}

void vivante_unaccel_GetImage(DrawablePtr pDrawable, int x, int y,
	int w, int h, unsigned int format, unsigned long planeMask, char *d)
{
	vivante_prepare_drawable(pDrawable, ACCESS_RO);
	fbGetImage(pDrawable, x, y, w, h, format, planeMask, d);
	vivante_finish_drawable(pDrawable, ACCESS_RO);
}

Bool vivante_unaccel_ChangeWindowAttributes(WindowPtr pWin, unsigned long mask)
{
	Bool ret;
	vivante_prepare_window(pWin);
	ret = fbChangeWindowAttributes(pWin, mask);
	vivante_finish_window(pWin);
	return ret;
}

RegionPtr vivante_unaccel_BitmapToRegion(PixmapPtr pixmap)
{
	RegionPtr ret;
	vivante_prepare_drawable(&pixmap->drawable, ACCESS_RO);
	ret = fbPixmapToRegion(pixmap);
	vivante_finish_drawable(&pixmap->drawable, ACCESS_RO);
	return ret;
}

void vivante_unaccel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	BoxPtr pBox, int nBox, int dx, int dy, Bool reverse, Bool upsidedown,
	Pixel bitPlane, void *closure)
{
	vivante_prepare_drawable(pDst, ACCESS_RW);
	if (pDst != pSrc)
		vivante_prepare_drawable(pSrc, ACCESS_RO);
	fbCopyNtoN(pSrc, pDst, pGC, pBox, nBox, dx, dy, reverse, upsidedown,
			   bitPlane, closure);
	if (pDst != pSrc)
		vivante_finish_drawable(pSrc, ACCESS_RO);
	vivante_finish_drawable(pDst, ACCESS_RW);
}
