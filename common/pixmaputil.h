#ifndef PIXMAPUTIL_H
#define PIXMAPUTIL_H

#include <X11/Xdefs.h>
#include "list.h"
#include "pixmapstr.h"
#include "scrnintstr.h"
#include "windowstr.h"
#include "utils.h"

char *drawable_desc(DrawablePtr pDraw, char *str, size_t n);
CARD32 get_first_pixel(DrawablePtr pDraw);

static inline PixmapPtr drawable_pixmap(DrawablePtr pDrawable)
{
	if (OnScreenDrawable(pDrawable->type)) {
		WindowPtr pWin;

		pWin = container_of(pDrawable, struct _Window, drawable);

		return pDrawable->pScreen->GetWindowPixmap(pWin);
	} else {
		return container_of(pDrawable, struct _Pixmap, drawable);
	}
}

PixmapPtr drawable_pixmap_deltas(DrawablePtr pDrawable, int *x, int *y);
PixmapPtr drawable_pixmap_offset(DrawablePtr pDrawable, xPoint *offset);

/*
 * drawable_contains: returns true if x,y,w,h is contained entirely
 * within the drawable
 */
static inline Bool drawable_contains(DrawablePtr drawable,
	int x, int y, int w, int h)
{
	if (x < 0 ||
	    y < 0 ||
	    x + w > drawable->width ||
	    y + h > drawable->height)
		return FALSE;

	return TRUE;
}

#endif
