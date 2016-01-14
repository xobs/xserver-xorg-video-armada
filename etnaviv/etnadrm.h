#ifndef ETNADRM_H
#define ETNADRM_H

struct etna_bo;
struct etna_ctx;

void etna_emit_reloc(struct etna_ctx *ctx, uint32_t buf_offset,
	struct etna_bo *mem, uint32_t offset, Bool write);
int etnadrm_open_render(const char *name);

int etna_bo_flink(struct etna_bo *bo, uint32_t *name);
int etna_bo_to_dmabuf(struct viv_conn *conn, struct etna_bo *mem);
struct etna_bo *etna_bo_from_name(struct viv_conn *conn, uint32_t name);

#endif
