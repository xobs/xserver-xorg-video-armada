/*
 * Marvell Armada DRM-based Xvideo driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <armada_bufmgr.h>

#include "armada_drm.h"
#include "drm_fourcc.h"
#include "xf86Crtc.h"
#include "xf86xv.h"
#include "fourcc.h"
#include <X11/extensions/Xv.h>
#include <X11/Xatom.h>

#include "armada_ioctl.h"

#define FOURCC_VYUY 0x59555956
#define GUID4CC(a,b,c,d) { a,b,c,d, 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }
#define XVIMAGE_VYUY { \
		FOURCC_VYUY, XvYUV, LSBFirst, GUID4CC('V', 'Y', 'U', 'Y'), \
		16, XvPacked, 1,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1, "VYUY", XvTopToBottom, }

#define FOURCC_I422 0x32323449
#define XVIMAGE_I422 { \
		FOURCC_I422, XvYUV, LSBFirst, GUID4CC('I', '4', '2', '2'), \
		16, XvPlanar, 3,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "YUV", XvTopToBottom, }

#define FOURCC_YV16 0x36315659
#define XVIMAGE_YV16 { \
		FOURCC_YV16, XvYUV, LSBFirst, GUID4CC('Y', 'V', '1', '6'), \
		16, XvPlanar, 3,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "YVU", XvTopToBottom, }

#define FOURCC_XVBO 0x4f425658
#define XVIMAGE_XVBO { \
		FOURCC_XVBO, XvYUV, LSBFirst, { 0 }, \
		16, XvPacked, 1,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "UYVY", XvTopToBottom, }

#define XVIMAGE_ARGB8888 { \
		DRM_FORMAT_ARGB8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGRA", XvTopToBottom, }

#define XVIMAGE_ABGR8888 { \
		DRM_FORMAT_ABGR8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGBA", XvTopToBottom, }

#define XVIMAGE_XRGB8888 { \
		DRM_FORMAT_XRGB8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_XBGR8888 { \
		DRM_FORMAT_XBGR8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define XVIMAGE_RGB888 { \
		DRM_FORMAT_RGB888, XvRGB, LSBFirst, { 0 }, \
		24, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_BGR888 { \
		DRM_FORMAT_BGR888, XvRGB, LSBFirst, { 0 }, \
		24, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define XVIMAGE_ARGB1555 { \
		DRM_FORMAT_ARGB1555, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  15, 0x7c00, 0x03e0, 0x001f, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGRA", XvTopToBottom, }

#define XVIMAGE_ABGR1555 { \
		DRM_FORMAT_ABGR1555, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  15, 0x001f, 0x03e0, 0x7c00, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGBA", XvTopToBottom, }

#define XVIMAGE_RGB565 { \
		DRM_FORMAT_RGB565, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  16, 0xf800, 0x07e0, 0x001f, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_BGR565 { \
		DRM_FORMAT_BGR565, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  16, 0x001f, 0x07e0, 0xf800, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define MAKE_ATOM(a) MakeAtom(a, sizeof(a) - 1, TRUE)

static Atom xvAutoPaintColorKey;
static Atom xvColorKey;
static Atom xvBrightness;
static Atom xvContrast;
static Atom xvSaturation;
static Atom xvDeinterlace;
static Atom xvPipe;

/* Size of physical addresses via BMM */
typedef uint32_t phys_t;
#define INVALID_PHYS	(~(phys_t)0)

#define NR_BUFS	3

struct armada_format {
	uint32_t	drm_format;
	XF86ImageRec	xv_image;
	uint32_t	flags;
};

struct drm_xv {
	struct armada_drm_info *drm;

	/* Common information */
	xf86CrtcPtr desired_crtc;
	Bool is_bmm;
	Bool autopaint_colorkey;

	/* Cached image information */
	RegionRec clipBoxes;
	int fourcc;
	short width;
	short height;
	uint32_t image_size;
	uint32_t pitches[3];
	uint32_t offsets[3];

	unsigned bo_idx;
	struct {
		struct drm_armada_bo *bo;
		phys_t phys;
		uint32_t fb_id;
	} bufs[NR_BUFS];

	union {
		phys_t phys;
		uint32_t name;
	} last;

	int (*get_fb)(ScrnInfoPtr, struct drm_xv *, unsigned char *,
		uint32_t *);

	/* Plane information */
	uint32_t plane_fb_id;
	drmModePlanePtr plane;
	drmModePlanePtr planes[2];
	const struct armada_format *plane_format;

