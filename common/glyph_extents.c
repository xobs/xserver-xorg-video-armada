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

#include "glyph_extents.h"

void GlyphExtents(int nlist, GlyphListPtr list, GlyphPtr *glyphs,
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
