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

#define LOADSTATE(st, num)						\
	(VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE |			\
	 VIV_FE_LOAD_STATE_HEADER_COUNT(num) |				\
	 VIV_FE_LOAD_STATE_HEADER_OFFSET((st) >> 2))

#define DRAW2D(count)							\
	(VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |				\
	 VIV_FE_DRAW_2D_HEADER_COUNT(count))

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

#define EL_START(etp, max_sz)						\
	do {								\
		struct etnaviv *_et = etp;				\
		unsigned int _batch_size = _et->batch_size;		\
		unsigned int _batch_max = _batch_size + 4 * max_sz;	\
		uint32_t *_batch = &_et->batch[_batch_size];		\
		assert(_batch_max <= MAX_BATCH_SIZE)

#define EL_END()							\
		_batch_size = _batch - _et->batch;			\
		_batch_size += _batch_size & 1;				\
		assert(_batch_size <= _batch_max);			\
		_et->batch_size = _batch_size;				\
	} while (0)

#define EL_ALIGN()	_batch += (_batch - _et->batch) & 1
#define EL_SKIP()	_batch++
#define EL(val)		*_batch++ = val

#define EL_RELOC(_bo, _off, _wr)					\
	do {								\
		etnaviv_add_reloc(_et, _bo, _wr, _batch - _et->batch);	\
		EL(_off);						\
	} while (0)

#define EL_NOP()							\
	do {								\
		EL(VIV_FE_NOP_HEADER_OP_NOP);				\
		EL_SKIP();						\
	} while (0)

#define EL_STALL(_from, _to)						\
	do {								\
		EL(VIV_FE_STALL_HEADER_OP_STALL);			\
		EL(VIV_FE_STALL_TOKEN_FROM(_from) |			\
		   VIV_FE_STALL_TOKEN_TO(_to));				\
	} while (0)

static void etnaviv_add_reloc(struct etnaviv *etnaviv, struct etna_bo *bo,
	int write, unsigned int batch_index)
{
	struct etnaviv_reloc *r = &etnaviv->reloc[etnaviv->reloc_size++];

	r->bo = bo;
	r->batch_index = batch_index;
	r->write = write;
}

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

	EL_START(etnaviv, 6);
	EL(LOADSTATE(VIVS_DE_SRC_ADDRESS, 5));
	EL_RELOC(buf->bo, 0, FALSE);
	EL(VIVS_DE_SRC_STRIDE_STRIDE(buf->pitch));
	EL(VIVS_DE_SRC_ROTATION_CONFIG_ROTATION_DISABLE);
	EL(src_cfg);
	EL(VIVS_DE_SRC_ORIGIN_X(buf->offset.x) |
	   VIVS_DE_SRC_ORIGIN_Y(buf->offset.y));
	EL_END();
}

static void etnaviv_set_dest_bo(struct etnaviv *etnaviv,
	const struct etnaviv_blit_buf *buf, uint32_t cmd)
{
	uint32_t dst_cfg;

	dst_cfg = VIVS_DE_DEST_CONFIG_FORMAT(buf->format.format) | cmd |
		  VIVS_DE_DEST_CONFIG_SWIZZLE(buf->format.swizzle);

	if (buf->format.tile)
		dst_cfg |= VIVS_DE_DEST_CONFIG_TILED_ENABLE;

	EL_START(etnaviv, 6);
	EL(LOADSTATE(VIVS_DE_DEST_ADDRESS, 4));
	EL_RELOC(buf->bo, 0, TRUE);
	EL(VIVS_DE_DEST_STRIDE_STRIDE(buf->pitch));
	EL(VIVS_DE_DEST_ROTATION_CONFIG_ROTATION_DISABLE);
	EL(dst_cfg);
	EL_END();
}

static void etnaviv_emit_rop_clip(struct etnaviv *etnaviv, unsigned fg_rop,
	unsigned bg_rop, const BoxRec *clip, xPoint offset)
{
	EL_START(etnaviv, 4);
	EL(LOADSTATE(VIVS_DE_ROP, clip ? 3 : 1));
	EL(VIVS_DE_ROP_ROP_FG(fg_rop) |
	   VIVS_DE_ROP_ROP_BG(bg_rop) |
	   VIVS_DE_ROP_TYPE_ROP4);
	if (clip) {
		EL(VIVS_DE_CLIP_TOP_LEFT_X(clip->x1 + offset.x) |
		   VIVS_DE_CLIP_TOP_LEFT_Y(clip->y1 + offset.y));
		EL(VIVS_DE_CLIP_BOTTOM_RIGHT_X(clip->x2 + offset.x) |
		   VIVS_DE_CLIP_BOTTOM_RIGHT_Y(clip->y2 + offset.y));
	}
	EL_END();
}

