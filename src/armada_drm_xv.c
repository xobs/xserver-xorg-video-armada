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
#include "xf86Crtc.h"
#include "xf86xv.h"
#include "fourcc.h"
#include <X11/extensions/Xv.h>
#include <X11/Xatom.h>

#include "armada_ioctl.h"

#define FOURCC_VYUY 0x59555956
#define XVIMAGE_VYUY \
	{ \
		FOURCC_VYUY, XvYUV, LSBFirst, \
		{ 'V', 'Y', 'U', 'Y', 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }, \
		16, XvPacked, 1, \
		0, 0, 0, 0, \
		8, 8, 8, \
		1, 2, 2, \
		1, 1, 1, \
		{ 'V', 'Y', 'U', 'Y' }, \
		XvTopToBottom, \
	}

#define FOURCC_I422 0x32323449
#define XVIMAGE_I422 \
	{ \
		FOURCC_I422, XvYUV, LSBFirst, \
		{ 'I', '4', '2', '2', 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }, \
		16, XvPlanar, 3, \
		0, 0, 0, 0, \
		8, 8, 8, \
		1, 2, 2, \
		1, 1, 1, \
		{ 'Y', 'U', 'V' }, \
		XvTopToBottom, \
	}

#define FOURCC_YV16 0x36315659
#define XVIMAGE_YV16 \
	{ \
		FOURCC_YV16, XvYUV, LSBFirst, \
		{ 'Y', 'V', '1', '6', 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }, \
		16, XvPlanar, 3, \
		0, 0, 0, 0, \
		8, 8, 8, \
		1, 2, 2, \
		1, 1, 1, \
		{ 'Y', 'V', 'U' }, \
		XvTopToBottom, \
	}

#define FOURCC_XVBO 0x4f425658
#define XVIMAGE_XVBO \
	{ \
		FOURCC_XVBO, XvYUV, LSBFirst, \
		{ 'X', 'V', 'B', 'O', 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }, \
		16, XvPacked, 1, \
		0, 0, 0, 0, \
		8, 8, 8, \
		1, 2, 2, \
		1, 1, 1, \
		{ 'U', 'Y', 'V', 'Y' }, \
		XvTopToBottom, \
	}

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

struct drm_xv {
	struct armada_drm_info *drm;
	xf86CrtcPtr desired_crtc;
	Bool is_bmm;
	Bool autopaint_colorkey;

	/* Cache the information */
	int image_fourcc;
	short image_width;
	short image_height;
	const XF86ImageRec *image;
	uint32_t image_flags;
	uint32_t image_size;
	int pitches[3];
	int offsets[3];
	int (*cvt)(struct drm_xv *, unsigned char *, struct drm_armada_bo **);

	unsigned bo_idx;
	struct drm_armada_bo *bo[3];
	phys_t bo_phys[3];

	RegionRec clipBoxes;

	struct drm_armada_overlay_attrs attrs;
	struct drm_armada_overlay_put_image arg;
};

static XF86VideoEncodingRec OverlayEncodings[] = {
	{ 0, "XV_IMAGE", 2048, 2048, { 1, 1 }, },
};

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
static XF86ImageRec OverlayImages[] = {
	XVIMAGE_I420,
	XVIMAGE_YV12,
	XVIMAGE_I422,
	XVIMAGE_YUY2,
	XVIMAGE_UYVY,
	XVIMAGE_VYUY,
	XVIMAGE_YV16,
	XVIMAGE_XVBO,
};

/* It would be nice to be given the image pointer... */
static const XF86ImageRec *armada_drm_ovl_get_img(int image)
{
	const XF86ImageRec *img = NULL;
	int i;

	for (i = 0; i < sizeof(OverlayImages) / sizeof(XF86ImageRec); i++)
		if (OverlayImages[i].id == image) {
			img = &OverlayImages[i];
			break;
		}

	return img;
}

