/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_COMPAT_H
#define VIVANTE_COMPAT_H

#include "utils.h"

#if HAS_DEVPRIVATEKEYREC
#define etnaviv_CreateKey(key, type) dixRegisterPrivateKey(key, type, 0)
#define etnaviv_GetKeyPriv(dp, key)  dixGetPrivate(dp, key)
#define etnaviv_Key                  DevPrivateKeyRec
#else
#define etnaviv_CreateKey(key, type) dixRequestPrivate(key, 0)
#define etnaviv_GetKeyPriv(dp, key)  dixLookupPrivate(dp, key)
#define etnaviv_Key                  int
#endif

/*
 * Etnaviv itself does not provide these functions.  We'd like these
 * to be named this way, but some incompatible etnaviv functions clash.
 */
#define etna_bo_from_name my_etna_bo_from_name
struct etna_bo *etna_bo_from_name(struct viv_conn *conn, uint32_t name);
#define etna_bo_to_dmabuf my_etna_bo_to_dmabuf
int etna_bo_to_dmabuf(struct viv_conn *conn, struct etna_bo *bo);

#endif
