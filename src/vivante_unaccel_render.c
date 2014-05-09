/*
 * Copyright (C) 2012 Russell King.
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
#include "mipict.h"

#include "compat-api.h"
#include "vivante_unaccel.h"
#include "vivante_utils.h"

static void vivante_prepare_picture(PicturePtr pPicture, int access)
{
    if (pPicture->pDrawable) {
        vivante_prepare_drawable(pPicture->pDrawable, access);
        if (pPicture->alphaMap)
            vivante_prepare_drawable(pPicture->alphaMap->pDrawable, access);
    }
}

static void vivante_finish_picture(PicturePtr pPicture, int access)
{
    if (pPicture->pDrawable) {
        if (pPicture->alphaMap)
            vivante_finish_drawable(pPicture->alphaMap->pDrawable, access);
        vivante_finish_drawable(pPicture->pDrawable, access);
    }
}

static void GlyphExtents(int nlist, GlyphListPtr list, GlyphPtr *glyphs,
    BoxPtr extents)
{
    int x1, y1, x2, y2, n, x, y;
    GlyphPtr glyph;

    x = y = 0;
    extents->x1 = extents->y1 = MAXSHORT;
    extents->x2 = extents->y2 = MINSHORT;
    while (nlist--) {
        x += list->xOff;
        y += list->yOff;
        n = list->len;
        list++;
        while (n--) {
            glyph = *glyphs++;
            x1 = x - glyph->info.x;
            if (x1 < MINSHORT)
                x1 = MINSHORT;
            y1 = y - glyph->info.y;
            if (y1 < MINSHORT)
                y1 = MINSHORT;
            x2 = x1 + glyph->info.width;
            if (x2 > MAXSHORT)
                x2 = MAXSHORT;
            y2 = y1 + glyph->info.height;
            if (y2 > MAXSHORT)
                y2 = MAXSHORT;
            if (x1 < extents->x1)
                extents->x1 = x1;
            if (x2 > extents->x2)
                extents->x2 = x2;
            if (y1 < extents->y1)
                extents->y1 = y1;
            if (y2 > extents->y2)
                extents->y2 = y2;
            x += glyph->info.xOff;
            y += glyph->info.yOff;
        }
    }
}

#define NeedsComponent(f) (PICT_FORMAT_A(f) != 0 && PICT_FORMAT_RGB(f) != 0)

void vivante_unaccel_Glyphs(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
    PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int nlist,
    GlyphListPtr list, GlyphPtr * glyphs)
{
    ScreenPtr pScreen = pDst->pDrawable->pScreen;
    PixmapPtr pMaskPixmap = NULL;
    PicturePtr pMask;
    int width = 0, height = 0, x, y, n;
    int xDst = list->xOff, yDst = list->yOff;
    BoxRec extents = { 0, 0, 0, 0 };

    if (maskFormat) {
        xRectangle rect;
        CARD32 component_alpha;
        GCPtr pGC;
        int error;

        GlyphExtents(nlist, list, glyphs, &extents);
        if (extents.x2 <= extents.x1 || extents.y2 <= extents.y1)
            return;

        width = extents.x2 - extents.x1;
        height = extents.y2 - extents.y1;
        pMaskPixmap = pScreen->CreatePixmap(pScreen, width, height,
                                             maskFormat->depth,
                                             CREATE_PIXMAP_USAGE_SCRATCH);
        if (!pMaskPixmap)
            return;

        component_alpha = NeedsComponent(maskFormat->format);
        pMask = CreatePicture(0, &pMaskPixmap->drawable, maskFormat,
                     CPComponentAlpha, &component_alpha, serverClient, &error);
        if (!pMask) {
            pScreen->DestroyPixmap(pMaskPixmap);
            return;
        }

        pGC = GetScratchGC(pMaskPixmap->drawable.depth, pScreen);
        ValidateGC(&pMaskPixmap->drawable, pGC);
        rect.x = 0;
        rect.y = 0;
        rect.width = width;
        rect.height = height;
        pGC->ops->PolyFillRect(&pMaskPixmap->drawable, pGC, 1, &rect);
        FreeScratchGC(pGC);
        x = -extents.x1;
        y = -extents.y1;
    } else {
        pMask = pDst;
    	x = 0;
    	y = 0;
    }

    while (nlist--) {
        x += list->xOff;
        y += list->yOff;
        n = list->len;
        while (n--) {
            GlyphPtr glyph = *glyphs++;
            PicturePtr g = GetGlyphPicture(glyph, pScreen);

            if (g) {
            	int dstx = x - glyph->info.x;
            	int dsty = y - glyph->info.y;
                if (maskFormat) {
                    CompositePicture(PictOpAdd, g, NULL, pMask,
                                     0, 0, 0, 0, dstx, dsty,
                                     glyph->info.width, glyph->info.height);
                } else {
                    CompositePicture(op, pSrc, g, pDst,
                                     xSrc + dstx - xDst,
                                     ySrc + dsty - yDst,
                                     0, 0, dstx, dsty,
                                     glyph->info.width, glyph->info.height);
                }
            }
            x += glyph->info.xOff;
            y += glyph->info.yOff;
        }
        list++;
    }

    if (maskFormat) {
        x = extents.x1;
        y = extents.y1;
        CompositePicture(op, pSrc, pMask, pDst,
                         xSrc + x - xDst, ySrc + y - yDst, 0, 0, x, y,
                         width, height);
        FreePicture(pMask, 0);
        pScreen->DestroyPixmap(pMaskPixmap);
    }
}

void vivante_unaccel_Triangles(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
    PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int ntri, xTriangle *tri)
{
    vivante_prepare_picture(pDst, ACCESS_RW);
    vivante_prepare_picture(pSrc, ACCESS_RO);
    fbTriangles(op, pSrc, pDst, maskFormat, xSrc, ySrc, ntri, tri);
    vivante_finish_picture(pSrc, ACCESS_RO);
    vivante_finish_picture(pDst, ACCESS_RW);
}

void vivante_unaccel_Trapezoids(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
    PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int ntrap,
    xTrapezoid * traps)
{
    vivante_prepare_picture(pDst, ACCESS_RW);
    vivante_prepare_picture(pSrc, ACCESS_RO);
    fbTrapezoids(op, pSrc, pDst, maskFormat, xSrc, ySrc, ntrap, traps);
    vivante_finish_picture(pSrc, ACCESS_RO);
    vivante_finish_picture(pDst, ACCESS_RW);
}

void vivante_unaccel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
    PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
    INT16 xDst, INT16 yDst, CARD16 w, CARD16 h)
{
    vivante_prepare_picture(pDst, ACCESS_RW);
    vivante_prepare_picture(pSrc, ACCESS_RO);
    if (pMask)
        vivante_prepare_picture(pMask, ACCESS_RO);
    fbComposite(op, pSrc, pMask, pDst, xSrc, ySrc, xMask, yMask,
          xDst, yDst, w, h);
    if (pMask)
        vivante_finish_picture(pMask, ACCESS_RO);
    vivante_finish_picture(pSrc, ACCESS_RO);
    vivante_finish_picture(pDst, ACCESS_RW);
}

void vivante_unaccel_AddTriangles(PicturePtr pPicture, INT16 x_off, INT16 y_off,
    int ntri, xTriangle *tris)
{
    vivante_prepare_picture(pPicture, ACCESS_RW);
    fbAddTriangles(pPicture, x_off, y_off, ntri, tris);
    vivante_finish_picture(pPicture, ACCESS_RW);
}

void vivante_unaccel_AddTraps(PicturePtr pPicture, INT16 x_off, INT16 y_off,
    int ntrap, xTrap *traps)
{
    vivante_prepare_picture(pPicture, ACCESS_RW);
    fbAddTraps(pPicture, x_off, y_off, ntrap, traps);
    vivante_finish_picture(pPicture, ACCESS_RW);
}
