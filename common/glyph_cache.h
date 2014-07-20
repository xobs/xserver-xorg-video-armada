#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H

typedef void (*glyph_upload_t)(ScreenPtr, PicturePtr, GlyphPtr,
			       PicturePtr, unsigned, unsigned);

Bool glyph_cache_init(ScreenPtr pScreen, glyph_upload_t,
	const unsigned *formats, size_t num_formats, unsigned usage_hint);

PicturePtr glyph_cache_only(ScreenPtr pScreen, GlyphPtr pGlyph, xPoint *pos);
PicturePtr glyph_cache(ScreenPtr pScreen, GlyphPtr pGlyph, xPoint *pos);
Bool glyph_cache_preload(ScreenPtr pScreen, int nlist, GlyphListPtr list,
	GlyphPtr *glyphs);

#define NeedsComponent(f) (PICT_FORMAT_A(f) != 0 && PICT_FORMAT_RGB(f) != 0)

#endif
