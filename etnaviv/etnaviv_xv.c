/*
 * Etnaviv Xv Textured overlay adapter
 *
 * Things discovered about filter blit in VR mode:
 *  - does not use CLIP_TOP_LEFT/CLIP_BOTTOM_RIGHT.
 *  - does not use SRC_ORIGIN/SRC_SIZE.
 *  - does not use SRC_ORIGIN_FRACTION.
 *
 * Todo:
 *  - sync with display (using drmWaitVBlank?)
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/mman.h>

#include "xf86.h"
#include "xf86Crtc.h"
#include "xf86xv.h"
#include "damage.h"
#include "fourcc.h"
#include <X11/extensions/Xv.h>

#include "compat-api.h"
#include "pixmaputil.h"
#include "utils.h"
#include "xv_attribute.h"
#include "xv_image_format.h"
#include "xvbo.h"

#include "armada_accel.h"
#include "common_drm_helper.h"

#include "etnaviv_accel.h"
#include "etnaviv_op.h"
#include "etnaviv_utils.h"
#include "etnaviv_xv.h"

#include "etnaviv/etna_bo.h"
#include "etnaviv/state_2d.xml.h"

/*
 * The Vivante GPU supports up to 32k x 32k, but that would be
 * 2GB in 16bpp.  Limit to 4k x 4k, which gives us 32M.
 */
#define ETNAVIV_XV_MAX_WIDTH  4096
#define ETNAVIV_XV_MAX_HEIGHT 4096

static XF86VideoEncodingRec etnaviv_encodings[] = {
	{
		.id = 0,
		.name = "XV_IMAGE",
		.width = ETNAVIV_XV_MAX_WIDTH,
		.height = ETNAVIV_XV_MAX_HEIGHT,
		.rate = {
			.numerator = 1,
			.denominator = 1,
		},
	},
};

static XF86VideoFormatRec etnaviv_formats[] = {
	{
		.depth = 15,
		.class = TrueColor,
	}, {
		.depth = 16,
		.class = TrueColor,
	}, {
		.depth = 24,
		.class = TrueColor,
	},
};

static const struct etnaviv_format fmt_uyvy = {
	.format = DE_FORMAT_UYVY,
	.swizzle = DE_SWIZZLE_ARGB,
	.planes = 1,
};

static const struct etnaviv_format fmt_yuy2 = {
	.format = DE_FORMAT_YUY2,
	.swizzle = DE_SWIZZLE_ARGB,
	.planes = 1,
};

static const struct etnaviv_format fmt_yv12 = {
	.format = DE_FORMAT_YV12,
	.swizzle = DE_SWIZZLE_ARGB,
	.planes = 3,
	.u = 2,
	.v = 1,
};

static const struct etnaviv_format fmt_i420 = {
	.format = DE_FORMAT_YV12,
	.swizzle = DE_SWIZZLE_ARGB,
	.planes = 3,
	.u = 1,
	.v = 2,
};

static const struct xv_image_format etnaviv_image_formats[] = {
	{
		.u.data = &fmt_uyvy,
		.xv_image = XVIMAGE_UYVY,
	}, {
		.u.data = &fmt_yuy2,
		.xv_image = XVIMAGE_YUY2,
	}, {
		.u.data = &fmt_yv12,
		.xv_image = XVIMAGE_YV12,
	}, {
		.u.data = &fmt_i420,
		.xv_image = XVIMAGE_I420,
	}, {
		.u.data = NULL,
		.xv_image = XVIMAGE_XVBO,
	},
};

#define KERNEL_ROWS	17
#define KERNEL_INDICES	9
#define KERNEL_SIZE	(KERNEL_ROWS * KERNEL_INDICES)
#define KERNEL_STATE_SZ	((KERNEL_SIZE + 1) / 2)

static uint32_t xv_filter_kernel[KERNEL_STATE_SZ];

enum {
	attr_sync_to_vblank,
	attr_last_prop,
	attr_pipe = attr_last_prop,
	attr_encoding,
};

struct etnaviv_xv_priv {
	struct etnaviv *etnaviv;
	xf86CrtcPtr desired_crtc;

