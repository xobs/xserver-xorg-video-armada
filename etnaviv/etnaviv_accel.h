/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_ACCEL_H
#define VIVANTE_ACCEL_H

#include <etnaviv/viv.h>
#include <etnaviv/etna.h>
#include <etnaviv/etna_bo.h>
#include "compat-list.h"
#include "pixmaputil.h"
#include "etnaviv_compat.h"
#include "etnaviv_op.h"

struct armada_accel_ops;
struct drm_armada_bo;
struct drm_armada_bufmgr;
struct etnaviv_dri2_info;

#undef DEBUG

/* Debugging options */
#define DEBUG_CHECK_DRAWABLE_USE
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

/* Private CreatePixmap usage hints. */
enum {
	CREATE_PIXMAP_USAGE_TILE = 0x80000000,
	CREATE_PIXMAP_USAGE_GPU = 0x40000000,	/* Must be vpix backed */
	CREATE_PIXMAP_USAGE_3D = 0x20000000,	/* 3D has restrictions */
};

/* Workarounds for hardware bugs */
enum {
	BUGFIX_SINGLE_BITBLT_DRAW_OP,
};

/*
 * The maximum size of an operation in the batch buffer.  A 2D draw
 * operation can contain up to 255 rectangles, which equates to 512
 * words (including the operation word.)  Add to this the states to
 * be loaded before, and 1024 is a conservative overestimation.
 */
#define MAX_BATCH_SIZE	1024
#define MAX_RELOC_SIZE	8

struct etnaviv {
	struct viv_conn *conn;
	struct etna_ctx *ctx;
	/* pixmaps queued for next commit */
	struct xorg_list batch_head;
	/* pixmaps committed with fence id, ordered by id */
	struct xorg_list fence_head;
	struct xorg_list busy_free_list;
	struct xorg_list usermem_free_list;
	OsTimerPtr cache_timer;
	uint32_t last_fence;
	Bool force_fallback;
	struct drm_armada_bufmgr *bufmgr;
	uint32_t bugs[1];
	struct etnaviv_blit_buf gc320_wa_src;
	struct etnaviv_blit_buf gc320_wa_dst;
	struct etna_bo *gc320_etna_bo;
	int scrnIndex;
#ifdef HAVE_DRI2
	Bool dri2_enabled;
	Bool dri2_armada;
	struct etnaviv_dri2_info *dri2;
#endif
#ifdef HAVE_DRI3
	Bool dri3_enabled;
	const char *render_node;
#endif

	uint32_t batch[MAX_BATCH_SIZE];
	unsigned int batch_setup_size;
	unsigned int batch_size;
	unsigned int batch_de_high_watermark;
	struct etnaviv_reloc {
		struct etna_bo *bo;
		unsigned int batch_index;
		Bool write;
	} reloc[MAX_RELOC_SIZE];
	unsigned int reloc_setup_size;
	unsigned int reloc_size;

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
	CreateScreenResourcesProcPtr CreateScreenResources;

	CompositeProcPtr Composite;
	GlyphsProcPtr Glyphs;
	TrapezoidsProcPtr Trapezoids;
	TrianglesProcPtr Triangles;
	AddTrianglesProcPtr AddTriangles;
	AddTrapsProcPtr AddTraps;
	UnrealizeGlyphProcPtr UnrealizeGlyph;

	struct etnaviv_xv_priv *xv;
	unsigned xv_ports;
	CloseScreenProcPtr xv_CloseScreen;
};

struct etnaviv_pixmap {
	uint16_t width;
	uint16_t height;
	unsigned pitch;
	struct etnaviv_format format;
	struct etnaviv_format pict_format;
	struct xorg_list batch_node;
	struct xorg_list busy_node;
	uint32_t fence;
	viv_usermem_t info;

	uint8_t batch_state;
#define B_NONE		0
#define B_PENDING	1
#define B_FENCED	2

	uint8_t state;
#define ST_CPU_R	(1 << 0)
#define ST_CPU_W	(1 << 1)
#define ST_CPU_RW	(3 << 0)
#define ST_GPU_R	(1 << 2)
#define ST_GPU_W	(1 << 3)
#define ST_GPU_RW	(3 << 2)
#define ST_DMABUF	(1 << 4)

#ifdef DEBUG_CHECK_DRAWABLE_USE
	int in_use;
#endif
	struct drm_armada_bo *bo;
	struct etna_bo *etna_bo;
	uint32_t name;
};

struct etnaviv_usermem_node {
	struct xorg_list node;
	struct etnaviv_pixmap *dst;
	struct etna_bo *bo;
	void *mem;
};

