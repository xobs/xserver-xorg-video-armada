#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pixmaputil.h"

PixmapPtr drawable_pixmap_deltas(DrawablePtr pDrawable, int *x, int *y)
{
	PixmapPtr pPixmap;

	*x = *y = 0;
	if (OnScreenDrawable(pDrawable->type)) {
		WindowPtr pWin;

		pWin = container_of(pDrawable, struct _Window, drawable);

		pPixmap = pDrawable->pScreen->GetWindowPixmap(pWin);
#ifdef COMPOSITE
		*x = -pPixmap->screen_x;
		*y = -pPixmap->screen_y;
#endif
	} else {
		pPixmap = container_of(pDrawable, struct _Pixmap, drawable);
	}
	return pPixmap;
}

PixmapPtr drawable_pixmap_offset(DrawablePtr pDrawable, xPoint *offset)
{
	int x, y;
	PixmapPtr pix = drawable_pixmap_deltas(pDrawable, &x, &y);
	offset->x = x;
	offset->y = y;
	return pix;
}