	unsigned short width;
	unsigned short height;
	int fourcc;
	const struct xv_image_format *fmt;
	uint32_t pitches[3];
	uint32_t offsets[3];
	size_t size;

	struct etnaviv_format source_format;
	struct etnaviv_format stage1_format;
	uint32_t stage1_pitch;
	size_t stage1_size;
	struct etna_bo *stage1_bo;

	INT32 props[attr_last_prop];
};

static XF86AttributeRec etnaviv_xv_attributes[] = {
	[attr_encoding] = {
		.flags = XvSettable | XvGettable,
		.min_value = 0,
		.max_value = 0,
		.name = "XV_ENCODING",
	},
	[attr_pipe] = {
		.flags = XvSettable | XvGettable,
		.min_value = -1,
		.max_value = 0,
		.name = "XV_PIPE"
	},
	[attr_sync_to_vblank] = {
		.flags = XvSettable | XvGettable,
		.min_value = 0,
		.max_value = 1,
		.name = "XV_SYNC_TO_VBLANK",
	},
};

static int etnaviv_xv_set_encoding(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	if (value != etnaviv_encodings[0].id)
		return XvBadEncoding;
	return Success;
}

static int etnaviv_xv_get_encoding(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	*value = etnaviv_encodings[0].id;
	return Success;
}

static int etnaviv_xv_set_prop(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct etnaviv_xv_priv *priv = data;

	priv->props[attr->id] = value;

	return Success;
}

static int etnaviv_xv_get_prop(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct etnaviv_xv_priv *priv = data;

	*value = priv->props[attr->id];

	return Success;
}

static int etnaviv_xv_set_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct etnaviv_xv_priv *priv = data;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

	if (value < -1 || value >= config->num_crtc)
		return BadValue;
	priv->desired_crtc = value == -1 ? NULL : config->crtc[value];
	return Success;
}

static int etnaviv_xv_get_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct etnaviv_xv_priv *priv = data;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	unsigned i;

	*value = -1;

	for (i = 0; i < config->num_crtc; i++)
		if (config->crtc[i] == priv->desired_crtc) {
			*value = i;
			break;
		}

	return Success;
}

static struct xv_attr_data etnaviv_attributes[] = {
	[attr_encoding] = {
		.set = etnaviv_xv_set_encoding,
		.get = etnaviv_xv_get_encoding,
		.attr = &etnaviv_xv_attributes[attr_encoding],
	},
	[attr_pipe] = {
		.set = etnaviv_xv_set_pipe,
		.get = etnaviv_xv_get_pipe,
		.attr = &etnaviv_xv_attributes[attr_pipe],
	},
	[attr_sync_to_vblank] = {
		.id = attr_sync_to_vblank,
		.set = etnaviv_xv_set_prop,
		.get = etnaviv_xv_get_prop,
		.attr = &etnaviv_xv_attributes[attr_sync_to_vblank],
	},
};

static const struct xv_image_format *etnaviv_get_fmt_xv(int id)
{
	return xv_image_xvfourcc(etnaviv_image_formats,
				 ARRAY_SIZE(etnaviv_image_formats), id);
}

static int etnaviv_get_fmt_info(const struct xv_image_format *fmt,
	uint32_t *pitch, uint32_t *offset, unsigned width, unsigned height)
{
	uint32_t size[3];
	int ret;

	if (fmt->xv_image.id == FOURCC_XVBO) {
		/* Our special XVBO format is only two uint32_t */
		pitch[0] = 2 * sizeof(uint32_t);
		offset[0] = 0;
		ret = pitch[0];
	} else if (fmt->xv_image.format == XvPlanar) {
		unsigned y = 0, u, v;

		if (fmt->xv_image.component_order[1] == 'V')
			v = 1, u = 2;
		else
			v = 2, u = 1;

		/*
		 * Alignment requirements seem rather odd.  Some suggest
		 * that 16 byte alignment is required for the pitches,
		 * but this causes problems with at least VLC, and probably
		 * gstreamer 0.10.  Dropping this to 8 for the U and V
		 * planes appears to work fine, at least on GC320 v5.0.0.7
		 * and GC600 0.0.1.9
		 */
		pitch[y] = ALIGN(width / fmt->xv_image.horz_y_period, 16);
		pitch[u] = ALIGN(width / fmt->xv_image.horz_u_period, 8);
		pitch[v] = ALIGN(width / fmt->xv_image.horz_v_period, 8);

		size[y] = pitch[y] * (height / fmt->xv_image.vert_y_period);
		size[u] = pitch[u] * (height / fmt->xv_image.vert_u_period);
		size[v] = pitch[v] * (height / fmt->xv_image.vert_v_period);

		offset[0] = 0;
		offset[1] = ALIGN(offset[0] + size[0], 64);
		offset[2] = ALIGN(offset[1] + size[1], 64);

		ret = size[0] + size[1] + size[2];
	} else if (fmt->xv_image.format == XvPacked) {
		offset[0] = 0;
		pitch[0] = etnaviv_pitch(width, fmt->xv_image.bits_per_pixel);
		ret = offset[0] + pitch[0] * height;
	} else {
		ret = 0;
	}

	/* Align size to page size so buffers can be mapped */
	return ALIGN(ret, getpagesize());
}