	struct drm_armada_overlay_attrs attrs;
};

static XF86VideoEncodingRec OverlayEncodings[] = {
	{ 0, "XV_IMAGE", 2048, 2048, { 1, 1 }, },
};

/* The list of visuals that which we can render against - anything really */
static XF86VideoFormatRec OverlayFormats[] = {
	{ 8,  PseudoColor },
	{ 16, TrueColor },
	{ 24, TrueColor },
	{ 32, TrueColor },
};

static XF86AttributeRec OverlayAttributes[] = {
	{ XvSettable | XvGettable, 0,    1,          "XV_AUTOPAINT_COLORKEY" },
	{ XvSettable | XvGettable, 0,    0x00ffffff, "XV_COLORKEY" },
	{ XvSettable | XvGettable, -128, 127,        "XV_BRIGHTNESS" },
	{ XvSettable | XvGettable, 0,    255,        "XV_CONTRAST" },
	{ XvSettable | XvGettable, 0,    1023,       "XV_SATURATION" },
	{ XvSettable | XvGettable, -1,   0,          "XV_PIPE" },
};

/*
 * These are in order of preference.  The I420/YV12 formats require
 * conversion within the X server rather than the application, that's
 * relatively easy to do, and moreover involves reading less data than
 * I422/YV16.  YV16 and VYUY are not common formats (vlc at least does
 * not have any support for it but does have I422) so these comes at
 * the very end, to try to avoid vlc complaining about them.
 */
static const struct armada_format armada_drm_formats[] = {
	/* Standard Xv formats */
	{ DRM_FORMAT_UYVY,	XVIMAGE_UYVY,
		ARMADA_OVERLAY_YUV422 | ARMADA_OVERLAY_YUV_PACKED |
		ARMADA_OVERLAY_Y_SWAP },
	{ DRM_FORMAT_YUYV,	XVIMAGE_YUY2,
		ARMADA_OVERLAY_YUV422 | ARMADA_OVERLAY_YUV_PACKED },
	{ DRM_FORMAT_YUV420,	XVIMAGE_I420,
		ARMADA_OVERLAY_YUV420 | ARMADA_OVERLAY_YUV_PLANAR },
	{ DRM_FORMAT_YVU420,	XVIMAGE_YV12,
		ARMADA_OVERLAY_YUV420 | ARMADA_OVERLAY_YUV_PLANAR },
	/* Our own formats */
	{ DRM_FORMAT_YUV422,	XVIMAGE_I422,
		ARMADA_OVERLAY_YUV422 | ARMADA_OVERLAY_YUV_PLANAR },
	{ DRM_FORMAT_YVU422,	XVIMAGE_YV16,
		ARMADA_OVERLAY_YUV422 | ARMADA_OVERLAY_YUV_PLANAR },
	{ DRM_FORMAT_VYUY,	XVIMAGE_VYUY,
		ARMADA_OVERLAY_YUV422 | ARMADA_OVERLAY_YUV_PACKED |
		ARMADA_OVERLAY_Y_SWAP | ARMADA_OVERLAY_UV_SWAP },
	{ DRM_FORMAT_ARGB8888,	XVIMAGE_ARGB8888, },
	{ DRM_FORMAT_ABGR8888,	XVIMAGE_ABGR8888, },
	{ DRM_FORMAT_XRGB8888,	XVIMAGE_XRGB8888, },
	{ DRM_FORMAT_XBGR8888,	XVIMAGE_XBGR8888, },
	{ DRM_FORMAT_RGB888,	XVIMAGE_RGB888,	},
	{ DRM_FORMAT_BGR888,	XVIMAGE_BGR888, },
	{ DRM_FORMAT_ARGB1555,	XVIMAGE_ARGB1555, },
	{ DRM_FORMAT_ABGR1555,	XVIMAGE_ABGR1555, },
	{ DRM_FORMAT_RGB565,	XVIMAGE_RGB565 },
	{ DRM_FORMAT_BGR565,	XVIMAGE_BGR565 },
	{ 0,			XVIMAGE_XVBO },
};

/* It would be nice to be given the image pointer... */
static const struct armada_format *armada_drm_lookup_xvfourcc(int fmt)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(armada_drm_formats); i++)
		if (armada_drm_formats[i].xv_image.id == fmt)
			return &armada_drm_formats[i];
	return NULL;
}

static const struct armada_format *armada_drm_lookup_drmfourcc(uint32_t fmt)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(armada_drm_formats); i++)
		if (armada_drm_formats[i].drm_format == fmt)
			return &armada_drm_formats[i];
	return NULL;
}