static int armada_drm_ovl_set_img_info(const XF86ImageRec *img,
	int *pitch, int *offset, short width, short height)
{
	int ret = 0;

	if (img->id == FOURCC_XVBO) {
		/* Our special XVBO format is only two uint32_t */
		pitch[0] = 2 * sizeof(uint32_t);
		offset[0] = 0;
		ret = pitch[0];
	} else if (img->format == XvPlanar) {
		int size[3];

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

static void armada_drm_ovl_bo_free(struct drm_xv *drmxv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bo); i++)
		if (drmxv->bo[i]) {
			drm_armada_bo_put(drmxv->bo[i]);
			drmxv->bo[i] = NULL;
		}
}

static struct drm_armada_bo *armada_drm_ovl_get_bo(struct drm_xv *drmxv,
	size_t size, size_t width)
{
	struct armada_drm_info *drm = drmxv->drm;
	struct drm_armada_bo *bo;

	bo = drm_armada_bo_dumb_create(drm->bufmgr, width, size / width / 4, 32);
	if (bo && drm_armada_bo_map(bo)) {
		drm_armada_bo_put(bo);
		bo = NULL;
	}
	return bo;
}

static void armada_drm_ovl_StopVideo(ScrnInfoPtr pScrn, pointer data, Bool cleanup)
{
	struct drm_xv *drmxv = data;
	struct drm_armada_overlay_put_image arg;
	int ret;

	RegionEmpty(&drmxv->clipBoxes);

	memset(&arg, 0, sizeof(arg));
	ret = drmIoctl(drmxv->drm->fd, DRM_IOCTL_ARMADA_OVERLAY_PUT_IMAGE, &arg);
	if (ret)
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] unable to stop overlay: %s\n", strerror(errno));

	if (cleanup) {
		drmxv->image = NULL;
		armada_drm_ovl_bo_free(drmxv);
	}
}

static int armada_drm_ovl_SetPortAttribute(ScrnInfoPtr pScrn, Atom attribute, INT32 value, pointer data)
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

static int armada_drm_ovl_GetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
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

static void armada_drm_ovl_QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
	short vid_w, short vid_h, short drw_w, short drw_h,
	unsigned int *p_w, unsigned int *p_h, pointer data)
{
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "QueryBestSize: vid %dx%d drw %dx%d\n",
		   vid_w, vid_h, drw_w, drw_h);
	*p_w = maxt(vid_w, drw_w);
	*p_h = maxt(vid_h, drw_h); 
}

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
		return (phys_t)-1;

	ptr = (uint32_t *)buf;
	if (*ptr != BMM_SHM_MAGIC1)
		return (phys_t)-1;

	len = 2 + ptr[1];
	if (armada_drm_bmm_chk(buf, len) != ptr[len])
		return (phys_t)-1;

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

static int armada_drm_ovl_bmm(struct drm_xv *drmxv, unsigned char *buf, struct drm_armada_bo **pbo)
{
	struct drm_armada_bo *old, *bo;
	phys_t phys;

	/* Marvell Dove special protocol */
	phys = armada_drm_bmm_getbuf(buf);
	if (phys == (phys_t)-1)
		return BadAlloc;

	/* Map the passed buffer into a bo */
	bo = drm_armada_bo_create_phys(drmxv->drm->bufmgr, phys, drmxv->image_size);
	if (!bo)
		return BadAlloc;

	/* Free the old bo, and pass it back */
	old = drmxv->bo[drmxv->bo_idx];
	if (old) {
		drm_armada_bo_put(old);
		armada_drm_bmm_putbuf(buf, drmxv->bo_phys[drmxv->bo_idx]);
	}
	drmxv->bo[drmxv->bo_idx] = bo;
	drmxv->bo_phys[drmxv->bo_idx] = phys;

	*pbo = bo;

	drm_armada_bo_get(bo);

	return Success;
}

