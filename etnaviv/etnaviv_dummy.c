/*
 * libetnaviv "compatibility" with additional etnaviv/drm APIs
 */
#include <etnaviv/viv.h>
#include <etnaviv/etna.h>
#include "etnaviv_compat.h"

int etna_bo_flink(struct etna_bo *bo, uint32_t *name)
{
	return -1;
}

struct etna_bo *etna_bo_from_name(struct viv_conn *conn, uint32_t name)
{
	return NULL;
}

int etna_bo_to_dmabuf(struct viv_conn *conn, struct etna_bo *bo)
{
	return -1;
}