static Bool etnaviv_realloc_stage1(ScrnInfoPtr pScrn,
	struct etnaviv_xv_priv *priv, size_t size)
{
	struct etnaviv *etnaviv = priv->etnaviv;

	if (priv->stage1_bo)
		etna_bo_del(etnaviv->conn, priv->stage1_bo, NULL);

	/*
	 * We don't need this bo mapped into this process at all, but
	 * etnaviv and galcore gives us no option.
	 */
	priv->stage1_bo = etna_bo_new(etnaviv->conn, size,
				      DRM_ETNA_GEM_TYPE_BMP |
				      DRM_ETNA_GEM_CACHE_WBACK);
	if (!priv->stage1_bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "etnaviv Xv: etna_bo_new(size=%zu) failed\n", size);
		priv->stage1_size = 0;
		return FALSE;
	}

	priv->stage1_size = size;
	return TRUE;
}

static void etnaviv_del_stage1(struct etnaviv_xv_priv *priv)
{
	struct etnaviv *etnaviv = priv->etnaviv;

	if (priv->stage1_bo) {
		etna_bo_del(etnaviv->conn, priv->stage1_bo, NULL);
		priv->stage1_bo = NULL;
		priv->stage1_size = 0;
	}
}

static void etnaviv_StopVideo(ScrnInfoPtr pScrn, pointer data, Bool shutdown)
{
	struct etnaviv_xv_priv *priv = data;

	if (shutdown) {
		etnaviv_del_stage1(priv);
		priv->fmt = NULL;
	}
}

static int etnaviv_SetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 value, pointer data)
{
	return xv_attr_SetPortAttribute(etnaviv_attributes,
		ARRAY_SIZE(etnaviv_attributes), pScrn, attribute, value, data);
}

static int etnaviv_GetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data)
{
	return xv_attr_GetPortAttribute(etnaviv_attributes,
		ARRAY_SIZE(etnaviv_attributes), pScrn, attribute, value, data);
}

static void etnaviv_QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
	short vid_w, short vid_h, short drw_w, short drw_h,
	unsigned int *p_w, unsigned int *p_h, pointer data)
{
	*p_w = maxt(vid_w, drw_w);
	*p_h = maxt(vid_h, drw_h);
}

static int etnaviv_configure_format(struct etnaviv_xv_priv *priv,
	short width, short height, int id, DrawablePtr drawable,
	struct etnaviv_pixmap *vPix)
{
	struct etnaviv *etnaviv = priv->etnaviv;
	const struct xv_image_format *fmt;

	fmt = etnaviv_get_fmt_xv(id);
	if (!fmt)
		return BadMatch;

	priv->size = etnaviv_get_fmt_info(fmt, priv->pitches, priv->offsets,
					  width, height);
	priv->width = width;
	priv->height = height;
	priv->fourcc = id;
	priv->fmt = fmt;

	priv->source_format = *(const struct etnaviv_format *)fmt->u.data;

