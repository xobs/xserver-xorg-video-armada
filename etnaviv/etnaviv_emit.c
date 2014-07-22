#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "fb.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"

void etnaviv_emit(struct etnaviv *etnaviv)
{
	struct etna_ctx *ctx = etnaviv->ctx;
	struct etnaviv_reloc *r;
	unsigned int i;

	for (i = 0, r = etnaviv->reloc; i < etnaviv->reloc_size; i++, r++)
		etnaviv->batch[r->batch_index] += etna_bo_gpu_address(r->bo);

	etna_reserve(ctx, etnaviv->batch_size);
	memcpy(&ctx->buf[ctx->offset], etnaviv->batch, etnaviv->batch_size * 4);
	ctx->offset += etnaviv->batch_size;
}
