#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "picturestr.h"
#include "pixmapstr.h"
#include "pictureutil.h"
#include "pixmaputil.h"

/*
 * Returns TRUE and the pixel value in COLOUR if the picture
 * represents a solid surface of constant colour.
 */
Bool picture_is_solid(PicturePtr pict, CARD32 *colour)
{
	if (pict->pDrawable) {
		DrawablePtr pDraw = pict->pDrawable;

		if (pDraw->width == 1 && pDraw->height == 1 && pict->repeat) {
			if (colour)
				*colour = get_first_pixel(pDraw);
			return TRUE;
		}
	} else {
		SourcePict *sp = pict->pSourcePict;

		if (sp->type == SourcePictTypeSolidFill) {
			if (colour)
				*colour = sp->solidFill.color;
			return TRUE;
		}
	}

	return FALSE;
}