	/* Setup the stage 1 (vertical blit) pitch and format */
	if (fmt->xv_image.type != XvYUV) {
		unsigned bpp;

		/*
		 * If the target has more bits per pixel, use that
		 * as the intermediate format.  Otherwise, use the
		 * source format.
		 */
		if (drawable->bitsPerPixel > fmt->xv_image.bits_per_pixel) {
			priv->stage1_format = vPix->format;
			bpp = drawable->bitsPerPixel;
		} else {
			priv->stage1_format = priv->source_format;
			bpp = fmt->xv_image.bits_per_pixel;
		}
		priv->stage1_format.tile = 1;
		priv->stage1_pitch = etnaviv_tile_pitch(width, bpp);
	} else if (VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20)) {
		priv->stage1_format = fmt_yuy2;
		priv->stage1_pitch = etnaviv_pitch(width, 16);
	} else {
		priv->stage1_format = vPix->format;
		priv->stage1_pitch = etnaviv_pitch(width,
						   drawable->bitsPerPixel);
	}

	return Success;
}

static int etnaviv_PutImage(ScrnInfoPtr pScrn,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	int id, unsigned char *buf, short width, short height,
	Bool sync, RegionPtr clipBoxes, pointer data, DrawablePtr drawable)
{
	struct etnaviv_xv_priv *priv = data;
	struct etnaviv *etnaviv = priv->etnaviv;
	struct etnaviv_vr_op op;
	struct etnaviv_pixmap *vPix;
	struct etna_bo *usr;
	drmVBlank vbl;
	xf86CrtcPtr crtc;
	BoxRec dst;
	xPoint dst_offset;
	INT32 x1, x2, y1, y2;
	Bool is_xvbo = id == FOURCC_XVBO;
	int s_w, s_h, xoff;

	dst.x1 = drw_x;
	dst.y1 = drw_y;
	dst.x2 = drw_x + drw_w;
	dst.y2 = drw_y + drw_h;

	x1 = src_x;
	x2 = src_x + src_w;
	y1 = src_y;
	y2 = src_y + src_h;

	vPix = etnaviv_drawable_offset(drawable, &dst_offset);
	if (!vPix)
		return BadMatch;

	if (!etnaviv_map_gpu(etnaviv, vPix, GPU_ACCESS_RW))
		return BadMatch;

	if (is_xvbo)
		/*
		 * XVBO support allows applications to prepare the DRM
		 * buffer object themselves, and pass a global name to
		 * the X server to update the hardware with.  This is
		 * similar to Intel XvMC support, except we also allow
		 * the image format to be specified via a fourcc as the
		 * first word.
		 */
		id = ((uint32_t *)buf)[0];

	/* If the format or size has changed, recalculate */
	if (priv->width != width || priv->height != height ||
	    priv->fourcc != id || !priv->fmt) {
		int ret;

		ret = etnaviv_configure_format(priv, width, height, id,
					       drawable, vPix);
		if (ret != Success)
			return ret;
	}

	if (!xf86_crtc_clip_video_helper(pScrn, &crtc, priv->desired_crtc,
					 &dst, &x1, &x2, &y1, &y2, clipBoxes,
					 width, height))
		return BadAlloc;

	/* Read the last vblank time */
	if (crtc) {
		if (common_drm_vblank_get(pScrn, crtc, &vbl, __FUNCTION__))
			crtc = NULL;
	}

	if (is_xvbo) {
		uint32_t name = ((uint32_t *)buf)[1];

		usr = etna_bo_from_name(etnaviv->conn, name);
		if (!usr)
			return BadAlloc;

		if (etna_bo_size(usr) < priv->size) {
			etna_bo_del(etnaviv->conn, usr, NULL);
			return BadAlloc;
		}

		xoff = 0;
	} else {
		/* The GPU alignment offset of the buffer. */
		xoff = (uintptr_t)buf & 63;

		usr = etna_bo_from_usermem_prot(etnaviv->conn, buf - xoff,
						priv->size + xoff, PROT_READ);
		if (!usr)
			return BadAlloc;

		xoff = (xoff >> 1) << 16;
	}

	op.src = INIT_BLIT_BO(usr, 0, priv->source_format, ZERO_OFFSET);
	op.src_pitches = priv->pitches;
	op.src_offsets = priv->offsets;
	op.src_bounds.x1 = xoff >> 16;
	op.src_bounds.y1 = 0;
	op.src_bounds.x2 = op.src_bounds.x1 + width;
	op.src_bounds.y2 = height;

	etna_set_state_multi(etnaviv->ctx, VIVS_DE_FILTER_KERNEL(0), KERNEL_STATE_SZ,
			     xv_filter_kernel);

	/*
	 * The resulting width/height of the source/destination
	 * after clipping etc.
	 */
	s_w = x2 - x1;
	s_h = y2 - y1;
	drw_w = dst.x2 - dst.x1;
	drw_h = dst.y2 - dst.y1;

	/* Check whether we need to scale in the vertical direction first. */
	if (s_h != drw_h << 16) {
		size_t stage1_size = priv->stage1_pitch;
		BoxRec box;

		if (priv->stage1_format.tile)
			stage1_size *= etnaviv_tile_height(drw_h);
		else
			stage1_size *= drw_h;

		/* Check whether we need to reallocate the temporary bo */
		if (stage1_size > priv->stage1_size &&
		    !etnaviv_realloc_stage1(pScrn, priv, stage1_size))
			goto bad_alloc;

		box.x1 = 0;
		box.y1 = 0;
		box.x2 = width;
		box.y2 = drw_h;

		/*
		 * Perform a vertical filter blit first, converting to
		 * YUY2 format if supported and the source is in YUV,
		 * otherwise keeping the original format.
		 */
		op.h_scale = 1 << 16;
		op.v_scale = s_h / drw_h;
		op.dst = INIT_BLIT_BO(priv->stage1_bo, priv->stage1_pitch,
				      priv->stage1_format, ZERO_OFFSET);
		op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_VER_FILTER_BLT;
		op.vr_op = VIVS_DE_VR_CONFIG_START_VERTICAL_BLIT;

		etnaviv_vr_op(etnaviv, &op, &box, xoff, y1, &box, 1);
		/* GC320 and GC600 do not seem to need a flush here */

		/* Set the source for the next stage */
		op.src = op.dst;
		op.src_pitches = op.src_offsets = NULL;

		/*
		 * We have already taken care of the Y offset on the
		 * source image in the above vertical filter blit.
		 */
		y1 = 0;

		op.src_bounds.x1 = 0;
		op.src_bounds.x2 = (x2 + 0xffff) >> 16;
		op.src_bounds.y2 = drw_h;
	} else {
		/* No need for the vertical scaling stage. */
		x1 += xoff;
		x2 += xoff;
	}

	op.dst = INIT_BLIT_BO(vPix->etna_bo, vPix->pitch, vPix->format, dst_offset);
	op.h_scale = s_w / drw_w;
	op.v_scale = 1 << 16;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_HOR_FILTER_BLT;
	op.vr_op = VIVS_DE_VR_CONFIG_START_HORIZONTAL_BLIT;

	/* Perform horizontal filter blt */
	etnaviv_vr_op(etnaviv, &op, &dst, x1, y1, RegionRects(clipBoxes),
		      RegionNumRects(clipBoxes));
	etnaviv_flush(etnaviv);

	/* Wait for vsync */
	if (crtc && priv->props[attr_sync_to_vblank]) {
		vbl.request.sequence = vbl.reply.sequence + 1;
		common_drm_vblank_wait(pScrn, crtc, &vbl, __FUNCTION__, FALSE);
	}

	/*
	 * It would be nice not to wait for the GPU to finish rendering
	 * here, but it seems we can't avoid it.  In theory, 'sync'
	 * would tell us whether we can, but in the case of non-shmem,
	 * that is always false, and the passed buffer is part of the
	 * client specific request buffer on the server.
	 */
	etna_finish(etnaviv->ctx);

	etna_bo_del(etnaviv->conn, usr, NULL);
	DamageDamageRegion(drawable, clipBoxes);

	return Success;

 bad_alloc:
	etna_bo_del(etnaviv->conn, usr, NULL);

	return BadAlloc;
}

