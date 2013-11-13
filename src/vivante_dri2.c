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

/* xorg includes */
#include "dixstruct.h"
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "dri2.h"

/* drm includes */
#include <xf86drm.h>
#include <armada_bufmgr.h>

#include "compat-api.h"
#include "vivante_accel.h"
#include "vivante_dri2.h"
#include "vivante_utils.h"

#if DRI2INFOREC_VERSION < 4
#error DRI2 is too old!
#endif

struct vivante_dri2_info {
	char *devname;
};

struct vivante_dri2_buffer {
	DRI2BufferRec dri2;
	PixmapPtr pixmap;
	unsigned ref;
};

enum event_type {
	DRI2_SWAP,
	DRI2_SWAP_CHAIN,
	DRI2_FLIP,
	DRI2_WAITMSC,
};

struct vivante_dri_wait {
	struct vivante_dri_wait *next;
	struct xorg_list drawable_list;
	struct xorg_list client_list;
	XID drawable_id;
	ClientPtr client;
	enum event_type type;
	unsigned crtc;
	int frame;

	/* For swaps/flips */
	DRI2SwapEventPtr event_func;
	void *event_data;
	DRI2BufferPtr front;
	DRI2BufferPtr back;
};

#if HAS_DEVPRIVATEKEYREC
typedef DevPrivateKeyRec vivante_dri2_client_key_t;
#else
typedef int vivante_dri2_client_key_t;
#endif

static vivante_dri2_client_key_t vivante_dri2_client_key;
static RESTYPE wait_client_restype, wait_drawable_restype;

#if HAS_DIXREGISTERPRIVATEKEY
#define vivante_dri2_get_client_private(c) \
	dixGetPrivateAddr(&(c)->devPrivates, &vivante_dri2_client_key)
#define vivante_dri2_register_private() \
	dixRegisterPrivateKey(&vivante_dri2_client_key, PRIVATE_CLIENT, \
			 sizeof(XID))
#else
#define vivante_dri2_get_client_private(c) \
	dixLookupPrivate(&(c)->devPrivates, &vivante_dri2_client_key)
#define vivante_dri2_register_private() \
	dixRequestPrivate(&vivante_dri2_client_key, sizeof(XID))
#endif

static int vivante_dri2_drawable_crtc(DrawablePtr draw)
{
	/* FIXME */
	return -1;
}

static void vivante_dri2_buffer_reference(DRI2Buffer2Ptr buffer)
{
	struct vivante_dri2_buffer *priv = buffer->driverPrivate;

	priv->ref++;
}

static DrawablePtr
vivante_dri2_get_drawable(DRI2BufferPtr buffer, DrawablePtr drawable)
{
	struct vivante_dri2_buffer *buf = buffer->driverPrivate;

	return buffer->attachment == DRI2BufferFrontLeft ?
		drawable : &buf->pixmap->drawable;
}

static PixmapPtr vivante_dri2_get_front_pixmap(DrawablePtr drawable)
{
	PixmapPtr pixmap = vivante_drawable_pixmap(drawable);
	struct vivante_pixmap *vpix = vivante_get_pixmap_priv(pixmap);

	if (!vpix)
		return NULL;

	pixmap->refcnt++;

	return pixmap;
}

static PixmapPtr
vivante_dri2_get_pixmap(DRI2BufferPtr buffer)
{
	struct vivante_dri2_buffer *buf = buffer->driverPrivate;
	return buf->pixmap;
}

static int vivante_dri2_client_gone(void *data, XID id)
{
	struct xorg_list *list = data;

	while (!xorg_list_is_empty(list)) {
		struct vivante_dri_wait *wait;

		wait = xorg_list_first_entry(list, struct vivante_dri_wait, client_list);
		xorg_list_del(&wait->client_list);
		wait->client = NULL;
	}
	free(list);

	return Success;
}

static int vivante_dri2_drawable_gone(void *data, XID id)
{
	struct xorg_list *list = data;

	while (!xorg_list_is_empty(list)) {
		struct vivante_dri_wait *wait;

		wait = xorg_list_first_entry(list, struct vivante_dri_wait, drawable_list);
		xorg_list_del(&wait->drawable_list);
		wait->drawable_id = None;
	}
	free(list);

	return Success;
}

static XID client_id(ClientPtr client)
{
	XID *ptr = vivante_dri2_get_client_private(client);

	if (*ptr == 0)
		*ptr = FakeClientID(client->index);

	return *ptr;
}