static int
armada_drm_get_fmt_info(const struct armada_format *fmt,
	uint32_t *pitch, uint32_t *offset, short width, short height)
{
	const XF86ImageRec *img = &fmt->xv_image;
	int ret = 0;

	if (img->id == FOURCC_XVBO) {
		/* Our special XVBO format is only two uint32_t */
		pitch[0] = 2 * sizeof(uint32_t);
		offset[0] = 0;
		ret = pitch[0];
	} else if (img->format == XvPlanar) {
		uint32_t size[3];

		pitch[0] = width / img->horz_y_period;
		pitch[1] = width / img->horz_u_period;
		pitch[2] = width / img->horz_v_period;
		size[0] = (pitch[0] * (height / img->vert_y_period) + 7) & ~7;
		size[1] = (pitch[1] * (height / img->vert_u_period) + 7) & ~7;
		size[2] = (pitch[2] * (height / img->vert_v_period) + 7) & ~7;
		offset[0] = 0;
		offset[1] = offset[0] + size[0];
		offset[2] = offset[1] + size[1];

		ret = size[0] + size[1] + size[2];
	} else if (img->format == XvPacked) {
		offset[0] = 0;
		pitch[0] = width * ((img->bits_per_pixel + 7) / 8);
		ret = offset[0] + pitch[0] * height;
	}

	return ret;
}

static void
armada_drm_coords_to_box(BoxPtr box, short x, short y, short w, short h)
{
	box->x1 = x;
	box->y1 = y;
	box->x2 = x + w;
	box->y2 = y + h;
}

static void armada_drm_bufs_free(struct drm_xv *drmxv)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++) {
		if (drmxv->bufs[i].fb_id) {
			if (drmxv->bufs[i].fb_id == drmxv->plane_fb_id)
				drmxv->plane_fb_id = 0;
			drmModeRmFB(drmxv->drm->fd, drmxv->bufs[i].fb_id);
			drmxv->bufs[i].fb_id = 0;
		}
		if (drmxv->bufs[i].bo) {
			drm_armada_bo_put(drmxv->bufs[i].bo);
			drmxv->bufs[i].bo = NULL;
		}
		drmxv->bufs[i].phys = INVALID_PHYS;
	}

	if (drmxv->plane_fb_id) {
		drmModeRmFB(drmxv->drm->fd, drmxv->plane_fb_id);
		drmxv->plane_fb_id = 0;
	}
}

static Bool
armada_drm_create_fbid(struct drm_xv *drmxv, struct drm_armada_bo *bo,
	uint32_t *id)
{
	uint32_t handles[3];

	/* Just set the three plane handles to be the same */
	handles[0] =
	handles[1] =
	handles[2] = bo->handle;

	/* Create the framebuffer object for this buffer */
	if (drmModeAddFB2(drmxv->drm->fd, drmxv->width, drmxv->height,
			  drmxv->plane_format->drm_format, handles,
			  drmxv->pitches, drmxv->offsets, id, 0))
		return FALSE;

	return TRUE;
}

static int armada_drm_bufs_alloc(struct drm_xv *drmxv)
{
	struct drm_armada_bufmgr *bufmgr = drmxv->drm->bufmgr;
	uint32_t width = drmxv->width;
	uint32_t height = drmxv->image_size / width / 4;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++) {
		struct drm_armada_bo *bo;

		bo = drm_armada_bo_dumb_create(bufmgr, width, height, 32);
		drmxv->bufs[i].bo = bo;
		if (!bo || drm_armada_bo_map(bo) ||
		    !armada_drm_create_fbid(drmxv, bo, &drmxv->bufs[i].fb_id)) {
			armada_drm_bufs_free(drmxv);
			return BadAlloc;
		}
	}

	return Success;
}

/*
 * The Marvell Xv protocol hack.
 *
 * This is pretty disgusting - it passes a magic number, a count, the
 * physical address of the BMM buffer, and a checksum via the Xv image
 * interface.
 *
 * The X server is then expected to queue the frame for display, and
 * then overwrite the SHM buffer with its own magic number, a count,
 * the physical address of a used BMM buffer, and a checksum back to
 * the application.
 *
 * Looking at other gstreamer implementations (such as fsl) this kind
 * of thing seems to be rather common, though normally only in one
 * direction.
 */
#define BMM_SHM_MAGIC1  0x13572468
#define BMM_SHM_MAGIC2  0x24681357

