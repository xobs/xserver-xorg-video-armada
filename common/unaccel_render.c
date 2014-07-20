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
#include "cpu_access.h"
#include "glyph_extents.h"
#include "unaccel.h"

static void prepare_cpu_picture(PicturePtr pPicture, int access)
{
    if (pPicture->pDrawable) {
        prepare_cpu_drawable(pPicture->pDrawable, access);
        if (pPicture->alphaMap)
            prepare_cpu_drawable(pPicture->alphaMap->pDrawable, access);
    }
}

static void finish_cpu_picture(PicturePtr pPicture, int access)
{
    if (pPicture->pDrawable) {
        if (pPicture->alphaMap)
            finish_cpu_drawable(pPicture->alphaMap->pDrawable, access);
        finish_cpu_drawable(pPicture->pDrawable, access);
    }
}

#define NeedsComponent(f) (PICT_FORMAT_A(f) != 0 && PICT_FORMAT_RGB(f) != 0)

void unaccel_Glyphs(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
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

void unaccel_Triangles(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
    PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int ntri, xTriangle *tri)
{
    prepare_cpu_picture(pDst, CPU_ACCESS_RW);
    prepare_cpu_picture(pSrc, CPU_ACCESS_RO);
    fbTriangles(op, pSrc, pDst, maskFormat, xSrc, ySrc, ntri, tri);
    finish_cpu_picture(pSrc, CPU_ACCESS_RO);
    finish_cpu_picture(pDst, CPU_ACCESS_RW);
}

void unaccel_Trapezoids(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
    PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int ntrap,
    xTrapezoid * traps)
{
    prepare_cpu_picture(pDst, CPU_ACCESS_RW);
    prepare_cpu_picture(pSrc, CPU_ACCESS_RO);
    fbTrapezoids(op, pSrc, pDst, maskFormat, xSrc, ySrc, ntrap, traps);
    finish_cpu_picture(pSrc, CPU_ACCESS_RO);
    finish_cpu_picture(pDst, CPU_ACCESS_RW);
}

void unaccel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
    PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
    INT16 xDst, INT16 yDst, CARD16 w, CARD16 h)
{
    prepare_cpu_picture(pDst, CPU_ACCESS_RW);
    prepare_cpu_picture(pSrc, CPU_ACCESS_RO);
    if (pMask)
        prepare_cpu_picture(pMask, CPU_ACCESS_RO);
    fbComposite(op, pSrc, pMask, pDst, xSrc, ySrc, xMask, yMask,
          xDst, yDst, w, h);
    if (pMask)
        finish_cpu_picture(pMask, CPU_ACCESS_RO);
    finish_cpu_picture(pSrc, CPU_ACCESS_RO);
    finish_cpu_picture(pDst, CPU_ACCESS_RW);
}

void unaccel_AddTriangles(PicturePtr pPicture, INT16 x_off, INT16 y_off,
    int ntri, xTriangle *tris)
{
    prepare_cpu_picture(pPicture, CPU_ACCESS_RW);
    fbAddTriangles(pPicture, x_off, y_off, ntri, tris);
    finish_cpu_picture(pPicture, CPU_ACCESS_RW);
}

void unaccel_AddTraps(PicturePtr pPicture, INT16 x_off, INT16 y_off,
    int ntrap, xTrap *traps)
{
    prepare_cpu_picture(pPicture, CPU_ACCESS_RW);
    fbAddTraps(pPicture, x_off, y_off, ntrap, traps);
    finish_cpu_picture(pPicture, CPU_ACCESS_RW);
}