static Bool add_reslist(RESTYPE type, XID id, struct xorg_list *node)
{
	struct xorg_list *list;
	void *ptr = NULL;

	dixLookupResourceByType(&ptr, id, type, NULL, DixWriteAccess);
	list = ptr;
	if (!list) {
		list = malloc(sizeof *list);
		if (!list)
			return FALSE;

		if (!AddResource(id, type, list)) {
			free(list);
			return FALSE;
		}

		xorg_list_init(list);
	}

	xorg_list_add(node, list);

	return TRUE;
}

static Bool
can_exchange(DrawablePtr drawable, DRI2BufferPtr front, DRI2BufferPtr back)
{
	PixmapPtr front_pix = vivante_dri2_get_pixmap(front);
	PixmapPtr back_pix = vivante_dri2_get_pixmap(back);

	if (!DRI2CanFlip(drawable))
		return FALSE;

	/* Front and back must be the same size and bpp */
	if (front_pix->drawable.width != back_pix->drawable.width ||
	    front_pix->drawable.height != back_pix->drawable.height ||
	    front_pix->drawable.bitsPerPixel != back_pix->drawable.bitsPerPixel)
		return FALSE;

	return TRUE;
}

static DRI2Buffer2Ptr
vivante_dri2_CreateBuffer(DrawablePtr drawable, unsigned int attachment,
	unsigned int format)
{
	struct vivante_dri2_buffer *buf;
	struct vivante_pixmap *vpix;
	ScreenPtr pScreen = drawable->pScreen;
	PixmapPtr pixmap = NULL;
	uint32_t name;

fprintf(stderr, "%s: %p %u %u\n", __func__, drawable, attachment, format);
	if (attachment == DRI2BufferFrontLeft) {
		pixmap = vivante_dri2_get_front_pixmap(drawable);
		if (!pixmap) {
			drawable = &pixmap->drawable;
			pixmap = NULL;
		}
	}

	if (pixmap == NULL) {
		int width = drawable->width;
		int height = drawable->height;
		int depth = format ? format : drawable->depth;

		pixmap = pScreen->CreatePixmap(pScreen, width, height, depth, 0);
		if (!pixmap)
			goto err;
	}

	vpix = vivante_get_pixmap_priv(pixmap);
	if (!vpix)
		goto err;

	buf = calloc(1, sizeof *buf);
	if (!buf)
		goto err;

	if (!vpix->bo || drm_armada_bo_flink(vpix->bo, &name)) {
		free(buf);
		goto err;
	}

	buf->dri2.attachment = attachment;
	buf->dri2.name = name;
	buf->dri2.pitch = pixmap->devKind;
	buf->dri2.cpp = pixmap->drawable.bitsPerPixel / 8;
	buf->dri2.flags = 0;
	buf->dri2.format = format;
	buf->dri2.driverPrivate = buf;
	buf->pixmap = pixmap;
	buf->ref = 1;

	return &buf->dri2;

 err:
	if (pixmap)
		pScreen->DestroyPixmap(pixmap);

	return NULL;
}

static void
vivante_dri2_DestroyBuffer(DrawablePtr drawable, DRI2Buffer2Ptr buffer)
{
	if (buffer) {
		struct vivante_dri2_buffer *buf = buffer->driverPrivate;
		ScreenPtr screen;

		/* This should never happen... due to how we allocate the private */
		assert(buf != NULL);

		if (--buf->ref != 0)
			return;

		screen = buf->pixmap->drawable.pScreen;
		screen->DestroyPixmap(buf->pixmap);

		free(buf);
	}
}

static void
vivante_dri2_CopyRegion(DrawablePtr drawable, RegionPtr pRegion,
	DRI2BufferPtr dstBuf, DRI2BufferPtr srcBuf)
{
	ScreenPtr screen = drawable->pScreen;
	DrawablePtr src = vivante_dri2_get_drawable(srcBuf, drawable);
	DrawablePtr dst = vivante_dri2_get_drawable(dstBuf, drawable);
	RegionPtr clip;
	GCPtr gc;

	gc = GetScratchGC(dst->depth, screen);
	if (!gc)
		return;

	clip = REGION_CREATE(screen, NULL, 0);
	REGION_COPY(screen, clip, pRegion);
	gc->funcs->ChangeClip(gc, CT_REGION, clip, 0);
	ValidateGC(dst, gc);

	/*
	 * FIXME: wait for scanline to be outside the region to be copied...
	 * that is an interesting problem for Dove/GAL stuff because they're
	 * independent, and there's no way for the GPU to know where the
	 * scan position is.  For now, just do the copy anyway.
	 */
	gc->ops->CopyArea(src, dst, gc, 0, 0,
			  drawable->width, drawable->height, 0, 0);

	FreeScratchGC(gc);
}

