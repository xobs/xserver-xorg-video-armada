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
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include <armada_bufmgr.h>
#include "cpu_access.h"
#include "gal_extension.h"
#include "pamdump.h"
#include "pixmaputil.h"

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


static gceSTATUS vivante_ioctl(struct vivante *vivante, unsigned cmd,
	void *buf, size_t size)
{
	return gcoOS_DeviceControl(vivante->os, cmd, buf, size, buf, size);
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


Bool vivante_map_dmabuf(struct vivante *vivante, int fd,
	struct vivante_pixmap *vPix)
{
	struct dmabuf_map map;
	gceSTATUS status;

	map.hdr.v2.zero = 0;
	map.hdr.v2.status = 0;
	map.fd = fd;
	map.prot = PROT_READ | PROT_WRITE;

	status = vivante_ioctl(vivante, IOC_GDMABUF_MAP, &map, sizeof(map));
	if (gcmIS_ERROR(status)) {
		xf86DrvMsg(vivante->scrnIndex, X_INFO,
			   "vivante: gpu dmabuf map failed: %d\n",
			   status);
		return FALSE;
	}

	vPix->handle = map.address;
	vPix->info = (void *)(uintptr_t)map.info;

	return TRUE;
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

	map.hdr.v2.zero = 0;
	map.hdr.v2.status = 0;
	map.fd = fd;
	map.prot = PROT_READ | PROT_WRITE;

	status = vivante_ioctl(vivante, IOC_GDMABUF_MAP, &map, sizeof(map));

	/* we don't need to keep the fd around anymore */
	close(fd);

	if (gcmIS_ERROR(status)) {
		xf86DrvMsg(vivante->scrnIndex, X_INFO,
			   "vivante: gpu dmabuf map failed: %d\n",
			   status);
		return FALSE;
	}

	*handle = map.address;
	*info = (void *)(uintptr_t)map.info;

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

	if (vPix->owner == GPU)
		return TRUE;

	if (bo) {
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
void finish_cpu_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = drawable_pixmap(pDrawable);
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);

	if (vPix) {
#ifdef DEBUG_CHECK_DRAWABLE_USE
		vPix->in_use--;
#endif
		if (vPix->bo)
			pixmap->devPrivate.ptr = NULL;
	}
}

/*
 * Prepare a bo for CPU access.  If the GPU has been accessing the
 * pixmap data, we need to unmap the buffer from the GPU to ensure
 * that our view is up to date.
 */
void prepare_cpu_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = drawable_pixmap(pDrawable);
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pixmap);

	if (vPix) {
		struct vivante *vivante = vivante_get_screen_priv(pDrawable->pScreen);

		/* Ensure that the drawable is up to date with all GPU operations */
		vivante_batch_wait_commit(vivante, vPix);

		if (vPix->bo) {
			if (vPix->owner == GPU)
				vivante_unmap_gpu(vivante, vPix);

			pixmap->devPrivate.ptr = vPix->bo->ptr;
		}
#ifdef DEBUG_CHECK_DRAWABLE_USE
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

Bool vivante_format_valid(struct vivante *vivante, gceSURF_FORMAT fmt)
{
	switch (fmt) {
	case gcvSURF_A8R8G8B8:
	case gcvSURF_X8R8G8B8:
	case gcvSURF_R5G6B5:
	case gcvSURF_A1R5G5B5:
	case gcvSURF_X1R5G5B5:
	case gcvSURF_A4R4G4B4:
	case gcvSURF_X4R4G4B4:
		return TRUE;
	case gcvSURF_A8B8G8R8:
	case gcvSURF_X8B8G8R8:
	case gcvSURF_B8G8R8A8:
	case gcvSURF_B8G8R8X8:
	case gcvSURF_B5G6R5:
	case gcvSURF_A1B5G5R5:
	case gcvSURF_X1B5G5R5:
	case gcvSURF_A4B4G4R4:
	case gcvSURF_X4B4G4R4:
	case gcvSURF_A8:
		return vivante->pe20;
	default:
		return FALSE;
	}
}

#if 1 //def DEBUG
static void dump_pix(struct vivante *vivante, struct vivante_pixmap *vPix,
	bool alpha, int x1, int y1, int x2, int y2,
	const char *fmt, va_list ap)
	__attribute__ ((__format__ (__printf__, 8, 0)));

static void dump_pix(struct vivante *vivante, struct vivante_pixmap *vPix,
	bool alpha, int x1, int y1, int x2, int y2,
	const char *fmt, va_list ap)
{
	struct drm_armada_bo *bo = vPix->bo;
	static int idx;
	unsigned owner = vPix->owner;
	char n[80];

	vivante_commit(vivante, TRUE);

	if (!bo)
		owner = CPU;

	if (owner == GPU) {
		vivante_unmap_gpu(vivante, vPix);
		vPix->owner = CPU;
	}

	vsprintf(n, fmt, ap);

	dump_pam(bo->ptr, vPix->pitch, alpha, x1, y1, x2, y2,
		 "/tmp/X.%04u.%s-%u.%u.%u.%u.pam",
		 idx++, n, x1, y1, x2, y2);

	if (owner == GPU)
		vivante_map_gpu(vivante, vPix);
}

void dump_Drawable(DrawablePtr pDraw, const char *fmt, ...)
{
	struct vivante *vivante = vivante_get_screen_priv(pDraw->pScreen);
	int x, y;
	PixmapPtr pPix = drawable_pixmap_deltas(pDraw, &x, &y);
	struct vivante_pixmap *vPix = vivante_get_pixmap_priv(pPix);
	va_list ap;

	if (!vPix)
		return;

	va_start(ap, fmt);
	dump_pix(vivante, vPix, false,
		 pDraw->x + x, pDraw->y + y, pDraw->width, pDraw->height,
		 fmt, ap);
	va_end(ap);
}

void dump_Picture(PicturePtr pDst, const char *fmt, ...)
{
	DrawablePtr pDraw = pDst->pDrawable;
	struct vivante *vivante;
	struct vivante_pixmap *vPix;
	PixmapPtr pPix;
	bool alpha;
	va_list ap;
	int x, y;

	vivante = vivante_get_screen_priv(pDraw->pScreen);
	pPix = drawable_pixmap_deltas(pDraw, &x, &y);
	vPix = vivante_get_pixmap_priv(pPix);
	if (!vPix)
		return;

	alpha = PICT_FORMAT_A(pDst->format) != 0;

	va_start(ap, fmt);
	dump_pix(vivante, vPix, alpha,
		 pDraw->x + x, pDraw->y + y, pDraw->width, pDraw->height,
		 fmt, ap);
	va_end(ap);
}

void dump_vPix(struct vivante *vivante, struct vivante_pixmap *vPix,
	      int alpha, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dump_pix(vivante, vPix, !!alpha,
		 0, 0, vPix->width, vPix->height,
		 fmt, ap);
	va_end(ap);
}
#endif