static int armada_drm_ovl_cvt(struct drm_xv *drmxv, unsigned char *src, struct drm_armada_bo **pbo)
{
	struct drm_armada_bo *bo;
	unsigned char *dst;
	size_t size;
	int i;

	/* Standard XV protocol */
	bo = drmxv->bo[drmxv->bo_idx];
	if (!bo)
		return BadAlloc;

	if (++drmxv->bo_idx >= ARRAY_SIZE(drmxv->bo))
		drmxv->bo_idx = 0;

	*pbo = bo;

	/* Convert YV12 to YV16 or I420 to I422 */
	dst = bo->ptr;
	size = drmxv->offsets[1];

	/* Copy Y */
	memcpy(dst, src, size);

	dst += size;
	src += size;

	/* Copy U and V, doubling the number of pixels vertically */
	size = drmxv->pitches[1];
	for (i = drmxv->image_height; i > 0; i--) {
		memcpy(dst, src, size);
		dst += size;
		memcpy(dst, src, size);
		dst += size;
		src += size;
	}

	drm_armada_bo_get(bo);

	return Success;
}

static int armada_drm_ovl_std(struct drm_xv *drmxv, unsigned char *src, struct drm_armada_bo **pbo)
{
	struct drm_armada_bo *bo;

	/* Standard XV protocol */
	bo = drmxv->bo[drmxv->bo_idx];
	if (!bo)
		return BadAlloc;

	if (++drmxv->bo_idx >= ARRAY_SIZE(drmxv->bo))
		drmxv->bo_idx = 0;

	*pbo = bo;

	memcpy(bo->ptr, src, drmxv->image_size);

	drm_armada_bo_get(bo);

	return Success;
}

static int armada_drm_ovl_SetupStd(struct drm_xv *drmxv, int image,
	short width, short height)
{
	int i, size, image2 = image;
	const XF86ImageRec *img;

	drmxv->image = NULL;

	/*
	 * We can't actually do YV12 or I420 - we convert to YV16
	 * or YV16 with U and V swapped.
	 */
	if (image2 == FOURCC_YV12)
		image2 = FOURCC_YV16;
	if (image2 == FOURCC_I420)
		image2 = FOURCC_I422;

	/*
	 * Lookup the image type.  Note that the server already did this in
	 * Xext/xvdisp.c, but xf86XVPutImage() in hw/xfree86/common/xf86xv.c
	 * threw that information away.
	 */
	img = armada_drm_ovl_get_img(image2);
	if (!img)
		return BadMatch;

	size = armada_drm_ovl_set_img_info(img, drmxv->pitches, drmxv->offsets,
					   width, height);
	if (!size)
		return BadAlloc;

	drmxv->arg.flags = ARMADA_OVERLAY_ENABLE;
	if (img->type == XvYUV) {
		if (img->vert_u_period == 1)
			drmxv->arg.flags |= ARMADA_OVERLAY_YUV422;
		else
			drmxv->arg.flags |= ARMADA_OVERLAY_YUV420;
	}

	if (img->format == XvPlanar) {
		drmxv->arg.flags |= ARMADA_OVERLAY_YUV_PLANAR;
		for (i = 0; i < img->num_planes; i++) {
			switch (img->component_order[i]) {
			case 'Y':
				drmxv->arg.offset_Y = drmxv->offsets[i];
				drmxv->arg.stride_Y = drmxv->pitches[i];
				break;
			case 'U':
				drmxv->arg.offset_U = drmxv->offsets[i];
				drmxv->arg.stride_UV = drmxv->pitches[i];
				break;
			case 'V':
				drmxv->arg.offset_V = drmxv->offsets[i];
				drmxv->arg.stride_UV = drmxv->pitches[i];
				break;
			}
		}
	} else {
		drmxv->arg.flags |= ARMADA_OVERLAY_YUV_PACKED;
		drmxv->arg.offset_Y = drmxv->offsets[0];
		drmxv->arg.offset_U = drmxv->offsets[0];
		drmxv->arg.offset_V = drmxv->offsets[0];
		drmxv->arg.stride_Y = drmxv->pitches[0];
		drmxv->arg.stride_UV = drmxv->pitches[0];

		switch (image) {
		case FOURCC_VYUY:
			drmxv->arg.flags |= ARMADA_OVERLAY_UV_SWAP;
		case FOURCC_UYVY:
			drmxv->arg.flags |= ARMADA_OVERLAY_Y_SWAP;
		case FOURCC_YUY2:
			break;
		default:
			return BadAlloc;
		}
	}

	armada_drm_ovl_bo_free(drmxv);

	if (drmxv->is_bmm) {
		drmxv->cvt = armada_drm_ovl_bmm;
	} else {
		if (image != image2)
			drmxv->cvt = armada_drm_ovl_cvt;
		else
			drmxv->cvt = armada_drm_ovl_std;

		for (i = 0; i < ARRAY_SIZE(drmxv->bo); i++) {
			drmxv->bo[i] = armada_drm_ovl_get_bo(drmxv, size, width);
			if (!drmxv->bo[i]) {
				armada_drm_ovl_bo_free(drmxv);
				return BadAlloc;
			}
		}
	}

	drmxv->image = img;
	drmxv->image_size = size;
	drmxv->image_width = width;
	drmxv->image_height = height;
	drmxv->image_fourcc = image;

	return Success;
}