void etnaviv_add_freemem(struct etnaviv *etnaviv,
	struct etnaviv_usermem_node *n);

static inline void etnaviv_enable_bugfix(struct etnaviv *etnaviv,
	unsigned int bug)
{
	unsigned int index = bug >> 5;
	uint32_t mask = 1 << (bug & 31);
	etnaviv->bugs[index] |= mask;
}

static inline bool etnaviv_has_bugfix(struct etnaviv *etnaviv,
	unsigned int bug)
{
	unsigned int index = bug >> 5;
	uint32_t mask = 1 << (bug & 31);
	return !!(etnaviv->bugs[index] & mask);
}

/* Addresses must be aligned */
#define VIVANTE_ALIGN_MASK	63

/* 2D acceleration */
Bool etnaviv_accel_FillSpans(DrawablePtr pDrawable, GCPtr pGC, int n,
	DDXPointPtr ppt, int *pwidth, int fSorted);
Bool etnaviv_accel_GetImage(DrawablePtr pDrawable, int x, int y, int w, int h,
	unsigned int format, unsigned long planeMask, char *d);
Bool etnaviv_accel_PutImage(DrawablePtr pDrawable, GCPtr pGC, int depth,
	int x, int y, int w, int h, int leftPad, int format, char *bits);
void etnaviv_accel_CopyNtoN(DrawablePtr pSrc, DrawablePtr pDst,
	GCPtr pGC, BoxPtr pBox, int nBox, int dx, int dy, Bool reverse,
	Bool upsidedown, Pixel bitPlane, void *closure);
Bool etnaviv_accel_PolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt);
Bool etnaviv_accel_PolyLines(DrawablePtr pDrawable, GCPtr pGC, int mode,
	int npt, DDXPointPtr ppt);
Bool etnaviv_accel_PolySegment(DrawablePtr pDrawable, GCPtr pGC, int nseg,
	xSegment *pSeg);
Bool etnaviv_accel_PolyFillRectSolid(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect);
Bool etnaviv_accel_PolyFillRectTiled(DrawablePtr pDrawable, GCPtr pGC, int n,
	xRectangle * prect);

void etnaviv_commit(struct etnaviv *etnaviv, Bool stall, uint32_t *fence);
void etnaviv_finish_fences(struct etnaviv *etnaviv, uint32_t fence);
void etnaviv_free_busy_vpix(struct etnaviv *etnaviv);

void etnaviv_batch_wait_commit(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix);
void etnaviv_batch_start(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op);

void etnaviv_accel_shutdown(struct etnaviv *);
Bool etnaviv_accel_init(struct etnaviv *);

static inline struct etnaviv_pixmap *etnaviv_get_pixmap_priv(PixmapPtr pixmap)
{
	extern etnaviv_Key etnaviv_pixmap_index;
	return etnaviv_GetKeyPriv(&pixmap->devPrivates, &etnaviv_pixmap_index);
}

static inline struct etnaviv_pixmap *etnaviv_drawable_offset(
	DrawablePtr pDrawable, xPoint *offset)
{
	PixmapPtr pix = drawable_pixmap_offset(pDrawable, offset);
	return etnaviv_get_pixmap_priv(pix);
}

static inline struct etnaviv_pixmap *etnaviv_drawable(DrawablePtr pDrawable)
{
	PixmapPtr pix = drawable_pixmap(pDrawable);
	return etnaviv_get_pixmap_priv(pix);
}

static inline struct etnaviv *etnaviv_get_screen_priv(ScreenPtr pScreen)
{
	extern etnaviv_Key etnaviv_screen_index;
	return etnaviv_GetKeyPriv(&pScreen->devPrivates, &etnaviv_screen_index);
}

static inline void etnaviv_set_pixmap_priv(PixmapPtr pixmap, struct etnaviv_pixmap *g)
{
	extern etnaviv_Key etnaviv_pixmap_index;
	dixSetPrivate(&pixmap->devPrivates, &etnaviv_pixmap_index, g);
}

static inline void etnaviv_set_screen_priv(ScreenPtr pScreen, struct etnaviv *g)
{
	extern etnaviv_Key etnaviv_screen_index;
	dixSetPrivate(&pScreen->devPrivates, &etnaviv_screen_index, g);
}

PixmapPtr etnaviv_pixmap_from_dmabuf(ScreenPtr pScreen, int fd,
	CARD16 width, CARD16 height, CARD16 stride, CARD8 depth, CARD8 bpp);

Bool etnaviv_pixmap_flink(PixmapPtr pixmap, uint32_t *name);

extern const struct armada_accel_ops etnaviv_ops;

#endif
