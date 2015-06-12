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

#include "armada_accel.h"
#include "armada_drm.h"
#include "common_drm.h"
#include "drm_fourcc.h"
#include "xf86Crtc.h"
#include "xf86xv.h"
#include "fourcc.h"
#include "utils.h"
#include <X11/extensions/Xv.h>
#include <X11/Xatom.h>

#include "armada_fourcc.h"
#include "armada_ioctl.h"
#include "xv_attribute.h"
#include "xv_image_format.h"
#include "xvbo.h"

/* Size of physical addresses via BMM */
typedef uint32_t phys_t;
#define INVALID_PHYS	(~(phys_t)0)

#define NR_BUFS	3

enum armada_drm_properties {
	PROP_DRM_SATURATION,
	PROP_DRM_BRIGHTNESS,
	PROP_DRM_CONTRAST,
	PROP_DRM_COLORKEY,
	NR_DRM_PROPS
};

static const char *armada_drm_property_names[NR_DRM_PROPS] = {
	[PROP_DRM_SATURATION] = "saturation",
	[PROP_DRM_BRIGHTNESS] = "brightness",
	[PROP_DRM_CONTRAST] = "contrast",
	[PROP_DRM_COLORKEY] = "colorkey",
};

struct drm_xv {
	int fd;
	struct drm_armada_bufmgr *bufmgr;

	/* Common information */
	xf86CrtcPtr desired_crtc;
	Bool is_xvbo;
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
		uint32_t fb_id;
	} bufs[NR_BUFS];

	struct drm_armada_bo *last_bo;

	int (*get_fb)(ScrnInfoPtr, struct drm_xv *, unsigned char *,
		uint32_t *);

	/* Plane information */
	const struct xv_image_format *plane_format;
	uint32_t plane_fb_id;
	drmModePlanePtr plane;
	drmModePlanePtr planes[2];
	drmModePropertyPtr props[NR_DRM_PROPS];
	uint64_t prop_values[NR_DRM_PROPS];
};

enum {
	attr_encoding,
	attr_saturation,
	attr_brightness,
	attr_contrast,
	attr_autopaint_colorkey,
	attr_colorkey,
	attr_pipe,
	attr_deinterlace,
};

static struct xv_attr_data armada_drm_xv_attributes[];

/*
 * Attribute support code
 */
static int armada_drm_prop_set(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;
	uint32_t prop_id;
	unsigned i;

	if (drmxv->props[attr->id] == NULL)
		return Success; /* Actually BadMatch... */

	drmxv->prop_values[attr->id] = value;

	prop_id = drmxv->props[attr->id]->prop_id;

	for (i = 0; i < ARRAY_SIZE(drmxv->planes); i++) {
		if (!drmxv->planes[i])
			continue;

		drmModeObjectSetProperty(drmxv->fd,
					 drmxv->planes[i]->plane_id,
					 DRM_MODE_OBJECT_PLANE, prop_id,
					 value);
	}
	return Success;
}

static int armada_drm_prop_get(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	*value = drmxv->prop_values[attr->id];
	return Success;
}

static int armada_drm_set_colorkey(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	RegionEmpty(&drmxv->clipBoxes);

	return armada_drm_prop_set(pScrn, attr, value, data);
}

static int armada_drm_set_autopaint(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	drmxv->autopaint_colorkey = !!value;
	if (value != 0) {
		RegionEmpty(&drmxv->clipBoxes);
		return Success;
	}

	attr = &armada_drm_xv_attributes[attr_colorkey];

	/*
	 * If autopainting of the colorkey is disabled, should we
	 * zero the colorkey?  For the time being, we do.
	 */
	return attr->set(pScrn, attr, 0, data);
}

static int armada_drm_get_autopaint(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	*value = drmxv->autopaint_colorkey;
	return Success;
}

static int armada_drm_set_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

	if (value < -1 || value >= config->num_crtc)
		return BadValue;
	if (value == -1)
		drmxv->desired_crtc = NULL;
	else
		drmxv->desired_crtc = config->crtc[value];
	return Success;
}

static int armada_drm_get_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	unsigned i;

	*value = -1;

	for (i = 0; i < config->num_crtc; i++)
		if (config->crtc[i] == drmxv->desired_crtc) {
			*value = i;
			break;
		}

	return Success;
}

static int armada_drm_set_ignore(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	return Success;
}

static int armada_drm_get_ignore(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	*value = attr->id;
	return Success;
}

