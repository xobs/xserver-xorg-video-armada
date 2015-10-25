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
#include "common_drm.h"
#include "common_drm_dri2.h"
#include "common_drm_helper.h"
#include "etnaviv_accel.h"
#include "etnaviv_dri2.h"
#include "etnaviv_utils.h"
#include "pixmaputil.h"

#if DRI2INFOREC_VERSION < 4
#error DRI2 is too old!
#endif

struct etnaviv_dri2_info {
	char *devname;
};

static DRI2Buffer2Ptr etnaviv_dri2_CreateBuffer(DrawablePtr drawable,
	unsigned int attachment, unsigned int format)
{
	struct common_dri2_buffer *buf;
	ScreenPtr pScreen = drawable->pScreen;
	PixmapPtr pixmap = NULL;
	uint32_t name;

	buf = calloc(1, sizeof *buf);
	if (!buf)
		return NULL;

	if (attachment == DRI2BufferFrontLeft) {
		pixmap = drawable_pixmap(drawable);

		if (!etnaviv_get_pixmap_priv(pixmap)) {
			/* Force the backing buffer to be reallocated */
			drawable = &pixmap->drawable;
			pixmap = NULL;
		} else {
			pixmap->refcnt++;
		}
	}

	if (pixmap == NULL) {
		pixmap = common_dri2_create_pixmap(drawable, attachment, format,
						   CREATE_PIXMAP_USAGE_GPU);
		if (!pixmap)
			goto err;
	}

	if (!etnaviv_pixmap_flink(pixmap, &name))
		goto err;

	return common_dri2_setup_buffer(buf, attachment, format,
					pixmap, name, 0);

 err:
	if (pixmap)
		pScreen->DestroyPixmap(pixmap);
	free(buf);

	return NULL;
}

static void etnaviv_dri2_CopyRegion(DrawablePtr drawable, RegionPtr pRegion,
	DRI2BufferPtr dstBuf, DRI2BufferPtr srcBuf)
{
	ScreenPtr screen = drawable->pScreen;
	DrawablePtr src = common_dri2_get_drawable(srcBuf, drawable);
	DrawablePtr dst = common_dri2_get_drawable(dstBuf, drawable);
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

	/*
	 * We will commit the copy via the flush callback called from
	 * WriteToClient.
	 */
}

static void etnaviv_dri2_blit(ClientPtr client, DrawablePtr draw,
	DRI2BufferPtr front, DRI2BufferPtr back, unsigned frame,
	unsigned tv_sec, unsigned tv_usec, DRI2SwapEventPtr func, void *data)
{
	RegionRec region;
	BoxRec box;

	box.x1 = 0;
	box.y1 = 0;
	box.x2 = draw->width;
	box.y2 = draw->height;
	RegionInit(&region, &box, 0);

	etnaviv_dri2_CopyRegion(draw, &region, front, back);

	DRI2SwapComplete(client, draw, frame, tv_sec, tv_usec,
			 DRI2_BLIT_COMPLETE, func, data);
}

static void etnaviv_dri2_swap(struct common_dri2_wait *wait, DrawablePtr draw,
	unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	etnaviv_dri2_blit(wait->client, draw, wait->front, wait->back,
			  frame, tv_sec, tv_usec,
			  wait->client ? wait->swap_func : NULL,
			  wait->swap_data);
	common_dri2_wait_free(wait);
}

static void etnaviv_dri2_flip_complete(struct common_dri2_wait *wait,
	DrawablePtr draw, unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	DRI2SwapComplete(wait->client, draw, frame, tv_sec, tv_usec,
			 DRI2_FLIP_COMPLETE,
			 wait->client ? wait->swap_func : NULL,
			 wait->swap_data);

	common_dri2_wait_free(wait);
}

static Bool etnaviv_dri2_ScheduleFlip(DrawablePtr drawable,
	struct common_dri2_wait *wait)
{
	ScreenPtr pScreen = drawable->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	PixmapPtr front = pScreen->GetScreenPixmap(pScreen);
	PixmapPtr back = to_common_dri2_buffer(wait->back)->pixmap;

	assert(front == to_common_dri2_buffer(wait->front)->pixmap);

	if (common_drm_flip(pScrn, back, wait, wait->crtc)) {
		struct etnaviv_pixmap *f_pix = etnaviv_get_pixmap_priv(front);
		struct etnaviv_pixmap *b_pix = etnaviv_get_pixmap_priv(back);

		etnaviv_set_pixmap_priv(front, b_pix);
		etnaviv_set_pixmap_priv(back, f_pix);

		common_dri2_flip_buffers(pScreen, wait);

		wait->event_func = etnaviv_dri2_flip_complete;

		return TRUE;
	}

	return FALSE;
}

static void etnaviv_dri2_flip(struct common_dri2_wait *wait, DrawablePtr draw,
	unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	if (common_dri2_can_flip(draw, wait) &&
	    etnaviv_dri2_ScheduleFlip(draw, wait))
		return;

	etnaviv_dri2_swap(wait, draw, frame, tv_sec, tv_usec);
}

