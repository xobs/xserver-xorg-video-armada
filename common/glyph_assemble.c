#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "fb.h"
#include "mipict.h"

#include "glyph_assemble.h"
#include "glyph_cache.h"
#include "glyph_extents.h"

static size_t glyph_list_count(int nlist, GlyphListPtr list)
{
	size_t total = 0;

	while (nlist--) {
		total += list->len;
		list++;
	}

	return total;
}

int glyphs_assemble(ScreenPtr pScreen, struct glyph_render **gp,
	BoxPtr extents, int nlist, GlyphListPtr list, GlyphPtr *glyphs)
{
	struct glyph_render *gr, *grp;
	int x, y, total;

	/*
	 * Preload the cache with the glyphs which we intend to use.
	 * This means we can avoid having to reset the destination
	 * for the PictOpAdd below.  If this fails, fallback.
	 */
	if (!glyph_cache_preload(pScreen, nlist, list, glyphs))
		return -1;

	GlyphExtents(nlist, list, glyphs, extents);
	if (extents->x2 <= extents->x1 || extents->y2 <= extents->y1)
		return 0;

	total = glyph_list_count(nlist, list);
	gr = malloc(sizeof(struct glyph_render) * total);
	if (!gr)
		return -1;

	x = -extents->x1;
	y = -extents->y1;
	grp = gr;

	while (nlist--) {
		int n = list->len;

		x += list->xOff;
		y += list->yOff;

		while (n--) {
			GlyphPtr glyph = *glyphs++;

			if (glyph->info.width && glyph->info.height) {
				grp->width = glyph->info.width;
				grp->height = glyph->info.height;
				grp->dest_x = x - glyph->info.x;
				grp->dest_y = y - glyph->info.y;
				grp->picture = glyph_cache_only(pScreen, glyph,
					      &grp->glyph_pos);
				if (!grp->picture) {
					free(gr);
					return -1;
				}
				grp++;
			}
			x += glyph->info.xOff;
			y += glyph->info.yOff;
		}
		list++;
	}

	*gp = gr;

	return grp - gr;
}