static uint32_t armada_drm_bmm_chk(unsigned char *buf, uint32_t len)
{
	uint32_t i, chk, *ptr = (uint32_t *)buf;

	for (chk = i = 0; i < len; i++)
		 chk ^= ptr[i];

	return chk;
}

static Bool armada_drm_is_bmm(unsigned char *buf)
{
	uint32_t *ptr, len;

	if ((uintptr_t)buf & (sizeof(*ptr) - 1))
		return FALSE;

	ptr = (uint32_t *)buf;
	if (*ptr != BMM_SHM_MAGIC1)
		return FALSE;

	len = 2 + ptr[1];
	return armada_drm_bmm_chk(buf, len) == ptr[len];
}

static phys_t armada_drm_bmm_getbuf(unsigned char *buf)
{
	uint32_t *ptr, len;

	if ((uintptr_t)buf & (sizeof(*ptr) - 1))
		return INVALID_PHYS;

	ptr = (uint32_t *)buf;
	if (*ptr != BMM_SHM_MAGIC1)
		return INVALID_PHYS;

	len = 2 + ptr[1];
	/* Only one buffer per call please */
	if (len > 3)
		return INVALID_PHYS;

	if (armada_drm_bmm_chk(buf, len) != ptr[len])
		return INVALID_PHYS;

	return ptr[2];
}

static void armada_drm_bmm_putbuf(unsigned char *buf, phys_t phys)
{
	uint32_t *ptr = (uint32_t *)buf;

	*ptr++ = BMM_SHM_MAGIC2;
	*ptr++ = 1;
	*ptr++ = phys;
	*ptr = armada_drm_bmm_chk(buf, 3);
}

static int
armada_drm_get_bmm(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *buf,
	uint32_t *id)
{
	struct drm_armada_bo *bo;
	phys_t phys;

	phys = armada_drm_bmm_getbuf(buf);
	if (phys == INVALID_PHYS)
		return BadAlloc;

	/* Is this a re-display of the previous frame? */
	if (drmxv->last.phys == phys) {
		*id = drmxv->plane_fb_id;
		return Success;
	}

	/* Map the passed buffer into a bo */
	bo = drm_armada_bo_create_phys(drmxv->drm->bufmgr, phys,
				       drmxv->image_size);
	if (bo) {
		phys_t old;

		/* Return the now unused phys buffer */
		old = drmxv->bufs[drmxv->bo_idx].phys;
		if (old != INVALID_PHYS)
			armada_drm_bmm_putbuf(buf, old);
		drmxv->bufs[drmxv->bo_idx].phys = phys;

		/* Move to the next buffer index now */
		if (++drmxv->bo_idx >= ARRAY_SIZE(drmxv->bufs))
			drmxv->bo_idx = 0;

		if (!armada_drm_create_fbid(drmxv, bo, id)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"[drm] BMM: drmModeAddFB2 failed: %s\n",
				strerror(errno));
			return BadAlloc;
		}

		drmxv->last.phys = phys;

		/*
		 * We're done with this buffer object.  We can drop our
		 * reference to it as it is now bound to the framebuffer,
		 * which will keep its own refcount(s) on the buffer.
		 */
		drm_armada_bo_put(bo);
	}

	return Success;
}

static int
armada_drm_get_xvbo(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *buf,
	uint32_t *id)
{
	struct drm_armada_bo *bo;
	uint32_t name = ((uint32_t *)buf)[1];

	/* Is this a re-display of the previous frame? */
	if (drmxv->last.name == name) {
		*id = drmxv->plane_fb_id;
		return Success;
	}

	bo = drm_armada_bo_create_from_name(drmxv->drm->bufmgr, name);
	if (bo) {
		if (!armada_drm_create_fbid(drmxv, bo, id)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"[drm] XVBO: drmModeAddFB2 failed: %s\n",
				strerror(errno));
			return BadAlloc;
		}

		drmxv->last.name = name;

		/*
		 * We're done with this buffer object.  We can drop our
		 * reference to it as it is now bound to the framebuffer,
		 * which will keep its own refcount(s) on the buffer.
		 */
		drm_armada_bo_put(bo);
	}

	return Success;
}

static int
armada_drm_get_std(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *src,
	uint32_t *id)
{
	struct drm_armada_bo *bo = drmxv->bufs[drmxv->bo_idx].bo;

	if (bo) {
		/* Copy new image data into the buffer */
		memcpy(bo->ptr, src, drmxv->image_size);

		/* Return this buffer's framebuffer id */
		*id = drmxv->bufs[drmxv->bo_idx].fb_id;

		/* Move to the next buffer index now */
		if (++drmxv->bo_idx >= ARRAY_SIZE(drmxv->bufs))
			drmxv->bo_idx = 0;
	}

	return bo ? Success : BadAlloc;
}