static void etnaviv_emit_brush(struct etnaviv *etnaviv, uint32_t fg)
{
	EL_START(etnaviv, 8);
	EL(LOADSTATE(VIVS_DE_PATTERN_MASK_LOW, 4));
	EL(~0);
	EL(~0);
	EL(0);
	EL(fg);
	EL_ALIGN();
	EL(LOADSTATE(VIVS_DE_PATTERN_CONFIG, 1));
	EL(VIVS_DE_PATTERN_CONFIG_INIT_TRIGGER(3));
	EL_END();
}

static void etnaviv_set_blend(struct etnaviv *etnaviv,
	const struct etnaviv_blend_op *op)
{
	EL_START(etnaviv, 8);
	if (!op) {
		EL(LOADSTATE(VIVS_DE_ALPHA_CONTROL, 1));
		EL(VIVS_DE_ALPHA_CONTROL_ENABLE_OFF);
	} else {
		Bool pe20 = VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20);

		EL(LOADSTATE(VIVS_DE_ALPHA_CONTROL, 2));
		EL(VIVS_DE_ALPHA_CONTROL_ENABLE_ON |
		   VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_SRC_ALPHA(op->src_alpha) |
		   VIVS_DE_ALPHA_CONTROL_PE10_GLOBAL_DST_ALPHA(op->dst_alpha));
		EL(op->alpha_mode);
		EL_ALIGN();

		if (pe20) {
			EL(LOADSTATE(VIVS_DE_GLOBAL_SRC_COLOR, 3));
			EL(op->src_alpha << 24);
			EL(op->dst_alpha << 24);
			EL(VIVS_DE_COLOR_MULTIPLY_MODES_SRC_PREMULTIPLY_DISABLE |
			   VIVS_DE_COLOR_MULTIPLY_MODES_DST_PREMULTIPLY_DISABLE |
			   VIVS_DE_COLOR_MULTIPLY_MODES_SRC_GLOBAL_PREMULTIPLY_DISABLE |
			   VIVS_DE_COLOR_MULTIPLY_MODES_DST_DEMULTIPLY_DISABLE);
		}
	}
	EL_END();
}

static size_t etnaviv_size_2d_draw(struct etnaviv *etnaviv, size_t n)
{
	return 2 + 2 * n;
}