static struct vivante_dri_wait *
new_wait_info(ClientPtr client, DrawablePtr draw, enum event_type type)
{
	struct vivante_dri_wait *wait = calloc(1, sizeof *wait);

	if (wait) {
		wait->drawable_id = draw->id;
		wait->client = client;
		wait->type = type;

		xorg_list_init(&wait->client_list);
		xorg_list_init(&wait->drawable_list);

		if (!add_reslist(wait_drawable_restype, draw->id,
				 &wait->drawable_list) ||
		    !add_reslist(wait_client_restype, client_id(client),
				 &wait->client_list)) {
			xorg_list_del(&wait->client_list);
			xorg_list_del(&wait->drawable_list);
			free(wait);
			wait = NULL;
		}
	}
	return wait;
}

static void del_wait_info(struct vivante_dri_wait *wait)
{
	xorg_list_del(&wait->client_list);
	xorg_list_del(&wait->drawable_list);

	vivante_dri2_DestroyBuffer(NULL, wait->front);
	vivante_dri2_DestroyBuffer(NULL, wait->back);

	free(wait);
}

static Bool
vivante_dri2_ScheduleFlip(DrawablePtr drawable, struct vivante_dri_wait *wait)
{
	return FALSE;
}

static void
vivante_dri2_blit(ClientPtr client, DrawablePtr draw, DRI2BufferPtr front,
	DRI2BufferPtr back, unsigned frame, unsigned tv_sec, unsigned tv_usec,
	DRI2SwapEventPtr func, void *data)
{
	RegionRec region;
	BoxRec box;

	box.x1 = 0;
	box.y1 = 0;
	box.x2 = draw->width;
	box.y2 = draw->height;
	RegionInit(&region, &box, 0);

	vivante_dri2_CopyRegion(draw, &region, front, back);

	DRI2SwapComplete(client, draw, frame, tv_sec, tv_usec,
			 DRI2_BLIT_COMPLETE, func, data);
}

void
vivante_dri2_vblank(int fd, unsigned frame, unsigned tv_sec, unsigned tv_usec,
	void *event)
{
	struct vivante_dri_wait *wait = event;
	DrawablePtr draw;

	if (!wait->drawable_id)
		goto out;

	if (dixLookupDrawable(&draw, wait->drawable_id, serverClient, M_ANY,
			      DixWriteAccess) != Success)
		goto out;

	switch (wait->type) {
	case DRI2_FLIP:
		if (can_exchange(draw, wait->front, wait->back) &&
		    vivante_dri2_ScheduleFlip(draw, wait))
			return;
		/* FALLTHROUGH */

	case DRI2_SWAP:
		vivante_dri2_blit(wait->client, draw, wait->front, wait->back,
				  frame, tv_sec, tv_usec,
				  wait->client ? wait->event_func : NULL,
				  wait->event_data);
		break;

	case DRI2_WAITMSC:
		if (wait->client)
			DRI2WaitMSCComplete(wait->client, draw, frame, tv_sec, tv_usec);
		break;

	default:
		xf86DrvMsg(xf86Screens[draw->pScreen->myNum]->scrnIndex, X_WARNING,
			   "%s: unknown vblank event received\n",
			   __FUNCTION__);
		break;
	}

 out:
	del_wait_info(wait);
}

static uint32_t drm_req_crtc(unsigned crtc)
{
	/*
	 * We only support newer kernels here - always
	 * encode the CRTC id in the high crtc field.
	 */
	return crtc << DRM_VBLANK_HIGH_CRTC_SHIFT;
}

static int
vivante_dri2_waitvblank(struct vivante *vivante, drmVBlank *vbl, unsigned crtc,
	const char *func)
{
	static int limit = 5;
	int ret;

	vbl->request.type = DRM_VBLANK_RELATIVE | drm_req_crtc(crtc);
	vbl->request.sequence = 0;

	ret = drmWaitVBlank(vivante->drm_fd, vbl);
	if (ret && limit) {
		xf86DrvMsg(vivante->scrnIndex, X_WARNING,
			   "%s: get vblank counter failed: %s\n",
			   func, strerror(errno));
		limit--;
	}
	return ret;
}

