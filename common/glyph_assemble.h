#ifndef GLYPH_ASSEMBLE_H
#define GLYPH_ASSEMBLE_H

struct glyph_render {
	PicturePtr picture;
	xPoint glyph_pos;
	unsigned short width, height;
	short dest_x, dest_y;
};

int glyphs_assemble(ScreenPtr pScreen, struct glyph_render **gp,
	BoxPtr extents, int nlist, GlyphListPtr list, GlyphPtr *glyphs);

#endif