static int etnaviv_dri2_ScheduleSwap(ClientPtr client, DrawablePtr draw,
	DRI2BufferPtr front, DRI2BufferPtr back, CARD64 *target_msc,
	CARD64 divisor, CARD64 remainder, DRI2SwapEventPtr func, void *data)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);
	struct common_dri2_wait *wait;
	drmVBlank vbl;
	CARD64 cur_msc;
	xf86CrtcPtr crtc;
	int ret;

	crtc = common_drm_drawable_covering_crtc(draw);

	/* Drawable not displayed... just complete */
	if (!crtc)
		goto blit;

	*target_msc &= 0xffffffff;
	divisor &= 0xffffffff;
	remainder &= 0xffffffff;

	wait = common_dri2_wait_alloc(client, draw, DRI2_SWAP);
	if (!wait)
		goto blit;

	wait->event_func = etnaviv_dri2_swap;
	wait->crtc = crtc;
	wait->swap_func = func;
	wait->swap_data = data;
	wait->front = front;
	wait->back = back;

	common_dri2_buffer_reference(front);
	common_dri2_buffer_reference(back);

	ret = common_drm_vblank_get(pScrn, crtc, &vbl, __FUNCTION__);
	if (ret)
		goto blit_free;

	cur_msc = vbl.reply.sequence;

	/* Flips need to be submitted one frame before */
	if (common_dri2_can_flip(draw, wait)) {
		wait->event_func = etnaviv_dri2_flip;
		wait->type = DRI2_FLIP;
		if (*target_msc > 0)
			*target_msc -= 1;
	}

	if (divisor == 0 || cur_msc < *target_msc) {
		/*
		 * If we can, schedule the flip directly from here rather
		 * than waiting for an event from the kernel for the current
		 * (or a past) MSC.
		 */
		if (wait->type == DRI2_FLIP &&
		    divisor == 0 && cur_msc >= *target_msc &&
		    etnaviv_dri2_ScheduleFlip(draw, wait)) {
			/*
			 * I think xf86-video-intel misses this: target_msc
			 * is in the past, we should update it with the new
			 * msc, otherwise it will remain at the original.
			 */
			*target_msc = cur_msc;
			return TRUE;
		}

		/*
		 * If target_msc has been passed, set it to cur_msc to
		 * ensure we return a reasonable value back to the caller.
		 * This makes the swap_interval logic more robust.
		 */
		if (cur_msc > *target_msc)
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

	ret = common_drm_vblank_queue_event(pScrn, crtc, &vbl, __FUNCTION__,
					    wait->type != DRI2_FLIP, wait);
	if (ret)
		goto blit_free;

	*target_msc = vbl.reply.sequence + (wait->type == DRI2_FLIP);
	wait->frame = *target_msc;

	return TRUE;

 blit_free:
	common_dri2_wait_free(wait);
 blit:
	etnaviv_dri2_blit(client, draw, front, back, 0, 0, 0, func, data);
	*target_msc = 0;
	return TRUE;
}

static const DRI2InfoRec dri2_info = {
	.version = 4,

	.CreateBuffer = etnaviv_dri2_CreateBuffer,
	.DestroyBuffer = common_dri2_DestroyBuffer,
	.CopyRegion = etnaviv_dri2_CopyRegion,

	.ScheduleSwap = etnaviv_dri2_ScheduleSwap,
	.GetMSC = common_dri2_GetMSC,
	.ScheduleWaitMSC = common_dri2_ScheduleWaitMSC,
};

Bool etnaviv_dri2_ScreenInit(ScreenPtr pScreen, int drm_fd, const char *name)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_dri2_info *dri;
	DRI2InfoRec info;
	int dri2_major = 0;
	int dri2_minor = 0;
	const char *driverNames[1];

	if (xf86LoaderCheckSymbol("DRI2Version"))
		DRI2Version(&dri2_major, &dri2_minor);

	if (dri2_major < 1 || (dri2_major == 1 && dri2_minor < 2)) {
		xf86DrvMsg(etnaviv->scrnIndex, X_WARNING,
			   "DRI2 requires DRI2 module version 1.2.0 or later\n");
		return FALSE;
	}

	if (!common_dri2_ScreenInit(pScreen))
		return FALSE;

	dri = xnfcalloc(1, sizeof *dri);
	dri->devname = drmGetDeviceNameFromFd(drm_fd);

	etnaviv->dri2 = dri;

	info = dri2_info;
	info.fd = drm_fd;
	info.driverName = name;
	info.deviceName = dri->devname;
	info.numDrivers = 1;
	info.driverNames = driverNames;
	driverNames[0] = info.driverName;

	return DRI2ScreenInit(pScreen, &info);
}

void etnaviv_dri2_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_dri2_info *dri = etnaviv->dri2;

	if (dri) {
		DRI2CloseScreen(pScreen);

		etnaviv->dri2 = NULL;
		drmFree(dri->devname);
		free(dri);
	}
}
