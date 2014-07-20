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

#include "cpu_access.h"
#include "unaccel.h"

static void prepare_cpu_gc(GCPtr pGC)
{
	if (pGC->stipple)
		prepare_cpu_drawable(&pGC->stipple->drawable, CPU_ACCESS_RO);
	if (pGC->fillStyle == FillTiled)
		prepare_cpu_drawable(&pGC->tile.pixmap->drawable, CPU_ACCESS_RO);
}

static void finish_cpu_gc(GCPtr pGC)
{
	if (pGC->fillStyle == FillTiled)
		finish_cpu_drawable(&pGC->tile.pixmap->drawable, CPU_ACCESS_RO);
	if (pGC->stipple)
		finish_cpu_drawable(&pGC->stipple->drawable, CPU_ACCESS_RO);
}

void unaccel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int nspans,
	DDXPointPtr ppt, int *pwidth, int fSorted)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbFillSpans(pDrawable, pGC, nspans, ppt, pwidth, fSorted);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_SetSpans(DrawablePtr pDrawable, GCPtr pGC, char *psrc,
	DDXPointPtr ppt, int *pwidth, int nspans, int fSorted)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbSetSpans(pDrawable, pGC, psrc, ppt, pwidth, nspans, fSorted);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbPutImage(pDrawable, pGC, depth, x, y, w, h, leftPad, format, bits);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

RegionPtr unaccel_CopyArea(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, int srcx, int srcy, int w, int h, int dstx, int dsty)
{
	RegionPtr ret;

	prepare_cpu_drawable(pDst, CPU_ACCESS_RW);
	prepare_cpu_drawable(pSrc, CPU_ACCESS_RO);
	ret = fbCopyArea(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty);
	finish_cpu_drawable(pSrc, CPU_ACCESS_RO);
	finish_cpu_drawable(pDst, CPU_ACCESS_RW);

	return ret;
}

RegionPtr unaccel_CopyPlane(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, int srcx, int srcy, int w, int h, int dstx, int dsty,
	unsigned long bitPlane)
{
	RegionPtr ret;

	prepare_cpu_drawable(pDst, CPU_ACCESS_RW);
	prepare_cpu_drawable(pSrc, CPU_ACCESS_RO);
	ret = fbCopyPlane(pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty, bitPlane);
	finish_cpu_drawable(pSrc, CPU_ACCESS_RO);
	finish_cpu_drawable(pDst, CPU_ACCESS_RW);

	return ret;
}

void unaccel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr pptInit)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	fbPolyPoint(pDrawable, pGC, mode, npt, pptInit);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_PolyLines(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt)
{
	if (pGC->lineWidth == 0) {
		prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
		prepare_cpu_gc(pGC);
		fbPolyLine(pDrawable, pGC, mode, npt, ppt);
		finish_cpu_gc(pGC);
		finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	} else {
		fbPolyLine(pDrawable, pGC, mode, npt, ppt);
	}
}

void unaccel_PolySegment(DrawablePtr pDrawable, GCPtr pGC,
	int nsegInit, xSegment * pSegInit)
{
	if (pGC->lineWidth == 0) {
		prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
		prepare_cpu_gc(pGC);
		fbPolySegment(pDrawable, pGC, nsegInit, pSegInit);
		finish_cpu_gc(pGC);
		finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	} else {
		fbPolySegment(pDrawable, pGC, nsegInit, pSegInit);
	}
}

