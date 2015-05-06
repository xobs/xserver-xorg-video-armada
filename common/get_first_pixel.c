#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pixmapstr.h"
#include "pixmaputil.h"

CARD32 get_first_pixel(DrawablePtr pDraw)
{
	union { CARD32 c32; CARD16 c16; CARD8 c8; char c; } pixel;

	pDraw->pScreen->GetImage(pDraw, 0, 0, 1, 1, ZPixmap, ~0, &pixel.c);

	switch (pDraw->bitsPerPixel) {
	case 32:
		return pixel.c32;
	case 16:
		return pixel.c16;
	case 8:
	case 4:
	case 1:
		return pixel.c8;
	default:
		assert(0);
	}
}