static int etnaviv_QueryImageAttributes(ScrnInfoPtr pScrn, int id,
	unsigned short *w, unsigned short *h, int *pitches, int *offsets)
{
	const struct xv_image_format *fmt;
	uint32_t pitch[3], offset[3];
	unsigned short w_align, h_align;
	unsigned i;
	int ret;

	fmt = etnaviv_get_fmt_xv(id);
	if (!fmt)
		return BadMatch;

	/*
	 * Apply our limitations to the width and height:
	 *  - for yuv packed, width must be multiple of 2
	 *  - for yuv planar, width must be multiple of 16
	 *  - must be no larger than the maximum
	 */
	if (fmt->xv_image.type == XvRGB) {
		w_align = 1;
		h_align = 1;
	} else if (fmt->xv_image.format == XvPlanar) {
		w_align = 16;
		h_align = 2;
	} else {
		w_align = 2;
		h_align = 1;
	}

	*w = ALIGN(*w, w_align);
	*h = ALIGN(*h, h_align);

	if (*w > ETNAVIV_XV_MAX_WIDTH)
		*w = ETNAVIV_XV_MAX_WIDTH;
	if (*h > ETNAVIV_XV_MAX_HEIGHT)
		*h = ETNAVIV_XV_MAX_HEIGHT;

	ret = etnaviv_get_fmt_info(fmt, pitch, offset, *w, *h);
	if (!ret)
		return BadMatch;

	for (i = 0; i < fmt->xv_image.num_planes; i++) {
		if (pitches)
			pitches[i] = pitch[i];
		if (offsets)
			offsets[i] = offset[i];
	}

	return ret;
}