static void etnaviv_emit_2d_draw(struct etnaviv *etnaviv, const BoxRec *pbox,
	size_t n, xPoint offset)
{
	size_t i;

	EL_START(etnaviv, etnaviv_size_2d_draw(etnaviv, n));
	EL(DRAW2D(n & 255));
	EL_SKIP();

	for (i = 0; i < n; i++, pbox++) {
		EL(VIV_FE_DRAW_2D_TOP_LEFT_X(offset.x + pbox->x1) |
		   VIV_FE_DRAW_2D_TOP_LEFT_Y(offset.y + pbox->y1));
		EL(VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(offset.x + pbox->x2) |
		   VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(offset.y + pbox->y2));
	}
	EL_END();
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
	EL_START(etnaviv, 46);
	EL(LOADSTATE(VIVS_GL_FLUSH_CACHE, 1));
	EL(VIVS_GL_FLUSH_CACHE_PE2D);
	EL(LOADSTATE(VIVS_GL_SEMAPHORE_TOKEN, 1));
	EL(VIVS_GL_SEMAPHORE_TOKEN_FROM(SYNC_RECIPIENT_FE) |
	   VIVS_GL_SEMAPHORE_TOKEN_TO(SYNC_RECIPIENT_PE));
	EL_STALL(SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);

	if (etnaviv->gc320_etna_bo) {
		int i;

		for (i = 0; i < 20; i++)
			EL_NOP();
	}
	EL_END();

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

	EL_START(etnaviv, op_size);
	EL(LOADSTATE(VIVS_DE_SRC_ORIGIN, 1));
	EL(VIVS_DE_SRC_ORIGIN_X(src_origin.x) |
	   VIVS_DE_SRC_ORIGIN_Y(src_origin.y));
	EL(DRAW2D(1));
	EL_SKIP();
	EL(VIV_FE_DRAW_2D_TOP_LEFT_X(offset.x + dest->x1) |
	   VIV_FE_DRAW_2D_TOP_LEFT_Y(offset.y + dest->y1));
	EL(VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(offset.x + dest->x2) |
	   VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(offset.y + dest->y2));
	EL(LOADSTATE(4, 1));
	EL(0);
	EL(LOADSTATE(4, 1));
	EL(0);
	EL(LOADSTATE(4, 1));
	EL(0);
	EL_END();
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

			EL_START(etnaviv, op_size);
			EL(DRAW2D(1));
			EL_SKIP();
			EL(VIV_FE_DRAW_2D_TOP_LEFT_X(offset.x + pBox->x1) |
			   VIV_FE_DRAW_2D_TOP_LEFT_Y(offset.y + pBox->y1));
			EL(VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(offset.x + pBox->x2) |
			   VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(offset.y + pBox->y2));
			EL(LOADSTATE(4, 1));
			EL(0);
			EL(LOADSTATE(4, 1));
			EL(0);
			EL(LOADSTATE(4, 1));
			EL(0);
			EL_END();
			pBox++;
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

			EL_START(etnaviv, 6);
			EL(LOADSTATE(4, 1));
			EL(0);
			EL(LOADSTATE(4, 1));
			EL(0);
			EL(LOADSTATE(4, 1));
			EL(0);
			EL_END();
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
	EL_START(etnaviv, 12);
	EL(LOADSTATE(VIVS_DE_SRC_ADDRESS, 4));
	EL_RELOC(op->src.bo, offset, FALSE);
	EL(VIVS_DE_SRC_STRIDE_STRIDE(pitch));
	EL(VIVS_DE_SRC_ROTATION_CONFIG_ROTATION_DISABLE);
	EL(cfg);
	EL_ALIGN();

	if (op->src.format.planes > 1) {
		unsigned u = op->src.format.u;
		unsigned v = op->src.format.v;

		EL(LOADSTATE(VIVS_DE_UPLANE_ADDRESS, 4));
		EL_RELOC(op->src.bo, op->src_offsets[u], FALSE);
		EL(VIVS_DE_UPLANE_STRIDE_STRIDE(op->src_pitches[u]));
		EL_RELOC(op->src.bo, op->src_offsets[v], FALSE);
		EL(VIVS_DE_VPLANE_STRIDE_STRIDE(op->src_pitches[v]));
		EL_ALIGN();
	}
	EL_END();

	etnaviv_set_dest_bo(etnaviv, &op->dst, op->cmd);

	EL_START(etnaviv, 10 * 8 * n);
	EL(LOADSTATE(VIVS_DE_ALPHA_CONTROL, 1));
	EL(VIVS_DE_ALPHA_CONTROL_ENABLE_OFF);

	EL(LOADSTATE(VIVS_DE_STRETCH_FACTOR_LOW, 2));
	EL(op->h_scale);
	EL(op->v_scale);
	EL_ALIGN();

	EL(LOADSTATE(VIVS_DE_VR_SOURCE_IMAGE_LOW, 2));
	EL(VIVS_DE_VR_SOURCE_IMAGE_LOW_LEFT(op->src_bounds.x1) |
	   VIVS_DE_VR_SOURCE_IMAGE_LOW_TOP(op->src_bounds.y1));
	EL(VIVS_DE_VR_SOURCE_IMAGE_HIGH_RIGHT(op->src_bounds.x2) |
	   VIVS_DE_VR_SOURCE_IMAGE_HIGH_BOTTOM(op->src_bounds.y2));
	EL_ALIGN();

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
		EL(LOADSTATE(VIVS_DE_VR_SOURCE_ORIGIN_LOW, 4));
		EL(VIVS_DE_VR_SOURCE_ORIGIN_LOW_X(x));
		EL(VIVS_DE_VR_SOURCE_ORIGIN_HIGH_Y(y));
		EL(VIVS_DE_VR_TARGET_WINDOW_LOW_LEFT(box.x1) |
		   VIVS_DE_VR_TARGET_WINDOW_LOW_TOP(box.y1));
		EL(VIVS_DE_VR_TARGET_WINDOW_HIGH_RIGHT(box.x2) |
		   VIVS_DE_VR_TARGET_WINDOW_HIGH_BOTTOM(box.y2));
		EL_ALIGN();

		/* 2 */
		EL(LOADSTATE(VIVS_DE_VR_CONFIG, 1));
		EL(op->vr_op);
	}
	EL_END();

	etnaviv_emit(etnaviv);
}

void etnaviv_flush(struct etnaviv *etnaviv)
{
	struct etna_ctx *ctx = etnaviv->ctx;
	etna_set_state(ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
	etna_set_state(ctx, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
}
