/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_ACCEL_H
#define VIVANTE_ACCEL_H

#include <gc_hal_base.h>
#include "vivante_compat.h"

struct drm_armada_bo;
struct drm_armada_bufmgr;
struct vivante_dri2_info;
struct armada_drm_info;

#undef DEBUG

/* Debugging options */
#define DEBUG_CHECK_DRAWABLE_USE
#undef DEBUG_BATCH
#undef DEBUG_MAP
#undef DEBUG_PIXMAP

/* Accelerated operations debugging */
#undef DEBUG_COPYNTON
#undef DEBUG_FILLSPANS
#undef DEBUG_POLYFILLRECT
#undef DEBUG_PUTIMAGE


/* Debugging */
#define OP_NOP 0
#define OP_USER_INV 1
#define OP_USER_CLN 2
#define OP_USER_FLS 3
#define OP_KERN_INV 5
#define OP_KERN_CLN 6
#define OP_KERN_FLS 7

#define dbg(fmt...) fprintf(stderr, fmt)

struct vivante {
	int drm_fd;
	gcoOS os;
	gcoHAL hal;
	gco2D e2d;
	unsigned max_rect_count;
#ifdef VIVANTE_BATCH
	struct drm_armada_bo *batch_bo;
	int32_t *batch_ptr;
	void *batch_info;
	uint32_t batch_handle;
	uint16_t batch_idx_max;
	uint16_t batch_idx;
	int32_t batch_serial;
	struct xorg_list batch_list;
	struct vivante_batch *batch;
#else
	Bool need_stall;
#endif

	Bool need_commit;
	Bool force_fallback;
#ifdef RENDER
	Bool alpha_blend_enabled;
#endif
	struct drm_armada_bufmgr *bufmgr;
	int scrnIndex;
#ifdef HAVE_DRI2
	struct vivante_dri2_info *dri2;
#endif

	CloseScreenProcPtr CloseScreen;
	GetImageProcPtr GetImage;
	GetSpansProcPtr GetSpans;
	ChangeWindowAttributesProcPtr ChangeWindowAttributes;
	CopyWindowProcPtr CopyWindow;
	CreatePixmapProcPtr CreatePixmap;
	DestroyPixmapProcPtr DestroyPixmap;
	CreateGCProcPtr CreateGC;
	BitmapToRegionProcPtr BitmapToRegion;
	ScreenBlockHandlerProcPtr BlockHandler;

	CompositeProcPtr Composite;
	GlyphsProcPtr Glyphs;
	TrapezoidsProcPtr Trapezoids;
	TrianglesProcPtr Triangles;
	AddTrianglesProcPtr AddTriangles;
	AddTrapsProcPtr AddTraps;
	UnrealizeGlyphProcPtr UnrealizeGlyph;
};

struct vivante_pixmap {
	uint16_t width;
	uint16_t height;
	uint32_t handle;
	unsigned pitch;
	gceSURF_FORMAT format;
	gceSURF_FORMAT pict_format;
	gctPOINTER info;

#ifdef VIVANTE_BATCH
	struct xorg_list batch_node;
	struct vivante_batch *batch;
#else
	Bool need_stall;
#endif

	enum {
		NONE,
		CPU,
		GPU,
	} owner;
#ifdef DEBUG_CHECK_DRAWABLE_USE
	int in_use;
#endif
	struct drm_armada_bo *bo;
};

/* Addresses must be aligned */
#define VIVANTE_ALIGN_MASK 63

/* 2D acceleration */
Bool vivante_accel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n,
	DDXPointPtr ppt, int *pwidth, int fSorted);
Bool vivante_accel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits);
void vivante_accel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, BoxPtr pBox, int nBox, int dx, int dy, Bool reverse,
	Bool upsidedown, Pixel bitPlane, void *closure);
Bool vivante_accel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt);
Bool vivante_accel_PolyFillRectSolid(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect);
Bool vivante_accel_PolyFillRectTiled(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect);

/* 3D acceleration */
int vivante_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height);

void vivante_commit(struct vivante *vivante, Bool stall);

void vivante_batch_wait_commit(struct vivante *vivante, struct vivante_pixmap *vPix);

void vivante_accel_shutdown(struct vivante *);
Bool vivante_accel_init(struct vivante *);

static inline struct vivante_pixmap *vivante_get_pixmap_priv(PixmapPtr pixmap)
{
	extern vivante_Key vivante_pixmap_index;
	return vivante_GetKeyPriv(&pixmap->devPrivates, &vivante_pixmap_index);
}

static inline struct vivante *vivante_get_screen_priv(ScreenPtr pScreen)
{
	extern vivante_Key vivante_screen_index;
	return vivante_GetKeyPriv(&pScreen->devPrivates, &vivante_screen_index);
}

static inline void vivante_set_pixmap_priv(PixmapPtr pixmap, struct vivante_pixmap *g)
{
	extern vivante_Key vivante_pixmap_index;
	dixSetPrivate(&pixmap->devPrivates, &vivante_pixmap_index, g);
}

static inline void vivante_set_screen_priv(ScreenPtr pScreen, struct vivante *g)
{
	extern vivante_Key vivante_screen_index;
	dixSetPrivate(&pScreen->devPrivates, &vivante_screen_index, g);
}

#endif