/* Common methods */
static int
armada_drm_Xv_SetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;
	Bool update_attrs = TRUE;
	int ret = Success;

	if (attribute == xvAutoPaintColorKey) {
		drmxv->autopaint_colorkey = !!value;
		if (value == 0)
			drmxv->attrs.color_key = 0;
	} else if (attribute == xvColorKey)
		drmxv->attrs.color_key = value;
	else if (attribute == xvBrightness)
		drmxv->attrs.brightness = value;
	else if (attribute == xvContrast)
		drmxv->attrs.contrast = value;
	else if (attribute == xvSaturation)
		drmxv->attrs.saturation = value;
	else if (attribute == xvDeinterlace)
		update_attrs = FALSE;
	else if (attribute == xvPipe) {
		xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

		if (value < -1 || value >= config->num_crtc)
			return BadValue;
		if (value == -1)
			drmxv->desired_crtc = NULL;
		else
			drmxv->desired_crtc = config->crtc[value];

		update_attrs = FALSE;
	} else
		ret = BadMatch;

	if (ret == Success && update_attrs) {
		struct drm_armada_overlay_attrs arg;

		arg = drmxv->attrs;
		arg.flags = ARMADA_OVERLAY_UPDATE_ATTRS;
		drmIoctl(drmxv->drm->fd, DRM_IOCTL_ARMADA_OVERLAY_ATTRS, &arg);
	}

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "SetPortAttribute: attrib %#lx value %#lx ret %u\n",
		   attribute, value, ret);

	return ret;
}

static int
armada_drm_Xv_GetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	int ret = Success;

	if (attribute == xvAutoPaintColorKey)
		*value = drmxv->autopaint_colorkey;
	else if (attribute == xvColorKey)
		*value = drmxv->attrs.color_key;
	else if (attribute == xvBrightness)
		*value = drmxv->attrs.brightness;
	else if (attribute == xvContrast)
		*value = drmxv->attrs.contrast;
	else if (attribute == xvSaturation)
		*value = drmxv->attrs.saturation;
	else if (attribute == xvPipe) {
		xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
		int i;

		for (i = 0; i < config->num_crtc; i++)
			if (config->crtc[i] == drmxv->desired_crtc)
				break;

		if (i == config->num_crtc)
			i = -1;

		*value = i;
	} else
		ret = BadMatch;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "GetPortAttribute: attrib %#lx value %#lx ret %u\n",
		   attribute, *value, ret);

	return ret;
}

static void armada_drm_Xv_QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
	short vid_w, short vid_h, short drw_w, short drw_h,
	unsigned int *p_w, unsigned int *p_h, pointer data)
{
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "QueryBestSize: vid %dx%d drw %dx%d\n",
		   vid_w, vid_h, drw_w, drw_h);
	*p_w = maxt(vid_w, drw_w);
	*p_h = maxt(vid_h, drw_h); 
}

static int
armada_drm_Xv_QueryImageAttributes(ScrnInfoPtr pScrn, int image,
	unsigned short *width, unsigned short *height, int *pitches,
	int *offsets)
{
	const struct armada_format *fmt;
	unsigned i, ret = 0;
	uint32_t pitch[3], offset[3];

	*width = (*width + 1) & ~1;
	*height = (*height + 1) & ~1;

	fmt = armada_drm_lookup_xvfourcc(image);
	if (!fmt)
		return 0;

	ret = armada_drm_get_fmt_info(fmt, pitch, offset, *width, *height);
	if (ret) {
		for (i = 0; i < fmt->xv_image.num_planes; i++) {
			if (pitches)
				pitches[i] = pitch[i];
			if (offsets)
				offsets[i] = offset[i];
		}
	}

	return ret;
}

