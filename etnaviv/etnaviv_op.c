#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "fb.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"

#include <etnaviv/etna.h>
#include <etnaviv/etna_bo.h>
#include <etnaviv/state.xml.h>
#include <etnaviv/state_2d.xml.h>

#define BATCH_SETUP_START(etp)						\
	do {								\
		struct etnaviv *_et = etp;				\
		_et->batch_setup_size = 0;				\
		_et->batch_size = 0;					\
		_et->reloc_size = 0;					\
	} while (0)

#define BATCH_SETUP_END(etp)						\
	do {								\
		struct etnaviv *__et = etp;				\
		__et->batch_setup_size = __et->batch_size;		\
		__et->reloc_setup_size = __et->reloc_size;		\
	} while (0)

#define BATCH_OP_START(etp)						\
	do {								\
		struct etnaviv *__et = etp;				\
		__et->batch_size = __et->batch_setup_size;		\
		__et->reloc_size = __et->reloc_setup_size;		\
	} while (0)

#define EMIT(etp, val)							\
	do {								\
		struct etnaviv *_et = etp;				\
		assert(_et->batch_size < MAX_BATCH_SIZE);		\
		_et->batch[_et->batch_size++] = val;			\
	} while (0)

#define EMIT_RELOC(etp, _bo, _off, _wr)					\
	do {								\
		struct etnaviv *__et = etp;				\
		struct etnaviv_reloc *r = &__et->reloc[__et->reloc_size++]; \
		r->bo = _bo;						\
		r->batch_index = __et->batch_size;			\
		r->write = _wr;						\
		EMIT(__et, _off);					\
	} while (0)

#define EMIT_LOADSTATE(etp, st, num)					\
	do {								\
		struct etnaviv *__et = etp;				\
		assert(!(__et->batch_size & 1));			\
		EMIT(__et, VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE |	\
			   VIV_FE_LOAD_STATE_HEADER_COUNT(num) |	\
			   VIV_FE_LOAD_STATE_HEADER_OFFSET((st) >> 2));	\
	} while (0)

#define EMIT_DRAW_2D(etp, count)					\
	do {								\
		struct etnaviv *__et = etp;				\
		assert(!(__et->batch_size & 1));			\
		EMIT(__et, VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |		\
			   VIV_FE_DRAW_2D_HEADER_COUNT(count));		\
		/* next word is unused */				\
		__et->batch_size ++;					\
	} while (0)

#define EMIT_STALL(etp, from, to)					\
	do {								\
		struct etnaviv *__et = etp;				\
		assert(!(__et->batch_size & 1));			\
		EMIT(__et, VIV_FE_STALL_HEADER_OP_STALL);		\
		EMIT(__et, VIV_FE_STALL_TOKEN_FROM(from) |		\
			   VIV_FE_STALL_TOKEN_TO(to));			\
	} while (0)

#define EMIT_NOP(etp)							\
	do {								\
		struct etnaviv *__et = etp;				\
		assert(!(__et->batch_size & 1));			\
		EMIT(__et, VIV_FE_NOP_HEADER_OP_NOP);			\
		EMIT(__et, 0);						\
	} while (0)

#define EMIT_ALIGN(etp)							\
	do {								\
		struct etnaviv *__et = etp;				\
		__et->batch_size += __et->batch_size & 1;		\
	} while (0)

static inline uint32_t etnaviv_src_config(struct etnaviv_format fmt,
	Bool relative)
{
	uint32_t src_cfg;

	src_cfg = VIVS_DE_SRC_CONFIG_PE10_SOURCE_FORMAT(fmt.format) |
		  VIVS_DE_SRC_CONFIG_TRANSPARENCY(0) |
		  VIVS_DE_SRC_CONFIG_LOCATION_MEMORY |
		  VIVS_DE_SRC_CONFIG_PACK_PACKED8 |
		  VIVS_DE_SRC_CONFIG_SWIZZLE(fmt.swizzle) |
		  VIVS_DE_SRC_CONFIG_SOURCE_FORMAT(fmt.format);

	if (relative)
		src_cfg |= VIVS_DE_SRC_CONFIG_SRC_RELATIVE_RELATIVE;

	if (fmt.tile)
		src_cfg |= VIVS_DE_SRC_CONFIG_TILED_ENABLE;

	return src_cfg;
}