static int armada_drm_ovl_clip(ScrnInfoPtr pScrn, struct drm_xv *drmxv,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	RegionPtr clipBoxes, short width, short height, xf86CrtcPtr *crtcp)
{
	struct armada_crtc_info *drmc;
	xf86CrtcPtr crtc = NULL;
	INT32 xa, xb, ya, yb;
	short src_sw, src_sh;
	BoxRec dst;

	xa = src_x;
	xb = src_x + src_w;
	ya = src_y;
	yb = src_y + src_h;

	dst.x1 = drw_x;
	dst.x2 = drw_x + drw_w;
	dst.y1 = drw_y;
	dst.y2 = drw_y + drw_h;

	if (!xf86_crtc_clip_video_helper(pScrn, &crtc, drmxv->desired_crtc,
					 &dst, &xa, &xb, &ya, &yb, clipBoxes,
					 width, height))
		return BadAlloc;

	*crtcp = crtc;

	if (!crtc)
		return Success;

	src_sw = width * ((float)(dst.x2 - dst.x1) / drw_w);
	src_sh = height * ((float)(dst.y2 - dst.y1) / drw_h);

	/* Convert to coordinates on this CRTC. */
	drw_w = dst.x2 - dst.x1;
	drw_h = dst.y2 - dst.y1;
	drw_x = dst.x1 - crtc->x;
	drw_y = dst.y1 - crtc->y;

{
  static int dx, dy, dw, dh, sow, soh, ssw, ssh;
  if (dx != drw_x || dy != drw_y || dw != drw_w || dh != drw_h || sow != width || soh != height || ssw != src_sw || ssh != src_sh) {
	dx = drw_x; dy = drw_y; dw = drw_w; dh = drw_h; sow = width; soh = height; ssw = src_sw; ssh = src_sh;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "os[%dx%d] ss[%dx%d] dst[%d,%d %dx%d]\n",
	  sow, soh, ssw, ssh, dx, dy, dw, dh);
  }
}
	
	drmc = crtc->driver_private;

	drmxv->arg.src_width = width;
	drmxv->arg.src_height = height;
	drmxv->arg.src_scan_width = src_sw;
	drmxv->arg.src_scan_height = src_sh;
	drmxv->arg.crtc_id = drmc->mode_crtc->crtc_id;
	drmxv->arg.dst_x = drw_x;
	drmxv->arg.dst_y = drw_y;
	drmxv->arg.dst_width = drw_w;
	drmxv->arg.dst_height = drw_h;

	return Success;
}

