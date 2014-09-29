#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "fb.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"
#include "etnadrm.h"

void etnaviv_emit(struct etnaviv *etnaviv)
{
	struct etna_ctx *ctx = etnaviv->ctx;
	struct etnaviv_reloc *r;
	unsigned int i;

	etna_reserve(ctx, etnaviv->batch_size);
	memcpy(&ctx->buf[ctx->offset], etnaviv->batch, etnaviv->batch_size * 4);
	for (i = 0, r = etnaviv->reloc; i < etnaviv->reloc_size; i++, r++) {
		etna_emit_reloc(ctx, ctx->offset + r->batch_index,
			r->bo, etnaviv->batch[r->batch_index],
			r->write);
	}
	ctx->offset += etnaviv->batch_size;
}