static void etnaviv_set_source_bo(struct etnaviv *etnaviv,
	const struct etnaviv_blit_buf *buf, unsigned int src_origin_mode)
{
	uint32_t src_cfg = etnaviv_src_config(buf->format, src_origin_mode ==
					       SRC_ORIGIN_RELATIVE);

	EMIT_LOADSTATE(etnaviv, VIVS_DE_SRC_ADDRESS, 5);
	EMIT_RELOC(etnaviv, buf->bo, 0, FALSE);
	EMIT(etnaviv, VIVS_DE_SRC_STRIDE_STRIDE(buf->pitch));
	EMIT(etnaviv, VIVS_DE_SRC_ROTATION_CONFIG_ROTATION_DISABLE);
	EMIT(etnaviv, src_cfg);
	EMIT(etnaviv, VIVS_DE_SRC_ORIGIN_X(buf->offset.x) |
		      VIVS_DE_SRC_ORIGIN_Y(buf->offset.y));
	EMIT_ALIGN(etnaviv);
}

static void etnaviv_set_dest_bo(struct etnaviv *etnaviv,
	const struct etnaviv_blit_buf *buf, uint32_t cmd)
{
	uint32_t dst_cfg;

	dst_cfg = VIVS_DE_DEST_CONFIG_FORMAT(buf->format.format) | cmd |
		  VIVS_DE_DEST_CONFIG_SWIZZLE(buf->format.swizzle);

	if (buf->format.tile)
		dst_cfg |= VIVS_DE_DEST_CONFIG_TILED_ENABLE;

	EMIT_LOADSTATE(etnaviv, VIVS_DE_DEST_ADDRESS, 4);
	EMIT_RELOC(etnaviv, buf->bo, 0, TRUE);
	EMIT(etnaviv, VIVS_DE_DEST_STRIDE_STRIDE(buf->pitch));
	EMIT(etnaviv, VIVS_DE_DEST_ROTATION_CONFIG_ROTATION_DISABLE);
	EMIT(etnaviv, dst_cfg);
	EMIT_ALIGN(etnaviv);
}

static void etnaviv_emit_rop_clip(struct etnaviv *etnaviv, unsigned fg_rop,
	unsigned bg_rop, const BoxRec *clip, xPoint offset)
{
	EMIT_LOADSTATE(etnaviv, VIVS_DE_ROP, clip ? 3 : 1);
	EMIT(etnaviv, VIVS_DE_ROP_ROP_FG(fg_rop) |
		      VIVS_DE_ROP_ROP_BG(bg_rop) |
		      VIVS_DE_ROP_TYPE_ROP4);
	if (clip) {
		EMIT(etnaviv,
		     VIVS_DE_CLIP_TOP_LEFT_X(clip->x1 + offset.x) |
		     VIVS_DE_CLIP_TOP_LEFT_Y(clip->y1 + offset.y));
		EMIT(etnaviv,
		     VIVS_DE_CLIP_BOTTOM_RIGHT_X(clip->x2 + offset.x) |
		     VIVS_DE_CLIP_BOTTOM_RIGHT_Y(clip->y2 + offset.y));
	}
}

static void etnaviv_emit_brush(struct etnaviv *etnaviv, uint32_t fg)
{
	EMIT_LOADSTATE(etnaviv, VIVS_DE_PATTERN_MASK_LOW, 4);
	EMIT(etnaviv, ~0);
	EMIT(etnaviv, ~0);
	EMIT(etnaviv, 0);
	EMIT(etnaviv, fg);
	EMIT_ALIGN(etnaviv);
	EMIT_LOADSTATE(etnaviv, VIVS_DE_PATTERN_CONFIG, 1);
	EMIT(etnaviv, VIVS_DE_PATTERN_CONFIG_INIT_TRIGGER(3));
}

