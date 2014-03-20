/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include <armada_bufmgr.h>
#include "gal_extension.h"

#include "vivante_accel.h"
#include "vivante_utils.h"

static const char *vivante_errors[] = {
	"invalid argument",
	"invalid object",
	"out of memory",
	"memory locked",
	"memory unlocked",
	"heap corrupted",
	"generic IO",
	"invalid address",
	"context loss",
	"too complex",
	"buffer too small",
	"interface error",
	"not supported",
	"more data",
	"timeout",
	"out of resources",
	"invalid data",
	"invalid mipmap",
	"not found",
	"not aligned",
	"invalid request",
	"GPU unresponsive",
};

const char *vivante_strerror(int err)
{
	const char *str = NULL;

	if (err < 0) {
		if (err >= gcvSTATUS_GPU_NOT_RESPONDING)
			str = vivante_errors[-1 - err];
	}
	return str;
}

void __vivante_error(struct vivante *vivante, const char *fn, const char *w, int err)
{
	xf86DrvMsg(vivante->scrnIndex, X_ERROR,
		   "[vivante] %s: %s failed: %s\n", fn, w,
		   vivante_strerror(err));
}


PixmapPtr vivante_drawable_pixmap_deltas(DrawablePtr pDrawable, int *x, int *y)
{
	PixmapPtr pPixmap;

	*x = *y = 0;
	if (OnScreenDrawable(pDrawable->type)) {
		WindowPtr pWin = container_of(pDrawable, struct _Window, drawable);

		pPixmap = pDrawable->pScreen->GetWindowPixmap(pWin);
#ifdef COMPOSITE
		*x = -pPixmap->screen_x;
		*y = -pPixmap->screen_y;
#endif
	} else {
		pPixmap = container_of(pDrawable, struct _Pixmap, drawable);
	}
	return pPixmap;
}


/*
 * Unmap a pixmap from the GPU.  Note that we must wait for any outstanding
 * GPU operations to complete before unmapping the pixmap from the GPU.
 */
void vivante_unmap_gpu(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	struct drm_armada_bo *bo = vPix->bo;
	gceSTATUS err;

#ifdef DEBUG_MAP
	dbg("Unmapping vPix %p bo %p\n", vPix, bo);
#endif
	err = gcoOS_UnmapUserMemory(vivante->os, bo->ptr, bo->size,
				    vPix->info, vPix->handle);
	if (err != gcvSTATUS_OK)
		vivante_error(vivante, "gcoOS_UnmapUserMemory", err);

	vPix->handle = -1;
	vPix->info = NULL;
}


Bool vivante_map_bo_to_gpu(struct vivante *vivante, struct drm_armada_bo *bo,
	void **info, uint32_t *handle)
{
	struct dmabuf_map map;
	gceSTATUS status;
	int fd;

	if (drm_armada_bo_to_fd(bo, &fd)) {
		xf86DrvMsg(vivante->scrnIndex, X_ERROR,
			   "vivante: unable to get prime fd for bo: %s\n",
			   strerror(errno));
		return FALSE;
	}

	map.zero = 0;
	map.fd = fd;

	status = gcoOS_DeviceControl(vivante->os, IOC_GDMABUF_MAP,
				     &map, sizeof(map), &map, sizeof(map));

	/* we don't need to keep the fd around anymore */
	close(fd);

	if (gcmIS_ERROR(status)) {
		xf86DrvMsg(vivante->scrnIndex, X_INFO,
			   "vivante: gpu dmabuf map failed: %d\n",
			   status);
		return FALSE;
	}

	*handle = map.Address;
	*info = map.Info;

	return TRUE;
}

void vivante_unmap_from_gpu(struct vivante *vivante, void *info,
	uint32_t handle)
{
	gcoOS_UnmapUserMemory(vivante->os, (void *)1, 1, info, handle);
}

/*
 * Map a pixmap to the GPU, and mark the GPU as owning this BO.
 */