/*
 * This must match the strings and order in the table above
 *
 * XvSetPortAttribute(3) suggests that XV_BRIGHTNESS, XV_CONTRAST, XV_HUE
 * and XV_SATURATION should all be in the range of -1000 ... 1000.  It
 * seems not many drivers follow that requirement.
 */
static XF86AttributeRec OverlayAttributes[] = {
	{ XvSettable | XvGettable, 0,      0,          "XV_ENCODING" },
	{ XvSettable | XvGettable, -16384, 16383,      "XV_SATURATION" },
	{ XvSettable | XvGettable, -256,   255,        "XV_BRIGHTNESS" },
	{ XvSettable | XvGettable, -16384, 16383,      "XV_CONTRAST" },
	{ XvSettable | XvGettable, 0,      1,          "XV_AUTOPAINT_COLORKEY"},
	{ XvSettable | XvGettable, 0,      0x00ffffff, "XV_COLORKEY" },
	{ XvSettable | XvGettable, -1,     2,          "XV_PIPE" },
/*	{ XvSettable | XvGettable, 0,      0,          "XV_DEINTERLACE" }, */
};

static struct xv_attr_data armada_drm_xv_attributes[] = {
	[attr_encoding] = {
		.name = "XV_ENCODING",
		.set = armada_drm_set_ignore,
		.get = armada_drm_get_ignore,
		.attr = &OverlayAttributes[attr_encoding],
	},
	[attr_saturation] = {
		.name = "XV_SATURATION",
		.id = PROP_DRM_SATURATION,
		.offset = 16384,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_saturation],
	},
	[attr_brightness] = {
		.name = "XV_BRIGHTNESS",
		.id = PROP_DRM_BRIGHTNESS,
		.offset = 256,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_brightness],
	},
	[attr_contrast] = {
		.name = "XV_CONTRAST",
		.id = PROP_DRM_CONTRAST,
		.offset = 16384,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_contrast],
	},
	[attr_autopaint_colorkey] = {
		.name = "XV_AUTOPAINT_COLORKEY",
		.set = armada_drm_set_autopaint,
		.get = armada_drm_get_autopaint,
		.attr = &OverlayAttributes[attr_autopaint_colorkey],
	},
	[attr_colorkey] = {
		.name = "XV_COLORKEY",
		.id = PROP_DRM_COLORKEY,
		.set = armada_drm_set_colorkey,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_colorkey],
	},
	[attr_pipe] = {
		.name = "XV_PIPE",
		.set = armada_drm_set_pipe,
		.get = armada_drm_get_pipe,
		.attr = &OverlayAttributes[attr_pipe],
	},
	/*
	 * We could stop gst-plugins-bmmxv complaining, but arguably
	 * it is a bug in that code which _assumes_ that this atom
	 * exists.  Hence, this code is commented out.
	[attr_deinterlace] = {
		.name = "XV_DEINTERLACE",
		.set = armada_drm_set_ignore,
		.get = armada_drm_get_ignore,
		.attr = &OverlayAttributes[attr_deinterlace],
	},
	 */
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


/*
 * These are in order of preference.  The I420/YV12 formats require
 * conversion within the X server rather than the application, that's
 * relatively easy to do, and moreover involves reading less data than
 * I422/YV16.  YV16 and VYUY are not common formats (vlc at least does
 * not have any support for it but does have I422) so these comes at
 * the very end, to try to avoid vlc complaining about them.
 */