static void etnaviv_set_blend(struct etnaviv *etnaviv,
	const struct etnaviv_blend_op *op)
{
	if (!op) {
		EMIT_LOADSTATE(etnaviv, VIVS_DE_ALPHA_CONTROL, 1);
		EMIT(etnaviv, VIVS_DE_ALPHA_CONTROL_ENABLE_OFF);
	} else {
		Bool pe20 = VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20);

		EMIT_LOADSTATE(etnaviv, VIVS_DE_ALPHA_CONTROL, 2);
		EMIT(etnaviv,
		     VIVS_DE_ALPHA_CONTROL_ENABLE_ON |
		     VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_SRC_ALPHA(op->src_alpha) |
		     VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_DST_ALPHA(op->dst_alpha));
		EMIT(etnaviv, op->alpha_mode);
		EMIT_ALIGN(etnaviv);

		if (pe20) {
			EMIT_LOADSTATE(etnaviv, VIVS_DE_GLOBAL_SRC_COLOR, 3);
			EMIT(etnaviv, op->src_alpha << 24);
			EMIT(etnaviv, op->dst_alpha << 24);
			EMIT(etnaviv,
			     VIVS_DE_COLOR_MULTIPLY_MODES_SRC_PREMULTIPLY_DISABLE |
			     VIVS_DE_COLOR_MULTIPLY_MODES_DST_PREMULTIPLY_DISABLE |
			     VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_DISABLE |
			     VIVS_DE_COLOR_MULTIPLY_MODES_DST_DEMULTIPLY_DISABLE);
		}
	}
}

static void etnaviv_emit_2d_draw(struct etnaviv *etnaviv, const BoxRec *pbox,
	size_t n, xPoint offset)
{
	size_t i;

	EMIT_DRAW_2D(etnaviv, n == 256 ? 0 : n);

	for (i = 0; i < n; i++, pbox++) {
		EMIT(etnaviv,
		     VIV_FE_DRAW_2D_TOP_LEFT_X(offset.x + pbox->x1) |
		     VIV_FE_DRAW_2D_TOP_LEFT_Y(offset.y + pbox->y1));
		EMIT(etnaviv,
		     VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(offset.x + pbox->x2) |
		     VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(offset.y + pbox->y2));
	}
}

static size_t etnaviv_size_2d_draw(struct etnaviv *etnaviv, size_t n)
{
	return 2 + 2 * n;
}

void etnaviv_de_start(struct etnaviv *etnaviv, const struct etnaviv_de_op *op)
{
	BATCH_SETUP_START(etnaviv);

	if (op->src.bo)
		etnaviv_set_source_bo(etnaviv, &op->src, op->src_origin_mode);
	etnaviv_set_dest_bo(etnaviv, &op->dst, op->cmd);
	etnaviv_set_blend(etnaviv, op->blend_op);
	if (op->brush)
		etnaviv_emit_brush(etnaviv, op->fg_colour);
	etnaviv_emit_rop_clip(etnaviv, op->rop, op->rop, op->clip,
			      op->dst.offset);

	BATCH_SETUP_END(etnaviv);
}

void etnaviv_de_end(struct etnaviv *etnaviv)
{
	if (etnaviv->gc320_etna_bo) {
		BoxRec box = { 0, 1, 1, 2 };

		/* Append the GC320 workaround - 6 + 6 + 2 + 4 + 4 */
		etnaviv_set_source_bo(etnaviv, &etnaviv->gc320_wa_src,
				      SRC_ORIGIN_RELATIVE);
		etnaviv_set_dest_bo(etnaviv, &etnaviv->gc320_wa_dst,
				    VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT);
		etnaviv_set_blend(etnaviv, NULL);
		etnaviv_emit_rop_clip(etnaviv, 0xcc, 0xcc, &box, ZERO_OFFSET);
		etnaviv_emit_2d_draw(etnaviv, &box, 1, ZERO_OFFSET);
	}

	/* Append a flush, semaphore and stall to ensure that the FE */
	EMIT_LOADSTATE(etnaviv, VIVS_GL_FLUSH_CACHE, 1);
	EMIT(etnaviv, VIVS_GL_FLUSH_CACHE_PE2D);
	EMIT_LOADSTATE(etnaviv, VIVS_GL_SEMAPHORE_TOKEN, 1);
	EMIT(etnaviv, VIVS_GL_SEMAPHORE_TOKEN_FROM(SYNC_RECIPIENT_FE) |
		      VIVS_GL_SEMAPHORE_TOKEN_TO(SYNC_RECIPIENT_PE));
	EMIT_STALL(etnaviv, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);

	if (etnaviv->gc320_etna_bo) {
		int i;

		for (i = 0; i < 20; i++)
			EMIT_NOP(etnaviv);
	}

	etnaviv_emit(etnaviv);
}