static int armada_drm_ovl_PutImage(ScrnInfoPtr pScrn,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	int image, unsigned char *buf, short width, short height,
	Bool sync, RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	xf86CrtcPtr crtc = NULL;
	struct drm_xv *drmxv = data;
	struct drm_armada_bo *bo;
	int ret;

//    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//               "PutImage: s%dx%x+%d+%d d%dx%d+%d+%d image %x %dx%d sync %u\n",
//               src_w, src_h, src_x, src_y, drw_w, drw_h, drw_x, drw_y,
//               image, width, height, sync);

	ret = armada_drm_ovl_clip(pScrn, drmxv, src_x, src_y, drw_x, drw_y,
				src_w, src_h, drw_w, drw_h, clipBoxes,
				width, height, &crtc);
	if (ret != Success)
		return ret;

	if (!crtc) {
		armada_drm_ovl_StopVideo(pScrn, data, TRUE);
		return Success;
	}

	if (image == FOURCC_XVBO) {
		/* XVBO support allows applications to prepare the DRM buffer
		 * object themselves, and pass a global name to the X server to
		 * update the hardware with.  This is similar to Intel XvMC
		 * support, except we also allow the image format to be
		 * specified via a fourcc as the first word.
		 */
		uint32_t *p = (uint32_t *)buf;
		uint32_t fmt = p[0];
		uint32_t name = p[1];

		if (!drmxv->image || drmxv->image_fourcc != fmt ||
		    drmxv->image_width != width ||
		    drmxv->image_height != height) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "[drm] XVBO: fourcc %08x name %08x\n",
				   fmt, name);
			drmxv->is_bmm = 1;
			ret = armada_drm_ovl_SetupStd(drmxv, fmt, width, height);
			if (ret != Success) {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					   "[drm] XVBO: SetupStd failed: %d\n", ret);
				return ret;
			}
		}

		/*
		 * Convert the global name back to a buffer object, so we have
		 * a handle to pass to the kernel.
		 */
		bo = drm_armada_bo_create_from_name(drmxv->drm->bufmgr, name);
		if (!bo) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "[drm] XVBO: bo_create_from_name failed: %d\n", ret);
			return BadAlloc;
		}
	} else {
		if (!drmxv->image || drmxv->image_fourcc != image)
			drmxv->is_bmm = armada_drm_is_bmm(buf);

		if (!drmxv->image || drmxv->image_fourcc != image ||
			drmxv->image_width != width || drmxv->image_height != height) {
			ret = armada_drm_ovl_SetupStd(drmxv, image, width, height);
			if (ret != Success)
				return ret;

			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "[drm] bmm %u image fourcc %08x flags %08x\n",
				   drmxv->is_bmm, image, drmxv->arg.flags);
		}

		ret = drmxv->cvt(drmxv, buf, &bo);
		if (ret != Success)
			return ret;
	}

	drmxv->arg.bo_handle = bo->handle;

	ret = drmIoctl(drmxv->drm->fd, DRM_IOCTL_ARMADA_OVERLAY_PUT_IMAGE,
		       &drmxv->arg);

	/*
	 * The kernel will keep a reference internally to the buffer object
	 * while it is being displayed.
	 */
	drm_armada_bo_put(bo);

	/*
	 * Finally, fill the clip boxes; do this after we've done the ioctl
	 * so we don't impact on latency.
	 */
	if (drmxv->autopaint_colorkey &&
	    !RegionEqual(&drmxv->clipBoxes, clipBoxes)) {
		RegionCopy(&drmxv->clipBoxes, clipBoxes);

		xf86XVFillKeyHelper(pScrn->pScreen, drmxv->attrs.color_key, clipBoxes);
	}

	return Success;
}

