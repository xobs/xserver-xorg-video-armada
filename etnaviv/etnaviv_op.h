#ifndef ETNAVIV_OP_H
#define ETNAVIV_OP_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>

#include "xf86.h"

#define VIVANTE_MAX_2D_RECTS	255

struct etna_bo;
struct etnaviv;

#define UNKNOWN_FORMAT	0x1f

struct etnaviv_format {
	uint32_t
		format:5,
		swizzle:2,
		tile:1;
};

struct etnaviv_blend_op {
	uint32_t alpha_mode;
	uint8_t src_alpha;
	uint8_t dst_alpha;
};

struct etnaviv_blit_buf {
	struct etnaviv_format format;
	struct etnaviv_pixmap *pixmap;
	struct etna_bo *bo;
	unsigned pitch;
	xPoint offset;
};

#define INIT_BLIT_BUF(_fmt,_pix,_bo,_pitch,_off)	\
	((struct etnaviv_blit_buf){			\
		.format = _fmt,				\
		.pixmap = _pix,				\
		.bo = _bo,				\
		.pitch = _pitch,			\
		.offset	= _off,				\
	})

#define INIT_BLIT_PIX(_pix, _fmt, _off) \
	INIT_BLIT_BUF((_fmt), (_pix), (_pix)->etna_bo, (_pix)->pitch, (_off))

#define INIT_BLIT_BO(_bo, _pitch, _fmt, _off) \
	INIT_BLIT_BUF((_fmt), NULL, (_bo), (_pitch), (_off))

#define INIT_BLIT_NULL	\
	INIT_BLIT_BUF({ }, NULL, NULL, 0, ZERO_OFFSET)

#define ZERO_OFFSET ((xPoint){ 0, 0 })

struct etnaviv_de_op {
	struct etnaviv_blit_buf dst;
	struct etnaviv_blit_buf src;
	const struct etnaviv_blend_op *blend_op;
	const BoxRec *clip;
	unsigned rop, cmd;
	Bool brush;
	uint32_t fg_colour;
};

void etnaviv_de_start(struct etnaviv *etnaviv, const struct etnaviv_de_op *op);
void etnaviv_de_end(struct etnaviv *etnaviv);
void etnaviv_de_op(struct etnaviv *etnaviv, const struct etnaviv_de_op *op,
	const BoxRec *pBox, size_t nBox);
void etnaviv_emit(struct etnaviv *etnaviv);
void etnaviv_flush(struct etnaviv *etnaviv);

#endif