/* Plane interface support */
static int
armada_drm_plane_fbid(ScrnInfoPtr pScrn, struct drm_xv *drmxv, int image,
	unsigned char *buf, short width, short height, uint32_t *id)
{
	const struct armada_format *fmt;
	struct drm_armada_bo *bo;
	Bool is_bo = image == FOURCC_XVBO;
	int ret;

	if (is_bo)
		/*
		 * XVBO support allows applications to prepare the DRM
		 * buffer object themselves, and pass a global name to
		 * the X server to update the hardware with.  This is
		 * similar to Intel XvMC support, except we also allow
		 * the image format to be specified via a fourcc as the
		 * first word.
		 */
		image = ((uint32_t *)buf)[0];

	if (drmxv->width != width || drmxv->height != height ||
	    drmxv->fourcc != image || !drmxv->plane_format) {
		uint32_t size;

		/* format or size changed */
		fmt = armada_drm_lookup_xvfourcc(image);
		if (!fmt)
			return BadMatch;

		/* Check whether this is XVBO mapping */
		if (is_bo) {
			drmxv->is_bmm = TRUE;
			drmxv->get_fb = armada_drm_get_xvbo;
			drmxv->last.name = 0;
		} else if (armada_drm_is_bmm(buf)) {
			drmxv->is_bmm = TRUE;
			drmxv->get_fb = armada_drm_get_bmm;
			drmxv->last.phys = INVALID_PHYS;
		} else {
			drmxv->is_bmm = FALSE;
			drmxv->get_fb = armada_drm_get_std;
		}

		armada_drm_bufs_free(drmxv);

		size = armada_drm_get_fmt_info(fmt, drmxv->pitches,
					       drmxv->offsets, width, height);

		drmxv->image_size = size;
		drmxv->width = width;
		drmxv->height = height;
		drmxv->fourcc = image;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "[drm] bmm %u xvbo %u fourcc %08x\n",
			   drmxv->is_bmm, is_bo, image);

		/* Pre-allocate the buffers if we aren't using XVBO or BMM */
		if (!drmxv->is_bmm) {
			int ret = armada_drm_bufs_alloc(drmxv);
			if (ret != Success)
				return ret;
		}

		drmxv->plane_format = fmt;
	}

	ret = drmxv->get_fb(pScrn, drmxv, buf, id);
	if (ret != Success) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] Xv: failed to get framebuffer\n");
		return ret;
	}

	return Success;
}

static void
armada_drm_plane_StopVideo(ScrnInfoPtr pScrn, pointer data, Bool cleanup)
{
	struct drm_xv *drmxv = data;

	if (drmxv->plane) {
		int ret;

		RegionEmpty(&drmxv->clipBoxes);

		ret = drmModeSetPlane(drmxv->drm->fd, drmxv->plane->plane_id,
				      0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0);
		if (ret)
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "[drm] unable to stop overlay: %s\n",
				   strerror(errno));
	}

	if (cleanup) {
		drmxv->plane_format = NULL;
		armada_drm_bufs_free(drmxv);
	}
}

static Bool armada_drm_check_plane(ScrnInfoPtr pScrn, struct drm_xv *drmxv,
	xf86CrtcPtr crtc)
{
	drmModePlanePtr plane;
	uint32_t crtc_mask;

	if (!crtc) {
		/* Not being displayed on a CRTC */
		armada_drm_plane_StopVideo(pScrn, drmxv, TRUE);
		return FALSE;
	}

	crtc_mask = 1 << armada_crtc(crtc)->num;

	plane = drmxv->plane;
	if (plane && !(plane->possible_crtcs & crtc_mask)) {
		/* Moved on to a different CRTC */
		armada_drm_plane_StopVideo(pScrn, drmxv, FALSE);
		plane = NULL;
	}

	if (!plane) {
		unsigned i;

		for (i = 0; i < ARRAY_SIZE(drmxv->planes); i++)
			if (drmxv->planes[i] &&
			    drmxv->planes[i]->possible_crtcs & crtc_mask)
				plane = drmxv->planes[i];

		/* Our new plane */
		drmxv->plane = plane;

		if (!plane)
			return FALSE;
	}

	return TRUE;
}