Bool vivante_map_gpu(struct vivante *vivante, struct vivante_pixmap *vPix)
{
	struct drm_armada_bo *bo = vPix->bo;

#ifdef DEBUG_CHECK_DRAWABLE_USE
	assert(vPix->in_use == 0);
#endif

	if (bo->type == DRM_ARMADA_BO_SHMEM) {
		gceSTATUS err;
		gctUINT32 addr;

		err = gcoOS_MapUserMemory(vivante->os, bo->ptr, bo->size,
					  &vPix->info, &addr);
		if (err != gcvSTATUS_OK) {
			vivante_error(vivante, "gcoOS_MapUserMemory", err);
			return FALSE;
		}

#ifdef DEBUG_MAP
		dbg("Mapped vPix %p bo %p to 0x%08x\n", vPix, bo, addr);
#endif

		vPix->handle = addr;
	}
	vPix->owner = GPU;

	return TRUE;
}

/*
 * Finish a bo for CPU access.  NULL out the fb layer's pixmap data
 * pointer to ensure any further unprotected accesses get caught.
 */
void vivante_finish_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = vivante_drawable_pixmap(pDrawable);
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);

	if (vPix) {
#ifdef DEBUG_CHECK_DRAWABLE_USE
		vPix->in_use--;
#endif
		if (vPix->bo->type == DRM_ARMADA_BO_SHMEM)
			pixmap->devPrivate.ptr = NULL;
	}
}

/*
 * Prepare a bo for CPU access.  If the GPU has been accessing the
 * pixmap data, we need to unmap the buffer from the GPU to ensure
 * that our view is up to date.
 */
void vivante_prepare_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = vivante_drawable_pixmap(pDrawable);
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);

	if (vPix) {
		struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

		/* Ensure that the drawable is up to date with all GPU operations */
		vivante_batch_wait_commit(vivante, vPix);

		if (vPix->bo->type == DRM_ARMADA_BO_SHMEM) {
			if (vPix->owner == GPU)
				vivante_unmap_gpu(vivante, vPix);

			pixmap->devPrivate.ptr = vPix->bo->ptr;
		}
#ifdef DEBUG_CHECK_DRAWABLE_USE
		assert(vPix->in_use == 0);
		vPix->in_use++;
#endif
		vPix->owner = CPU;
	}
}

#ifdef RENDER
gceSURF_FORMAT vivante_pict_format(PictFormatShort format, Bool force)
{
	switch (format) {
#define C(pf,vf,af) case PICT_##pf: return force ? gcvSURF_##af : gcvSURF_##vf
	C(a2r10g10b10,	A2R10G10B10,	A2R10G10B10);
	C(x2r10g10b10,	X2R10G10B10,	A2R10G10B10);
	C(a2b10g10r10,	A2B10G10R10,	A2B10G10R10);
	C(x2b10g10r10,	UNKNOWN,	A2B10G10R10);
	C(a8r8g8b8, 	A8R8G8B8,	A8R8G8B8);
	C(x8r8g8b8, 	X8R8G8B8,	A8R8G8B8);
	C(a8b8g8r8, 	A8B8G8R8,	A8B8G8R8);
	C(x8b8g8r8, 	X8B8G8R8,	A8B8G8R8);
	C(b8g8r8a8, 	B8G8R8A8,	B8G8R8A8);
	C(b8g8r8x8, 	B8G8R8X8,	B8G8R8A8);
	C(r8g8b8,	R8G8B8,		UNKNOWN);
	C(b8g8r8,	B8G8R8,		UNKNOWN);
	C(r5g6b5,	R5G6B5,		UNKNOWN);
	C(b5g6r5,	B5G6R5,		UNKNOWN);
	C(a1r5g5b5, 	A1R5G5B5,	A1R5G5B5);
	C(x1r5g5b5, 	X1R5G5B5,	A1R5G5B5);
	C(a1b5g5r5, 	A1B5G5R5,	A1B5G5R5);
	C(x1b5g5r5, 	X1B5G5R5,	A1B5G5R5);
	C(a4r4g4b4, 	A4R4G4B4,	A4R4G4B4);
	C(x4r4g4b4, 	X4R4G4B4,	A4R4G4B4);
	C(a4b4g4r4, 	A4B4G4R4,	A4B4G4R4);
	C(x4b4g4r4, 	X4B4G4R4,	A4B4G4R4);
	C(a8,		A8,		A8);
	C(r3g3b2,	R3G3B2,		UNKNOWN);
	C(b2g3r3,	UNKNOWN,	UNKNOWN);
	C(a2r2g2b2, 	A2R2G2B2,	A2R2G2B2);
	C(a2b2g2r2, 	UNKNOWN,	A2R2G2B2);
	C(c8,		INDEX8,		UNKNOWN);
	C(g8,		L8,		UNKNOWN);
	C(x4a4,		UNKNOWN,	UNKNOWN);
//	C(x4c4,		UNKNOWN,	UNKNOWN); /* same value as c8 */
//	C(x4g4,		UNKNOWN,	UNKNOWN); /* same value as g8 */
	C(a4,		A4,		A4);
	C(r1g2b1,	UNKNOWN,	UNKNOWN);
	C(b1g2r1,	UNKNOWN,	UNKNOWN);
	C(a1r1g1b1, 	UNKNOWN,	UNKNOWN);
	C(a1b1g1r1, 	UNKNOWN,	UNKNOWN);
	C(c4,		INDEX4,		UNKNOWN);
	C(g4,		L4,		UNKNOWN);
	C(a1,		A1,		A1);
	C(g1,		L1,		UNKNOWN);
#undef C
	}
	return gcvSURF_UNKNOWN;
}
#endif