static int
vivante_dri2_ScheduleSwap(ClientPtr client, DrawablePtr draw,
	DRI2BufferPtr front, DRI2BufferPtr back, CARD64 *target_msc,
	CARD64 divisor, CARD64 remainder, DRI2SwapEventPtr func, void *data)
{
	struct vivante *vivante = vivante_get_screen_priv(draw->pScreen);
	struct vivante_dri_wait *wait;
	drmVBlank vbl;
	CARD64 cur_msc;
	int ret, crtc;

	crtc = vivante_dri2_drawable_crtc(draw);

	/* Drawable not displayed... just complete */
	if (crtc < 0)
		goto blit;

	*target_msc &= 0xffffffff;
	divisor &= 0xffffffff;
	remainder &= 0xffffffff;

	wait = new_wait_info(client, draw, DRI2_SWAP);
	if (!wait)
		goto blit;

	wait->crtc = crtc;
	wait->event_func = func;
	wait->event_data = data;
	wait->front = front;
	wait->back = back;

	vivante_dri2_buffer_reference(front);
	vivante_dri2_buffer_reference(back);

	ret = vivante_dri2_waitvblank(vivante, &vbl, crtc, __FUNCTION__);
	if (ret)
		goto blit_free;

	cur_msc = vbl.reply.sequence;

	/* Flips need to be submitted one frame before */
	if (can_exchange(draw, front, back)) {
		wait->type = DRI2_FLIP;
		if (*target_msc > 0)
			*target_msc -= 1;
	}

	if (divisor == 0 || cur_msc < *target_msc) {
		if (wait->type == DRI2_FLIP && vivante_dri2_ScheduleFlip(draw, wait))
			return TRUE;

		/*
		 * If target_msc has been reached or passed, set it to cur_msc
		 * to ensure we return a reasonable value back to the caller.
		 * This makes the swap_interval logic more robust.
		 */
		if (cur_msc >= *target_msc)
			*target_msc = cur_msc;

		vbl.request.sequence = *target_msc;
	} else {
		vbl.request.sequence = cur_msc - (cur_msc % divisor) + remainder;

		/*
		 * If the calculated deadline sequence is smaller than or equal
		 * to cur_msc, it means we've passed the point when effective
		 * onset frame seq could satisfy seq % divisor == remainder,
		 * so we need to wait for the next time this will happen.
		 *
		 * This comparison takes the 1 frame swap delay in pageflipping
		 * mode into account, as well as a potential
		 * DRM_VBLANK_NEXTONMISS delay if we are blitting/exchanging
		 * instead of flipping.
		 */
		 if (vbl.request.sequence <= cur_msc)
			 vbl.request.sequence += divisor;

		 /* Account for 1 frame extra pageflip delay if flip > 0 */
		 if (wait->type == DRI2_FLIP)
			 vbl.request.sequence -= 1;
	}

	vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | drm_req_crtc(crtc);
	if (wait->type != DRI2_FLIP)
		vbl.request.type |= DRM_VBLANK_NEXTONMISS;

	vbl.request.signal = (unsigned long)wait;
	ret = drmWaitVBlank(vivante->drm_fd, &vbl);
	if (ret) {
		xf86DrvMsg(vivante->scrnIndex, X_WARNING,
			   "get vblank counter failed: %s\n",
			   strerror(errno));
		goto blit_free;
	}

	*target_msc = vbl.reply.sequence + (wait->type == DRI2_FLIP);
	wait->frame = *target_msc;

	return TRUE;

 blit_free:
	del_wait_info(wait);
 blit:
	vivante_dri2_blit(client, draw, front, back, 0, 0, 0, func, data);
	*target_msc = 0;
	return TRUE;
}

static int vivante_dri2_GetMSC(DrawablePtr draw, CARD64 *ust, CARD64 *msc)
{
	struct vivante *vivante = vivante_get_screen_priv(draw->pScreen);
	drmVBlank vbl;
	int ret, crtc;

	crtc = vivante_dri2_drawable_crtc(draw);

	/* Drawable not displayed, make up a value */
	if (crtc < 0) {
		*ust = 0;
		*msc = 0;
		return TRUE;
	}

	ret = vivante_dri2_waitvblank(vivante, &vbl, crtc, __FUNCTION__);
	if (ret)
		return FALSE;

	*ust = ((CARD64)vbl.reply.tval_sec * 1000000) + vbl.reply.tval_usec;
	*msc = vbl.reply.sequence;

	return TRUE;
}