static inline float sinc(float x)
{
	return x != 0.0 ? sinf(x) / x : 1.0;
}

/*
 * Some interesting observations of the kernel.  According to the etnaviv
 * rnndb files:
 *  - there are 128 states which hold the kernel.
 *  - each entry contains 9 coefficients (one for each filter tap).
 *  - the entries are indexed by 5 bits from the fractional coordinate
 *    (which makes 32 entries.)
 *
 * As the kernel table is symmetrical around the centre of the fractional
 * coordinate, only half of the entries need to be stored.  In other words,
 * these pairs of indices should be the same:
 *
 *  00=31 01=30 02=29 03=28 04=27 05=26 06=25 07=24
 *  08=23 09=22 10=21 11=20 12=19 13=18 14=17 15=16
 *
 * This means that there are only 16 entries.  However, etnaviv
 * documentation says 17 are required.  What's the additional entry?
 *
 * The next issue is that the filter code always produces zero for the
 * ninth filter tap.  If this is always zero, what's the point of having
 * hardware deal with nine filter taps?  This makes no sense to me.
 */
static void etnaviv_init_filter_kernel(void)
{
	unsigned row, idx, i;
	int16_t kernel_val[KERNEL_STATE_SZ * 2];
	float row_ofs = 0.5;
	float radius = 4.0;

	/* Compute lanczos filter kernel */
	for (row = i = 0; row < KERNEL_ROWS; row++) {
		float kernel[KERNEL_INDICES] = { 0.0 };
		float sum = 0.0;

		for (idx = 0; idx < KERNEL_INDICES; idx++) {
			float x = idx - 4.0 + row_ofs;

			if (fabs(x) <= radius)
				kernel[idx] = sinc(M_PI * x) *
					      sinc(M_PI * x / radius);

			sum += kernel[idx];
		}

		/* normalise the row */
		if (sum)
			for (idx = 0; idx < KERNEL_INDICES; idx++)
				kernel[idx] /= sum;

		/* convert to 1.14 format */
		for (idx = 0; idx < KERNEL_INDICES; idx++) {
			int val = kernel[idx] * (float)(1 << 14);

			if (val < -0x8000)
				val = -0x8000;
			else if (val > 0x7fff)
				val = 0x7fff;

			kernel_val[i++] = val;
		}

		row_ofs -= 1.0 / ((KERNEL_ROWS - 1) * 2);
	}

	kernel_val[KERNEL_SIZE] = 0;

	/* Now convert the kernel values into state values */
	for (i = 0; i < KERNEL_STATE_SZ * 2; i += 2)
		xv_filter_kernel[i / 2] =
			VIVS_DE_FILTER_KERNEL_COEFFICIENT0(kernel_val[i]) |
			VIVS_DE_FILTER_KERNEL_COEFFICIENT1(kernel_val[i + 1]);
}