static const struct xv_image_format armada_drm_formats[] = {
	/* Standard Xv formats */
	{
		.u.drm_format = DRM_FORMAT_UYVY,
		.xv_image = XVIMAGE_UYVY,
	}, {
		.u.drm_format = DRM_FORMAT_YUYV,
		.xv_image = XVIMAGE_YUY2,
	}, {
		.u.drm_format = DRM_FORMAT_YUV420,
		.xv_image = XVIMAGE_I420,
	}, {
		.u.drm_format = DRM_FORMAT_YVU420,
		.xv_image = XVIMAGE_YV12,
	}, {
	/* Our own formats */
		.u.drm_format = DRM_FORMAT_YUV422,
		.xv_image = XVIMAGE_I422,
	}, {
		.u.drm_format = DRM_FORMAT_YVU422,
		.xv_image = XVIMAGE_YV16,
	}, {
		.u.drm_format = DRM_FORMAT_VYUY,
		.xv_image = XVIMAGE_VYUY,
	}, {
		.u.drm_format = DRM_FORMAT_ARGB8888,
		.xv_image = XVIMAGE_ARGB8888,
	}, {
		.u.drm_format = DRM_FORMAT_ABGR8888,
		.xv_image = XVIMAGE_ABGR8888,
	}, {
		.u.drm_format = DRM_FORMAT_XRGB8888,
		.xv_image = XVIMAGE_XRGB8888,
	}, {
		.u.drm_format = DRM_FORMAT_XBGR8888,
		.xv_image = XVIMAGE_XBGR8888,
	}, {
		.u.drm_format = DRM_FORMAT_RGB888,
		.xv_image = XVIMAGE_RGB888,
	}, {
		.u.drm_format = DRM_FORMAT_BGR888,
		.xv_image = XVIMAGE_BGR888,
	}, {
		.u.drm_format = DRM_FORMAT_ARGB1555,
		.xv_image = XVIMAGE_ARGB1555,
	}, {
		.u.drm_format = DRM_FORMAT_ABGR1555,
		.xv_image = XVIMAGE_ABGR1555,
	}, {
		.u.drm_format = DRM_FORMAT_RGB565,
		.xv_image = XVIMAGE_RGB565
	}, {
		.u.drm_format = DRM_FORMAT_BGR565,
		.xv_image = XVIMAGE_BGR565
	}, {
		.u.drm_format = 0,
		.xv_image = 		XVIMAGE_XVBO
	},
};

/* It would be nice to be given the image pointer... */
static const struct xv_image_format *armada_drm_lookup_xvfourcc(int fmt)
{
	return xv_image_xvfourcc(armada_drm_formats,
				 ARRAY_SIZE(armada_drm_formats), fmt);
}

static const struct xv_image_format *armada_drm_lookup_drmfourcc(uint32_t fmt)
{
	return xv_image_drm(armada_drm_formats,
			    ARRAY_SIZE(armada_drm_formats), fmt);
}

static int
armada_drm_get_fmt_info(const struct xv_image_format *fmt,
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
			drmModeRmFB(drmxv->fd, drmxv->bufs[i].fb_id);
			drmxv->bufs[i].fb_id = 0;
		}
		if (drmxv->bufs[i].bo) {
			drm_armada_bo_put(drmxv->bufs[i].bo);
			drmxv->bufs[i].bo = NULL;
		}
	}

	if (drmxv->plane_fb_id) {
		drmModeRmFB(drmxv->fd, drmxv->plane_fb_id);
		drmxv->plane_fb_id = 0;
	}

	if (drmxv->last_bo) {
		drm_armada_bo_put(drmxv->last_bo);
		drmxv->last_bo = NULL;
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
	if (drmModeAddFB2(drmxv->fd, drmxv->width, drmxv->height,
			  drmxv->plane_format->u.drm_format, handles,
			  drmxv->pitches, drmxv->offsets, id, 0))
		return FALSE;

	return TRUE;
}