static int
armada_drm_plane_Put(ScrnInfoPtr pScrn, struct drm_xv *drmxv, uint32_t fb_id,
	short src_x, short src_y, short src_w, short src_h,
	short width, short height, BoxPtr dst, RegionPtr clipBoxes)
{
	drmModePlanePtr plane;
	xf86CrtcPtr crtc = NULL;
	uint32_t crtc_x, crtc_y;
	INT32 x1, x2, y1, y2;

	x1 = src_x;
	x2 = src_x + src_w;
	y1 = src_y;
	y2 = src_y + src_h;

	if (!xf86_crtc_clip_video_helper(pScrn, &crtc, drmxv->desired_crtc,
					 dst, &x1, &x2, &y1, &y2, clipBoxes,
					 width, height))
		return BadAlloc;

	if (!armada_drm_check_plane(pScrn, drmxv, crtc))
		return Success;

	/* Calculate the position on this CRTC */
	crtc_x = dst->x1 - crtc->x;
	crtc_y = dst->y1 - crtc->y;

	plane = drmxv->plane;
	drmModeSetPlane(drmxv->drm->fd, plane->plane_id,
			armada_crtc(crtc)->mode_crtc->crtc_id, fb_id, 0,
			crtc_x, crtc_y, dst->x2 - dst->x1, dst->y2 - dst->y1,
			x1, y1, x2 - x1, y2 - y1);

	/*
	 * Finally, fill the clip boxes; do this after we've done the ioctl
	 * so we don't impact on latency.
	 */
	if (drmxv->autopaint_colorkey &&
	    !RegionEqual(&drmxv->clipBoxes, clipBoxes)) {
		RegionCopy(&drmxv->clipBoxes, clipBoxes);
		xf86XVFillKeyHelper(pScrn->pScreen, drmxv->attrs.color_key,
				    clipBoxes);
	}

	return Success;
}

static int armada_drm_plane_PutImage(ScrnInfoPtr pScrn,
        short src_x, short src_y, short drw_x, short drw_y,
        short src_w, short src_h, short drw_w, short drw_h,
        int image, unsigned char *buf, short width, short height,
        Bool sync, RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	struct drm_xv *drmxv = data;
	BoxRec dst;
	uint32_t fb_id;
	int ret;

	armada_drm_coords_to_box(&dst, drw_x, drw_y, drw_w, drw_h);

	ret = armada_drm_plane_fbid(pScrn, drmxv, image, buf, width, height,
				    &fb_id);
	if (ret != Success)
		return ret;

	ret = armada_drm_plane_Put(pScrn, drmxv, fb_id,
				    src_x, src_y, src_w, src_h,
				    width, height, &dst, clipBoxes);

	/* If there was a previous fb, release it. */
	if (drmxv->is_bmm &&
	    drmxv->plane_fb_id && drmxv->plane_fb_id != fb_id) {
		drmModeRmFB(drmxv->drm->fd, drmxv->plane_fb_id);
		drmxv->plane_fb_id = 0;
	}

	drmxv->plane_fb_id = fb_id;

	return ret;
}

static int armada_drm_plane_ReputImage(ScrnInfoPtr pScrn,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	struct drm_xv *drmxv = data;
	BoxRec dst;

	if (drmxv->plane_fb_id == 0)
		return Success;

	armada_drm_coords_to_box(&dst, drw_x, drw_y, drw_w, drw_h);

	return armada_drm_plane_Put(pScrn, drmxv, drmxv->plane_fb_id,
				    src_x, src_y, src_w, src_h,
				    drmxv->width, drmxv->height,
				    &dst, clipBoxes);
}

static XF86VideoAdaptorPtr
armada_drm_XvInitPlane(ScrnInfoPtr pScrn, DevUnion *priv, struct drm_xv *drmxv)
{
	XF86VideoAdaptorPtr p;
	XF86ImageRec *images;
	unsigned i, num_images;

	p = xf86XVAllocateVideoAdaptorRec(pScrn);
	if (!p)
		return NULL;

#if 0
	images = calloc(ARRAY_SIZE(armada_drm_formats), sizeof(*images));
	if (!images) {
		free(p);
		return NULL;
	}

	for (num_images = i = 0; i < ARRAY_SIZE(armada_drm_formats); i++) {
		unsigned j;

		if (armada_drm_formats[i].drm_format == 0)
			continue;

		for (j = 0; j < drmxv->planes[0]->count_formats; j++) {
			if (armada_drm_formats[i].drm_format ==
			    drmxv->planes[0]->formats[j]) {
				images[num_images++] = armada_drm_formats[i].xv_image;
			}
		}
	}
#else
	images = calloc(drmxv->planes[0]->count_formats, sizeof(*images));
	if (!images) {
		free(p);
		return NULL;
	}

	for (num_images = i = 0; i < drmxv->planes[0]->count_formats; i++) {
		const struct armada_format *fmt;
		uint32_t id = drmxv->planes[0]->formats[i];

		if (id == 0)
			continue;

		fmt = armada_drm_lookup_drmfourcc(id);
		if (fmt)
			images[num_images++] = fmt->xv_image;
	}
#endif

	p->type = XvWindowMask | XvInputMask | XvImageMask;
	p->flags = VIDEO_OVERLAID_IMAGES;
	p->name = "Marvell Armada Overlay Video Plane";
	p->nEncodings = sizeof(OverlayEncodings) / sizeof(XF86VideoEncodingRec);
	p->pEncodings = OverlayEncodings;
	p->nFormats = sizeof(OverlayFormats) / sizeof(XF86VideoFormatRec);
	p->pFormats = OverlayFormats;
	p->nPorts = 1;
	p->pPortPrivates = priv;
	p->nAttributes = sizeof(OverlayAttributes) / sizeof(XF86AttributeRec);
	p->pAttributes = OverlayAttributes;
	p->nImages = num_images;
	p->pImages = images;
	p->StopVideo = armada_drm_plane_StopVideo;
	p->SetPortAttribute = armada_drm_Xv_SetPortAttribute;
	p->GetPortAttribute = armada_drm_Xv_GetPortAttribute;
	p->QueryBestSize = armada_drm_Xv_QueryBestSize;
	p->PutImage = armada_drm_plane_PutImage;
	p->ReputImage = armada_drm_plane_ReputImage;
	p->QueryImageAttributes = armada_drm_Xv_QueryImageAttributes;

	return p;
}