static int
vivante_dri2_ScheduleWaitMSC(ClientPtr client, DrawablePtr draw,
	CARD64 target_msc, CARD64 divisor, CARD64 remainder)
{
	struct vivante *vivante = vivante_get_screen_priv(draw->pScreen);
	struct vivante_dri_wait *wait;
	drmVBlank vbl;
	int ret, crtc;
	CARD64 cur_msc;

	target_msc &= 0xffffffff;
	divisor &= 0xffffffff;
	remainder &= 0xffffffff;

	crtc = vivante_dri2_drawable_crtc(draw);

	/* Drawable not displayed, just complete */
	if (crtc < 0)
		goto out;

	wait = new_wait_info(client, draw, DRI2_WAITMSC);
	if (!wait)
		goto out;

	/* Get current count */
	ret = vivante_dri2_waitvblank(vivante, &vbl, crtc, __FUNCTION__);
	if (ret)
		goto out_free;

	cur_msc = vbl.reply.sequence;

	/*
	 * If the divisor is zero, or cur_msc is smaller than target_msc, we
	 * just need to make sure target_msc passes before waking up the client.
	 */
	if (divisor == 0 || cur_msc < target_msc) {
		if (cur_msc >= target_msc)
			target_msc = cur_msc;

		vbl.request.sequence = target_msc;
	} else {
		/*
		 * If we get here, target_msc has already passed or we
		 * don't have one, so queue an event that will satisfy
		 * the divisor/remainder equation.
		 */
		vbl.request.sequence = cur_msc - (cur_msc % divisor) + remainder;

		/*
		 * If calculated remainder is larger than requested
		 * remainder, it means we've passed the point where
		 * seq % divisor == remainder, so we need to wait for
		 * the next time that will happen.
		 */
		if ((cur_msc & divisor) >= remainder)
			vbl.request.sequence += divisor;
	}

	vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT | drm_req_crtc(crtc);
	vbl.request.signal = (unsigned long)wait;
	ret = drmWaitVBlank(vivante->drm_fd, &vbl);
	if (ret) {
		xf86DrvMsg(vivante->scrnIndex, X_WARNING,
				   "%s: get vblank counter failed: %s\n",
				   __FUNCTION__, strerror(errno));
		goto out_free;
	}

	wait->frame = vbl.reply.sequence;
	DRI2BlockClient(client, draw);
	return TRUE;

 out_free:
	del_wait_info(wait);
 out:
	DRI2WaitMSCComplete(client, draw, target_msc, 0, 0);
	return TRUE;
}

static int dri2_server_generation;

Bool vivante_dri2_ScreenInit(ScreenPtr pScreen)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_dri2_info *dri;
	DRI2InfoRec info;
	int dri2_major = 0;
	int dri2_minor = 0;
	const char *driverNames[1];

	if (xf86LoaderCheckSymbol("DRI2Version"))
		DRI2Version(&dri2_major, &dri2_minor);

	if (dri2_major < 1 || (dri2_major == 1 && dri2_minor < 1)) {
		xf86DrvMsg(vivante->scrnIndex, X_WARNING,
			   "DRI2 requires DRI2 module version 1.1.0 or later\n");
		return FALSE;
	}

	if (!vivante_dri2_register_private())
		return FALSE;

	if (dri2_server_generation != serverGeneration) {
		dri2_server_generation = serverGeneration;

		wait_client_restype = CreateNewResourceType(vivante_dri2_client_gone,
							"Frame Event Client");
		wait_drawable_restype = CreateNewResourceType(vivante_dri2_drawable_gone,
							  "Frame Event Drawable");

		if (!wait_client_restype || !wait_drawable_restype) {
			xf86DrvMsg(vivante->scrnIndex, X_WARNING,
				   "Can not register DRI2 frame event resources\n");
			return FALSE;
		}
	}

	dri = xnfcalloc(1, sizeof *dri);
	dri->devname = drmGetDeviceNameFromFd(vivante->drm_fd);

	vivante->dri2 = dri;

	memset(&info, 0, sizeof(info));
	info.version = 4;
	info.fd = vivante->drm_fd;
	info.driverName = "galdri";
	info.deviceName = dri->devname;

	info.CreateBuffer = vivante_dri2_CreateBuffer;
	info.DestroyBuffer = vivante_dri2_DestroyBuffer;
	info.CopyRegion = vivante_dri2_CopyRegion;

	info.ScheduleSwap = vivante_dri2_ScheduleSwap;
	info.GetMSC = vivante_dri2_GetMSC;
	info.ScheduleWaitMSC = vivante_dri2_ScheduleWaitMSC;
	info.numDrivers = 1;
	info.driverNames = driverNames;
	driverNames[0] = info.driverName;

	return DRI2ScreenInit(pScreen, &info);
}

void vivante_dri2_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_dri2_info *dri = vivante->dri2;

	if (dri) {
		DRI2CloseScreen(pScreen);

		vivante->dri2 = NULL;
		drmFree(dri->devname);
		free(dri);
	}
}