#if 1 //def DEBUG
#include <stdarg.h>
#include <sys/fcntl.h>
#include <unistd.h>
static void dump_pix(struct vivante *vivante, struct vivante_pixmap *vPix,
	int x1, int y1, int x2, int y2, int alpha,
	const char *fmt, va_list ap)
{
	struct drm_armada_bo *bo = vPix->bo;
	static int idx;
	char fn[160], n[80];
	int fd;

	if (vPix->bo->type == DRM_ARMADA_BO_SHMEM && vPix->owner == GPU)
		vivante_unmap_gpu(vivante, vPix);

	vsprintf(n, fmt, ap);
	sprintf(fn, "/tmp/X.%04u.%s-%u.%u.%u.%u.pam",
			idx++, n, x1, y1, x2, y2);
	fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0) {
		char buf[16*1024 + 16384];
		uint32_t *bo_p;
		int x, y, i;

		sprintf(buf, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %u\nMAXVAL 255\nTUPLTYPE RGB%s\nENDHDR\n",
				x2 - x1, y2 - y1, 3 + alpha, alpha ? "_ALPHA" : "");
		write(fd, buf, strlen(buf));

		for (y = y1, i = 0; y < y2; y++) {
			bo_p = (((void *)bo->ptr) + (y * vPix->pitch));
			for (x = x1; x < x2; x++) {
				buf[i++] = bo_p[x] >> 16; // R
				buf[i++] = bo_p[x] >> 8;  // G
				buf[i++] = bo_p[x];       // B
				if (alpha)
					buf[i++] = bo_p[x] >> 24; // A
			}
			if (i >= 16*1024) {
				write(fd, buf, i);
				i = 0;
			}
		}
		if (i)
			write(fd, buf, i);
		close(fd);
	}

	if (vPix->owner == GPU && vPix->bo->type == DRM_ARMADA_BO_SHMEM)
		vivante_map_gpu(vivante, vPix);
}

void dump_Drawable(DrawablePtr pDraw, const char *fmt, ...)
{
	struct vivante *vivante = vivante_get_screen_priv(pDraw->pScreen);
	struct vivante_pixmap *vPix;
	PixmapPtr pPix;
	int off_x, off_y;
	va_list ap;

	pPix = vivante_drawable_pixmap_deltas(pDraw, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);

	if (!vPix)
		return;

	va_start(ap, fmt);
	dump_pix(vivante, vPix, 0, 0, pDraw->width, pDraw->height, 0, fmt, ap);
	va_end(ap);
}

void dump_Picture(PicturePtr pDst, const char *fmt, ...)
{
	struct vivante *vivante = vivante_get_screen_priv(pDst->pDrawable->pScreen);
	struct vivante_pixmap *vPix;
	PixmapPtr pPix;
	int alpha, off_x, off_y;
	va_list ap;

	pPix = vivante_drawable_pixmap_deltas(pDst->pDrawable, &off_x, &off_y);
	vPix = vivante_get_pixmap_priv(pPix);

	if (!vPix)
		return;

	alpha = PICT_FORMAT_A(pDst->format) != 0;

	va_start(ap, fmt);
	dump_pix(vivante, vPix, 0, 0, vPix->width, vPix->height, alpha, fmt, ap);
	va_end(ap);
}

void dump_vPix(struct vivante *vivante, struct vivante_pixmap *vPix,
	      int alpha, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dump_pix(vivante, vPix, 0, 0, vPix->width, vPix->height, alpha, fmt, ap);
	va_end(ap);
}
#endif