Bool armada_drm_XvInit(ScrnInfoPtr pScrn)
{
	ScreenPtr scrn = screenInfo.screens[pScrn->scrnIndex];
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	XF86VideoAdaptorPtr xv[2], plane;
	drmModePlaneResPtr res;
	struct drm_xv *drmxv;
	DevUnion priv[1];
	unsigned i, num;
	Bool ret;

	if (!xvColorKey) {
		xvAutoPaintColorKey = MAKE_ATOM("XV_AUTOPAINT_COLORKEY");
		xvColorKey = MAKE_ATOM("XV_COLORKEY");
		xvBrightness = MAKE_ATOM("XV_BRIGHTNESS");
		xvContrast = MAKE_ATOM("XV_CONTRAST");
		xvSaturation = MAKE_ATOM("XV_SATURATION");
		xvDeinterlace = MAKE_ATOM("XV_DEINTERLACE");
		xvPipe = MAKE_ATOM("XV_PIPE");
	}

	/* FIXME: we leak this */
	drmxv = calloc(1, sizeof *drmxv);
	if (!drmxv)
		return FALSE;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++)
		drmxv->bufs[i].phys = INVALID_PHYS;

	/* Get the plane resources and the overlay planes */
	res = drmModeGetPlaneResources(drm->fd);
	if (!res)
		goto err_free;

	/* Get all plane information */
	for (i = 0; i < res->count_planes && i < ARRAY_SIZE(drmxv->planes); i++) {
		uint32_t plane_id = res->planes[i];

		drmxv->planes[i] = drmModeGetPlane(drm->fd, plane_id);
		if (!drmxv->planes[i]) {
			drmModeFreePlaneResources(res);
			goto err_free;
		}
	}

	/* Done with the plane resources */
	drmModeFreePlaneResources(res);

	drmxv->drm = drm;
	drmxv->autopaint_colorkey = TRUE;

	drmIoctl(drm->fd, DRM_IOCTL_ARMADA_OVERLAY_ATTRS, &drmxv->attrs);

	for (i = 0; i < ARRAY_SIZE(drmxv->planes); i++) {
		drmModePlanePtr plane = drmxv->planes[i];
		unsigned j;

		if (!plane)
			continue;

		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			"Plane %u CRTC %u FB %u Possible CRTC %08x\n",
			plane->plane_id, plane->crtc_id, plane->fb_id,
			plane->possible_crtcs);
		for (j = 0; j < plane->count_formats; j++) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				"  Format %08x %.4s\n",
				plane->formats[j],
				(char *)&plane->formats[j]);
		}
	}

	num = 0;
	priv[0].ptr = drmxv;

	if (drmxv->planes[0]) {
		plane = armada_drm_XvInitPlane(pScrn, priv, drmxv);
		if (!plane)
			goto err_free;
		xv[num++] = plane;
	}

	ret = xf86XVScreenInit(scrn, xv, num);

	for (i = 0; i < num; i++) {
		if (xv[i]) {
			free(xv[i]->pImages);
			free(xv[i]);
		}
	}
	if (!ret)
		goto err_free;
	return TRUE;

 err_free:
	for (i = 0; i < ARRAY_SIZE(drmxv->plane); i++)
		if (drmxv->planes[i])
			drmModeFreePlane(drmxv->planes[i]);
	free(drmxv);
	return FALSE;
}
