#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pixmaputil.h"

PixmapPtr drawable_pixmap_offset(DrawablePtr pDrawable, xPoint *offset)
{
	PixmapPtr pPixmap;

	offset->x = 0;
	offset->y = 0;
	if (OnScreenDrawable(pDrawable->type)) {
		WindowPtr pWin;

		pWin = container_of(pDrawable, struct _Window, drawable);

		pPixmap = pDrawable->pScreen->GetWindowPixmap(pWin);
#ifdef COMPOSITE
		offset->x = -pPixmap->screen_x;
		offset->y = -pPixmap->screen_y;
#endif
	} else {
		pPixmap = container_of(pDrawable, struct _Pixmap, drawable);
	}
	return pPixmap;
}