void unaccel_PolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrect,
	xRectangle * prect)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbPolyFillRect(pDrawable, pGC, nrect, prect);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_ImageGlyphBlt(DrawablePtr pDrawable, GCPtr pGC,
	int x, int y, unsigned int nglyph, CharInfoPtr * ppci, pointer pglyphBase)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbImageGlyphBlt(pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_PolyGlyphBlt(DrawablePtr pDrawable, GCPtr pGC,
	int x, int y, unsigned int nglyph, CharInfoPtr * ppci, pointer pglyphBase)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_gc(pGC);
	fbPolyGlyphBlt(pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

void unaccel_PushPixels(GCPtr pGC, PixmapPtr pBitmap,
	DrawablePtr pDrawable, int w, int h, int x, int y)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RW);
	prepare_cpu_drawable(&pBitmap->drawable, CPU_ACCESS_RO);
	prepare_cpu_gc(pGC);
	fbPushPixels(pGC, pBitmap, pDrawable, w, h, x, y);
	finish_cpu_gc(pGC);
	finish_cpu_drawable(&pBitmap->drawable, CPU_ACCESS_RO);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RW);
}

/* Non-GC ops */

void unaccel_GetSpans(DrawablePtr pDrawable, int wMax, DDXPointPtr ppt,
	int *pwidth, int nspans, char *pdstStart)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RO);
	fbGetSpans(pDrawable, wMax, ppt, pwidth, nspans, pdstStart);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RO);
}

void unaccel_GetImage(DrawablePtr pDrawable, int x, int y,
	int w, int h, unsigned int format, unsigned long planeMask, char *d)
{
	prepare_cpu_drawable(pDrawable, CPU_ACCESS_RO);
	fbGetImage(pDrawable, x, y, w, h, format, planeMask, d);
	finish_cpu_drawable(pDrawable, CPU_ACCESS_RO);
}

static void unaccel_fixup_tile(DrawablePtr pDraw, PixmapPtr *ppPix)
{
	PixmapPtr pNew, pPixmap = *ppPix;

	if (pPixmap->drawable.bitsPerPixel != pDraw->bitsPerPixel) {
		prepare_cpu_drawable(&pPixmap->drawable, CPU_ACCESS_RO);
		pNew = fb24_32ReformatTile(pPixmap, pDraw->bitsPerPixel);
		finish_cpu_drawable(&pPixmap->drawable, CPU_ACCESS_RO);

		pDraw->pScreen->DestroyPixmap(pPixmap);
		*ppPix = pPixmap = pNew;
	}

	if (FbEvenTile(pPixmap->drawable.width * pPixmap->drawable.bitsPerPixel)) {
		prepare_cpu_drawable(&pPixmap->drawable, CPU_ACCESS_RW);
		fbPadPixmap(pPixmap);
		finish_cpu_drawable(&pPixmap->drawable, CPU_ACCESS_RW);
	}
}

Bool unaccel_ChangeWindowAttributes(WindowPtr pWin, unsigned long mask)
{
	if (mask & CWBackPixmap && pWin->backgroundState == BackgroundPixmap)
		unaccel_fixup_tile(&pWin->drawable,
					   &pWin->background.pixmap);

	if (mask & CWBorderPixmap && !pWin->borderIsPixel)
		unaccel_fixup_tile(&pWin->drawable,
					   &pWin->border.pixmap);

	return TRUE;
}

RegionPtr unaccel_BitmapToRegion(PixmapPtr pixmap)
{
	RegionPtr ret;
	prepare_cpu_drawable(&pixmap->drawable, CPU_ACCESS_RO);
	ret = fbPixmapToRegion(pixmap);
	finish_cpu_drawable(&pixmap->drawable, CPU_ACCESS_RO);
	return ret;
}

void unaccel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,
	BoxPtr pBox, int nBox, int dx, int dy, Bool reverse, Bool upsidedown,
	Pixel bitPlane, void *closure)
{
	prepare_cpu_drawable(pDst, CPU_ACCESS_RW);
	if (pDst != pSrc)
		prepare_cpu_drawable(pSrc, CPU_ACCESS_RO);
	fbCopyNtoN(pSrc, pDst, pGC, pBox, nBox, dx, dy, reverse, upsidedown,
			   bitPlane, closure);
	if (pDst != pSrc)
		finish_cpu_drawable(pSrc, CPU_ACCESS_RO);
	finish_cpu_drawable(pDst, CPU_ACCESS_RW);
}