static Bool etnaviv_xv_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_xv_priv *priv = etnaviv->xv;
	unsigned i;

	if (priv) {
		for (i = 0; i < etnaviv->xv_ports; i++)
			etnaviv_StopVideo(pScrn, &priv[i], TRUE);

		free(priv);
	}

	pScreen->CloseScreen = etnaviv->xv_CloseScreen;

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

XF86VideoAdaptorPtr etnaviv_xv_init(ScreenPtr pScreen, unsigned int *caps)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_xv_priv *priv;
	XF86VideoAdaptorPtr p;
	XF86ImageRec *images;
	DevUnion *devUnions;
	Bool has_yuy2;
	unsigned nports = 16, i, num_images;

#ifdef HAVE_DRI2
	if (etnaviv->dri2_enabled) {
		if (etnaviv->dri2_armada)
			*caps = XVBO_CAP_KMS_DRM;
		else
			*caps = XVBO_CAP_GPU_DRM;
	}
#endif

	etnaviv_init_filter_kernel();

	etnaviv_xv_attributes[attr_pipe].max_value =
		XF86_CRTC_CONFIG_PTR(pScrn)->num_crtc - 1;

	if (!xv_attr_init(etnaviv_attributes, ARRAY_SIZE(etnaviv_attributes)))
		return NULL;

	p = xf86XVAllocateVideoAdaptorRec(pScrn);
	devUnions = calloc(nports, sizeof(*devUnions));
	priv = calloc(nports, sizeof(*priv));
	images = calloc(ARRAY_SIZE(etnaviv_image_formats), sizeof(*images));
	if (!p || !devUnions || !priv || !images) {
		free(images);
		free(priv);
		free(devUnions);
		free(p);
		return NULL;
	}

	for (num_images = i = 0; i < ARRAY_SIZE(etnaviv_image_formats); i++) {
		const struct xv_image_format *fmt = &etnaviv_image_formats[i];
		const struct etnaviv_format *f = fmt->u.data;

		/* Omit formats the hardware is unable to process */
		if (f && !etnaviv_src_format_valid(etnaviv, *f))
			continue;

		if (fmt->xv_image.format == FOURCC_XVBO) {
#ifdef HAVE_DRI2
			if(!etnaviv->dri2_enabled)
#endif
				continue;
		}

		images[num_images++] = fmt->xv_image;
	}

	p->type = XvWindowMask | XvInputMask | XvImageMask;
	p->flags = 0;
	p->name = "Etnaviv Textured Video";
	p->nEncodings = ARRAY_SIZE(etnaviv_encodings);
	p->pEncodings = etnaviv_encodings;
	p->nFormats = ARRAY_SIZE(etnaviv_formats);
	p->pFormats = etnaviv_formats;
	p->nPorts = nports;
	p->pPortPrivates = devUnions;
	p->nAttributes = ARRAY_SIZE(etnaviv_xv_attributes);
	p->pAttributes = etnaviv_xv_attributes;
	p->nImages = num_images;
	p->pImages = images;
	p->StopVideo = etnaviv_StopVideo;
	p->SetPortAttribute = etnaviv_SetPortAttribute;
	p->GetPortAttribute = etnaviv_GetPortAttribute;
	p->QueryBestSize = etnaviv_QueryBestSize;
	p->PutImage = etnaviv_PutImage;
	p->QueryImageAttributes = etnaviv_QueryImageAttributes;

	for (i = 0; i < nports; i++) {
		priv[i].etnaviv = etnaviv;
		priv[i].props[attr_sync_to_vblank] = 1;
		p->pPortPrivates[i].ptr = (pointer) &priv[i];
	}

	/* This feature bit is a guess for the GC supporting YUY2 target... */
	has_yuy2 = VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20);
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "etnaviv: Xv: using %s format intermediate YUV target\n",
		   has_yuy2 ? "YUY2 tiled" : "destination");

	etnaviv->xv = priv;
	etnaviv->xv_ports = nports;
	etnaviv->xv_CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = etnaviv_xv_CloseScreen;

	return p;
}