static int armada_drm_bufs_alloc(struct drm_xv *drmxv)
{
	struct drm_armada_bufmgr *bufmgr = drmxv->bufmgr;
	uint32_t width = drmxv->width;
	uint32_t height = drmxv->image_size / width / 2;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++) {
		struct drm_armada_bo *bo;

		bo = drm_armada_bo_dumb_create(bufmgr, width, height, 16);
		if (!bo) {
			armada_drm_bufs_free(drmxv);
			return BadAlloc;
		}

		drmxv->bufs[i].bo = bo;
		if (drm_armada_bo_map(bo) ||
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

static int
armada_drm_get_xvbo(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *buf,
	uint32_t *id)
{
	struct drm_armada_bo *bo;
	uint32_t name = ((uint32_t *)buf)[1];

	/* Lookup the bo for the global name */
	bo = drm_armada_bo_create_from_name(drmxv->bufmgr, name);
	if (!bo)
		return BadAlloc;

	/* Is this a re-display of the previous frame? */
	if (drmxv->last_bo == bo) {
		drm_armada_bo_put(bo);
		*id = drmxv->plane_fb_id;
		return Success;
	}

	if (!armada_drm_create_fbid(drmxv, bo, id)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"[drm] XVBO: drmModeAddFB2 failed: %s\n",
			strerror(errno));
		return BadAlloc;
	}

	/* Now replace the last bo with the current bo */
	if (drmxv->last_bo)
		drm_armada_bo_put(drmxv->last_bo);

	drmxv->last_bo = bo;

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
	return xv_attr_SetPortAttribute(armada_drm_xv_attributes,
		ARRAY_SIZE(armada_drm_xv_attributes),
		pScrn, attribute, value, data);
}

static int
armada_drm_Xv_GetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data)
{
	return xv_attr_GetPortAttribute(armada_drm_xv_attributes,
		ARRAY_SIZE(armada_drm_xv_attributes),
		pScrn, attribute, value, data);
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
	const struct xv_image_format *fmt;
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
	const struct xv_image_format *fmt;
	Bool is_xvbo = image == FOURCC_XVBO;
	int ret;

	if (is_xvbo)
		/*
		 * XVBO support allows applications to prepare the DRM
		 * buffer object themselves, and pass a global name to
		 * the X server to update the hardware with.  This is
		 * similar to Intel XvMC support, except we also allow
		 * the image format to be specified via a fourcc as the
		 * first word.
		 */
		image = ((uint32_t *)buf)[0];
	else if (armada_drm_is_bmm(buf))
		/*
		 * We no longer handle the old Marvell BMM buffer
		 * passing protocol
		 */
		return BadAlloc;

	if (drmxv->width != width || drmxv->height != height ||
	    drmxv->fourcc != image || !drmxv->plane_format) {
		uint32_t size;

		/* format or size changed */
		fmt = armada_drm_lookup_xvfourcc(image);
		if (!fmt)
			return BadMatch;

		/* Check whether this is XVBO mapping */
		if (is_xvbo) {
			drmxv->is_xvbo = TRUE;
			drmxv->get_fb = armada_drm_get_xvbo;
		} else {
			drmxv->is_xvbo = FALSE;
			drmxv->get_fb = armada_drm_get_std;
		}

		armada_drm_bufs_free(drmxv);

		size = armada_drm_get_fmt_info(fmt, drmxv->pitches,
					       drmxv->offsets, width, height);

		drmxv->plane_format = fmt;
		drmxv->image_size = size;
		drmxv->width = width;
		drmxv->height = height;
		drmxv->fourcc = image;

//		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//			   "[drm] xvbo %u fourcc %08x\n",
//			   is_xvbo, image);

		/* Pre-allocate the buffers if we aren't using XVBO or BMM */
		if (!drmxv->is_xvbo) {
			int ret = armada_drm_bufs_alloc(drmxv);
			if (ret != Success) {
				drmxv->plane_format = NULL;
				return ret;
			}
		}

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

		ret = drmModeSetPlane(drmxv->fd, drmxv->plane->plane_id,
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

	crtc_mask = 1 << common_crtc(crtc)->num;

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
	drmModeSetPlane(drmxv->fd, plane->plane_id,
			common_crtc(crtc)->mode_crtc->crtc_id, fb_id, 0,
			crtc_x, crtc_y, dst->x2 - dst->x1, dst->y2 - dst->y1,
			x1, y1, x2 - x1, y2 - y1);

	/*
	 * Finally, fill the clip boxes; do this after we've done the ioctl
	 * so we don't impact on latency.
	 */
	if (drmxv->autopaint_colorkey &&
	    !RegionEqual(&drmxv->clipBoxes, clipBoxes)) {
		RegionCopy(&drmxv->clipBoxes, clipBoxes);
		xf86XVFillKeyHelper(pScrn->pScreen,
				    drmxv->prop_values[PROP_DRM_COLORKEY],
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
	if (drmxv->is_xvbo &&
	    drmxv->plane_fb_id && drmxv->plane_fb_id != fb_id) {
		drmModeRmFB(drmxv->fd, drmxv->plane_fb_id);
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

	images = calloc(drmxv->planes[0]->count_formats + 1, sizeof(*images));
	if (!images) {
		free(p);
		return NULL;
	}

	for (num_images = i = 0; i < drmxv->planes[0]->count_formats; i++) {
		const struct xv_image_format *fmt;
		uint32_t id = drmxv->planes[0]->formats[i];

		if (id == 0)
			continue;

		fmt = armada_drm_lookup_drmfourcc(id);
		if (fmt)
			images[num_images++] = fmt->xv_image;
	}

	images[num_images++] = (XF86ImageRec)XVIMAGE_XVBO;

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

static Bool armada_drm_init_atoms(ScrnInfoPtr pScrn)
{
	unsigned i;
	Bool mismatch = FALSE;

	if (armada_drm_xv_attributes[0].x_atom)
		return TRUE;

	if (!xv_attr_init(armada_drm_xv_attributes,
			  ARRAY_SIZE(armada_drm_xv_attributes)))
		return FALSE;

	for (i = 0; i < ARRAY_SIZE(armada_drm_xv_attributes); i++) {
		struct xv_attr_data *d = &armada_drm_xv_attributes[i];

		/*
		 * We could generate the overlay attributes from
		 * our own attribute information, which would
		 * eliminate the need for this check.
		 */
		if (strcmp(d->name, OverlayAttributes[i].name)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Overlay attribute %u mismatch\n", i);
			mismatch = TRUE;
		}

		/*
		 * XV_PIPE needs to be initialized with the number
		 * of CRTCs, which is not known at build time.
		 */
		if (strcmp(d->name, "XV_PIPE") == 0) {
			xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
			OverlayAttributes[i].max_value = config->num_crtc - 1;
		}
	}

	/* If we encounter a mismatch, error out */
	return !mismatch;
}

Bool armada_drm_XvInit(ScrnInfoPtr pScrn)
{
	ScreenPtr scrn = screenInfo.screens[pScrn->scrnIndex];
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	XF86VideoAdaptorPtr xv[2], plane, gpu_adap;
	drmModePlaneResPtr res;
	struct drm_xv *drmxv;
	DevUnion priv[1];
	unsigned i, num;
	Bool ret, prefer_overlay;

	if (!armada_drm_init_atoms(pScrn))
		return FALSE;

	/* Initialise the GPU textured adapter first. */
	if (arm->accel_ops && arm->accel_ops->xv_init)
		gpu_adap = arm->accel_ops->xv_init(scrn);
	else
		gpu_adap = NULL;

	/* FIXME: we leak this */
	drmxv = calloc(1, sizeof *drmxv);
	if (!drmxv)
		return FALSE;

	drmxv->fd = drm->fd;
	drmxv->bufmgr = arm->bufmgr;
	drmxv->autopaint_colorkey = TRUE;

	/* Get the plane resources and the overlay planes */
	res = drmModeGetPlaneResources(drmxv->fd);
	if (!res)
		goto err_free;

	/* Get all plane information */
	for (i = 0; i < res->count_planes && i < ARRAY_SIZE(drmxv->planes); i++) {
		drmModeObjectPropertiesPtr props;
		uint32_t plane_id = res->planes[i];
		unsigned j;

		drmxv->planes[i] = drmModeGetPlane(drmxv->fd, plane_id);
		props = drmModeObjectGetProperties(drmxv->fd, plane_id,
						   DRM_MODE_OBJECT_PLANE);
		if (!drmxv->planes[i] || !props) {
			drmModeFreePlaneResources(res);
			goto err_free;
		}

		for (j = 0; j < props->count_props; j++) {
			drmModePropertyPtr prop;
			unsigned k;

			prop = drmModeGetProperty(drmxv->fd, props->props[j]);
			if (!prop)
				continue;

			for (k = 0; k < NR_DRM_PROPS; k++) {
				const char *name = armada_drm_property_names[k];
				if (drmxv->props[k])
					continue;

				if (strcmp(prop->name, name) == 0) {
					drmxv->props[k] = prop;
					drmxv->prop_values[k] = props->prop_values[j];
					prop = NULL;
					break;
				}
			}

			if (prop)
				drmModeFreeProperty(prop);
		}
		drmModeFreeObjectProperties(props);
	}

	/* Done with the plane resources */
	drmModeFreePlaneResources(res);

	prefer_overlay = xf86ReturnOptValBool(arm->Options,
					      OPTION_XV_PREFEROVL, TRUE);

	num = 0;
	if (gpu_adap && !prefer_overlay)
		xv[num++] = gpu_adap;

	if (drmxv->planes[0]) {
		priv[0].ptr = drmxv;
		plane = armada_drm_XvInitPlane(pScrn, priv, drmxv);
		if (!plane)
			goto err_free;
		xv[num++] = plane;
	}

	if (gpu_adap && prefer_overlay)
		xv[num++] = gpu_adap;

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
	for (i = 0; i < ARRAY_SIZE(drmxv->planes); i++)
		if (drmxv->planes[i])
			drmModeFreePlane(drmxv->planes[i]);
	if (gpu_adap) {
		free(gpu_adap->pImages);
		free(gpu_adap->pPortPrivates);
		free(gpu_adap);
	}
	free(drmxv);
	return FALSE;
}