static int armada_drm_ovl_ReputImage(ScrnInfoPtr pScrn,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	xf86CrtcPtr crtc = NULL;
	struct drm_xv *drmxv = data;
	int ret;

	if (!drmxv->image)
		return Success;

	ret = armada_drm_ovl_clip(pScrn, drmxv, src_x, src_y, drw_x, drw_y,
				src_w, src_h, drw_w, drw_h, clipBoxes,
				drmxv->image_width, drmxv->image_height, &crtc);
	if (ret)
		return ret;

	if (!crtc) {
		armada_drm_ovl_StopVideo(pScrn, data, TRUE);
		return Success;
	}

	ret = drmIoctl(drmxv->drm->fd, DRM_IOCTL_ARMADA_OVERLAY_PUT_IMAGE,
		       &drmxv->arg);

	if (drmxv->autopaint_colorkey &&
	    !RegionEqual(&drmxv->clipBoxes, clipBoxes)) {
		RegionCopy(&drmxv->clipBoxes, clipBoxes);

		xf86XVFillKeyHelper(pScrn->pScreen, drmxv->attrs.color_key, clipBoxes);
	}

	return Success;
}

static int armada_drm_ovl_QueryImageAttributes(ScrnInfoPtr pScrn,
	int image, unsigned short *width, unsigned short *height,
	int *pitches, int *offsets)
{
	const XF86ImageRec *img;
	unsigned ret = 0;

	*width = (*width + 1) & ~1;
	*height = (*height + 1) & ~1;

	img = armada_drm_ovl_get_img(image);
	if (img) {
		int pitch[3], offset[3];

		ret = armada_drm_ovl_set_img_info(img, pitch, offset,
						  *width, *height);

		if (ret) {
			if (pitches)
				memcpy(pitches, pitch, sizeof(*pitches) * img->num_planes);
			if (offsets)
				memcpy(offsets, offset, sizeof(*offsets) * img->num_planes);
		}
	}
	return ret;
}

Bool armada_drm_XvInit(ScrnInfoPtr pScrn)
{
	ScreenPtr scrn = screenInfo.screens[pScrn->scrnIndex];
	XF86VideoAdaptorPtr xv[1], p;
	struct armada_drm_info *drm = GET_DRM_INFO(pScrn);
	struct drm_xv *drmxv;
	DevUnion priv[1];
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

	drmxv = calloc(1, sizeof *drmxv);
	if (!drmxv)
		return FALSE;

	drmxv->drm = drm;
	drmxv->autopaint_colorkey = TRUE;

	drmIoctl(drm->fd, DRM_IOCTL_ARMADA_OVERLAY_ATTRS, &drmxv->attrs);

	p = xf86XVAllocateVideoAdaptorRec(pScrn);
	if (!p) {
		free(drmxv);
		return FALSE;
	}

	priv[0].ptr = drmxv;

	p->type = XvWindowMask | XvInputMask | XvImageMask;
	p->flags = VIDEO_OVERLAID_IMAGES;
	p->name = "Marvell Armada Overlay Video";
	p->nEncodings = sizeof(OverlayEncodings) / sizeof(XF86VideoEncodingRec);
	p->pEncodings = OverlayEncodings;
	p->nFormats = sizeof(OverlayFormats) / sizeof(XF86VideoFormatRec);
	p->pFormats = OverlayFormats;
	p->nPorts = 1;
	p->pPortPrivates = priv;
	p->nAttributes = sizeof(OverlayAttributes) / sizeof(XF86AttributeRec);
	p->pAttributes = OverlayAttributes;
	p->nImages = sizeof(OverlayImages) / sizeof(XF86ImageRec);
	p->pImages = OverlayImages;
	p->StopVideo = armada_drm_ovl_StopVideo;
	p->SetPortAttribute = armada_drm_ovl_SetPortAttribute;
	p->GetPortAttribute = armada_drm_ovl_GetPortAttribute;
	p->QueryBestSize = armada_drm_ovl_QueryBestSize;
	p->PutImage = armada_drm_ovl_PutImage;
	p->ReputImage = armada_drm_ovl_ReputImage;
	p->QueryImageAttributes = armada_drm_ovl_QueryImageAttributes;

	xv[0] = p;
	ret = xf86XVScreenInit(scrn, xv, 1);
	free(p);

	if (!ret)
		free(drmxv);

	return ret;
}