void etnaviv_de_op_src_origin(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op, xPoint src_origin, const BoxRec *dest)
{
	unsigned int high_wm = etnaviv->batch_de_high_watermark;
	size_t op_size = etnaviv_size_2d_draw(etnaviv, 1) + 6 + 2;
	xPoint offset = op->dst.offset;

	if (op_size > high_wm - etnaviv->batch_size) {
		etnaviv_de_end(etnaviv);
		BATCH_OP_START(etnaviv);
	}

	EMIT_LOADSTATE(etnaviv, VIVS_DE_SRC_ORIGIN, 1);
	EMIT(etnaviv, VIVS_DE_SRC_ORIGIN_X(src_origin.x) |
		      VIVS_DE_SRC_ORIGIN_Y(src_origin.y));
	EMIT_DRAW_2D(etnaviv, 1);
	EMIT(etnaviv,
	     VIV_FE_DRAW_2D_TOP_LEFT_X(offset.x + dest->x1) |
	     VIV_FE_DRAW_2D_TOP_LEFT_Y(offset.y + dest->y1));
	EMIT(etnaviv,
	     VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(offset.x + dest->x2) |
	     VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(offset.y + dest->y2));
	EMIT_LOADSTATE(etnaviv, 4, 1);
	EMIT(etnaviv, 0);
	EMIT_LOADSTATE(etnaviv, 4, 1);
	EMIT(etnaviv, 0);
	EMIT_LOADSTATE(etnaviv, 4, 1);
	EMIT(etnaviv, 0);
}

void etnaviv_de_op(struct etnaviv *etnaviv, const struct etnaviv_de_op *op,
	const BoxRec *pBox, size_t nBox)
{
	unsigned int high_wm = etnaviv->batch_de_high_watermark;

	assert(nBox);

	if (op->cmd == VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT &&
	    etnaviv_has_bugfix(etnaviv, BUGFIX_SINGLE_BITBLT_DRAW_OP)) {
		size_t op_size = etnaviv_size_2d_draw(etnaviv, 1) + 6;
		xPoint offset = op->dst.offset;

		while (nBox--) {
			if (op_size > high_wm - etnaviv->batch_size) {
				etnaviv_de_end(etnaviv);
				BATCH_OP_START(etnaviv);
			}

			etnaviv_emit_2d_draw(etnaviv, pBox++, 1, offset);

			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
		}		
	} else {
		unsigned int n;

		do {
			unsigned int remaining = high_wm - etnaviv->batch_size;

			if (remaining <= 8) {
				etnaviv_de_end(etnaviv);
				BATCH_OP_START(etnaviv);
				continue;
			}

			n = (remaining - 8) / 2;
			if (n > VIVANTE_MAX_2D_RECTS)
				n = VIVANTE_MAX_2D_RECTS;
			if (n > nBox)
				n = nBox;
			
			etnaviv_emit_2d_draw(etnaviv, pBox, n, op->dst.offset);

			pBox += n;
			nBox -= n;

			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
			EMIT_LOADSTATE(etnaviv, 4, 1);
			EMIT(etnaviv, 0);
		} while (nBox);
	}
}

