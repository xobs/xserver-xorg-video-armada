/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_H
#define VIVANTE_H

struct drm_armada_bufmgr;
struct drm_armada_bo;

/* Acceleration support */
Bool vivante_ScreenInit(ScreenPtr pScreen, struct drm_armada_bufmgr *bufmgr);
void vivante_free_pixmap(PixmapPtr pixmap);
void vivante_set_pixmap_bo(PixmapPtr pixmap, struct drm_armada_bo *bo);

#endif