void etnaviv_vr_op(struct etnaviv *etnaviv, struct etnaviv_vr_op *op,
	const BoxRec *dst, uint32_t x1, uint32_t y1,
	const BoxRec *boxes, size_t n)
{
	uint32_t cfg, offset, pitch;

	cfg = etnaviv_src_config(op->src.format, FALSE);
	offset = op->src_offsets ? op->src_offsets[0] : 0;
	pitch = op->src_pitches ? op->src_pitches[0] : op->src.pitch;

	BATCH_SETUP_START(etnaviv);
	EMIT_LOADSTATE(etnaviv, VIVS_DE_SRC_ADDRESS, 4);
	EMIT_RELOC(etnaviv, op->src.bo, offset, FALSE);
	EMIT(etnaviv, VIVS_DE_SRC_STRIDE_STRIDE(pitch));
	EMIT(etnaviv, VIVS_DE_SRC_ROTATION_CONFIG_ROTATION_DISABLE);
	EMIT(etnaviv, cfg);
	EMIT_ALIGN(etnaviv);

	if (op->src.format.planes > 1) {
		unsigned u = op->src.format.u;
		unsigned v = op->src.format.v;

		EMIT_LOADSTATE(etnaviv, VIVS_DE_UPLANE_ADDRESS, 4);
		EMIT_RELOC(etnaviv, op->src.bo, op->src_offsets[u], FALSE);
		EMIT(etnaviv, VIVS_DE_UPLANE_STRIDE_STRIDE(op->src_pitches[u]));
		EMIT_RELOC(etnaviv, op->src.bo, op->src_offsets[v], FALSE);
		EMIT(etnaviv, VIVS_DE_VPLANE_STRIDE_STRIDE(op->src_pitches[v]));
		EMIT_ALIGN(etnaviv);
	}

	etnaviv_set_dest_bo(etnaviv, &op->dst, op->cmd);

	EMIT_LOADSTATE(etnaviv, VIVS_DE_ALPHA_CONTROL, 1);
	EMIT(etnaviv, VIVS_DE_ALPHA_CONTROL_ENABLE_OFF);

	EMIT_LOADSTATE(etnaviv, VIVS_DE_STRETCH_FACTOR_LOW, 2);
	EMIT(etnaviv, op->h_scale);
	EMIT(etnaviv, op->v_scale);
	EMIT_ALIGN(etnaviv);

	EMIT_LOADSTATE(etnaviv, VIVS_DE_VR_SOURCE_IMAGE_LOW, 2);
	EMIT(etnaviv,
	     VIVS_DE_VR_SOURCE_IMAGE_LOW_LEFT(op->src_bounds.x1) |
	     VIVS_DE_VR_SOURCE_IMAGE_LOW_TOP(op->src_bounds.y1));
	EMIT(etnaviv,
	     VIVS_DE_VR_SOURCE_IMAGE_HIGH_RIGHT(op->src_bounds.x2) |
	     VIVS_DE_VR_SOURCE_IMAGE_HIGH_BOTTOM(op->src_bounds.y2));
	EMIT_ALIGN(etnaviv);

	while (n--) {
		BoxRec box = *boxes++;
		uint32_t x, y;

		x = x1 + (box.x1 - dst->x1) * op->h_scale;
		y = y1 + (box.y1 - dst->y1) * op->v_scale;

		/* Factor in the drawable offsets for the target position */
		box.x1 += op->dst.offset.x;
		box.y1 += op->dst.offset.y;
		box.x2 += op->dst.offset.x;
		box.y2 += op->dst.offset.y;

		/* 6 */
		EMIT_LOADSTATE(etnaviv, VIVS_DE_VR_SOURCE_ORIGIN_LOW, 4);
		EMIT(etnaviv, VIVS_DE_VR_SOURCE_ORIGIN_LOW_X(x));
		EMIT(etnaviv, VIVS_DE_VR_SOURCE_ORIGIN_HIGH_Y(y));

		EMIT(etnaviv,
		     VIVS_DE_VR_TARGET_WINDOW_LOW_LEFT(box.x1) |
		     VIVS_DE_VR_TARGET_WINDOW_LOW_TOP(box.y1));
		EMIT(etnaviv,
		     VIVS_DE_VR_TARGET_WINDOW_HIGH_RIGHT(box.x2) |
		     VIVS_DE_VR_TARGET_WINDOW_HIGH_BOTTOM(box.y2));
		EMIT_ALIGN(etnaviv);

		/* 2 */
		EMIT_LOADSTATE(etnaviv, VIVS_DE_VR_CONFIG, 1);
		EMIT(etnaviv, op->vr_op);
	}

	etnaviv_emit(etnaviv);
}

void etnaviv_flush(struct etnaviv *etnaviv)
{
	struct etna_ctx *ctx = etnaviv->ctx;
	etna_set_state(ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
	etna_set_state(ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
}
