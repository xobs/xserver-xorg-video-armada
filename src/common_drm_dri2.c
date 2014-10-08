/*
 * Common DRM specific parts of DRI2 support
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "dixstruct.h"
#include "dri2.h"
#include "xf86.h"

#include "common_drm.h"
#include "common_drm_dri2.h"
#include "common_drm_helper.h"
#include "compat-api.h"

static DevPrivateKeyRec dri2_client_key;
static RESTYPE dri2_wait_client_restype;
static RESTYPE dri2_wait_drawable_restype;

#define dri2_get_client_private(c) \
	dixGetPrivateAddr(&(c)->devPrivates, &dri2_client_key)
#define dri2_register_private() \
	dixRegisterPrivateKey(&dri2_client_key, PRIVATE_CLIENT, sizeof(XID))

static XID common_dri2_client_id(ClientPtr client)
{
	XID *ptr = dri2_get_client_private(client);

	if (*ptr == 0)
		*ptr = FakeClientID(client->index);

	return *ptr;
}

static Bool common_dri2_add_reslist(XID id, RESTYPE type,
	struct xorg_list *node)
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

_X_EXPORT
struct common_dri2_wait *__common_dri2_wait_alloc(ClientPtr client,
	DrawablePtr draw, enum common_dri2_event_type type, size_t size)
{
	struct common_dri2_wait *wait;

	if (size < sizeof(*wait))
		return NULL;

	wait = calloc(1, size);
	if (wait) {
		wait->drawable_id = draw->id;
		wait->client = client;
		wait->type = type;

		xorg_list_init(&wait->client_list);
		xorg_list_init(&wait->drawable_list);

		if (!common_dri2_add_reslist(wait->drawable_id, dri2_wait_drawable_restype,
					     &wait->drawable_list) ||
		    !common_dri2_add_reslist(common_dri2_client_id(wait->client),
					     dri2_wait_client_restype,
					     &wait->client_list)) {
			common_dri2_wait_free(wait);
			wait = NULL;
		}
	}
	return wait;
}

_X_EXPORT
void common_dri2_wait_free(struct common_dri2_wait *wait)
{
	common_dri2_DestroyBuffer(NULL, wait->front);
	common_dri2_DestroyBuffer(NULL, wait->back);
	xorg_list_del(&wait->client_list);
	xorg_list_del(&wait->drawable_list);
	free(wait);
}

_X_EXPORT
xf86CrtcPtr common_dri2_drawable_crtc(DrawablePtr pDraw)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
	xf86CrtcPtr crtc;
	BoxRec box, crtcbox;

	box.x1 = pDraw->x;
	box.y1 = pDraw->y;
	box.x2 = box.x1 + pDraw->width;
	box.y2 = box.y1 + pDraw->height;

	crtc = common_drm_covering_crtc(pScrn, &box, NULL, &crtcbox);

	/* Make sure the CRTC is valid and this is the real front buffer */
	if (crtc && crtc->rotatedData)
		crtc = NULL;

	return crtc;
}

_X_EXPORT
Bool common_dri2_can_flip(DrawablePtr pDraw, struct common_dri2_wait *wait)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	PixmapPtr front_pix = to_common_dri2_buffer(wait->front)->pixmap;
	PixmapPtr back_pix = to_common_dri2_buffer(wait->back)->pixmap;

	if (pDraw->type == DRAWABLE_PIXMAP || drm->shadow_present)
		return FALSE;

	if (!DRI2CanFlip(pDraw))
		return FALSE;

	/* Front and back must be the same size and bpp */
	if (front_pix->drawable.width != back_pix->drawable.width ||
	    front_pix->drawable.height != back_pix->drawable.height ||
	    front_pix->drawable.bitsPerPixel != back_pix->drawable.bitsPerPixel)
		return FALSE;

	return TRUE;
}

/*
 * This works out whether we may (at some point in the future) be able
 * to flip this drawable.  This is almost the same as DRI2CanFlip()
 * except for the lack of clipping check, and attachment test.
 */
_X_EXPORT
Bool common_dri2_may_flip(DrawablePtr pDraw, unsigned int attachment)
{
	ScreenPtr pScreen = pDraw->pScreen;
	WindowPtr pWin, pRoot;
	PixmapPtr pWinPixmap, pRootPixmap;

	if (pDraw->type == DRAWABLE_PIXMAP)
		return FALSE;

	if (attachment != DRI2BufferFrontLeft &&
	    attachment != DRI2BufferBackLeft &&
	    attachment != DRI2BufferFrontRight &&
	    attachment != DRI2BufferBackRight)
		return FALSE;

	pWin = (WindowPtr)pDraw;
	pWinPixmap = pScreen->GetWindowPixmap(pWin);
	pRoot = pScreen->root;
	pRootPixmap = pScreen->GetWindowPixmap(pRoot);

	if (pWinPixmap != pRootPixmap ||
	    pDraw->x != 0 || pDraw->y != 0 ||
#ifdef COMPOSITE
	    pDraw->x != pWinPixmap->screen_x ||
	    pDraw->y != pWinPixmap->screen_y ||
#endif
	    pDraw->width != pWinPixmap->drawable.width ||
	    pDraw->height != pWinPixmap->drawable.height)
		return FALSE;

	return TRUE;
}

_X_EXPORT
void common_dri2_flip_buffers(ScreenPtr pScreen, struct common_dri2_wait *wait)
{
	struct common_dri2_buffer *front = to_common_dri2_buffer(wait->front);
	struct common_dri2_buffer *back = to_common_dri2_buffer(wait->back);
	uint32_t name;

	/* Swap the DRI2 buffer names */
	name = front->base.name;
	front->base.name = back->base.name;
	back->base.name = name;

	/* Swap the common drm pixmap information */
	common_drm_flip_pixmap(pScreen, front->pixmap, back->pixmap);
}

_X_EXPORT
PixmapPtr common_dri2_create_pixmap(DrawablePtr pDraw, unsigned int attachment,
	unsigned int format, int usage_hint)
{
	ScreenPtr pScreen = pDraw->pScreen;
	PixmapPtr pixmap;
	int width = pDraw->width;
	int height = pDraw->height;
	int depth = format ? format : pDraw->depth;

	pixmap = pScreen->CreatePixmap(pScreen, width, height,
				       depth, usage_hint);

	return pixmap;
}

_X_EXPORT
DRI2Buffer2Ptr common_dri2_setup_buffer(struct common_dri2_buffer *buf,
	unsigned int attachment, unsigned int format, PixmapPtr pixmap,
	uint32_t name, unsigned int flags)
{
	buf->base.attachment = attachment;
	buf->base.name = name;
	buf->base.pitch = pixmap->devKind;
	buf->base.cpp = pixmap->drawable.bitsPerPixel / 8;
	buf->base.flags = flags;
	buf->base.format = format;
	buf->pixmap = pixmap;
	buf->ref = 1;

	return &buf->base;
}

_X_EXPORT
void common_dri2_DestroyBuffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer)
{
	if (buffer) {
		struct common_dri2_buffer *buf = to_common_dri2_buffer(buffer);
		ScreenPtr screen;

		if (--buf->ref != 0)
			return;

		screen = buf->pixmap->drawable.pScreen;
		screen->DestroyPixmap(buf->pixmap);

		free(buf);
	}
}

_X_EXPORT
int common_dri2_GetMSC(DrawablePtr draw, CARD64 *ust, CARD64 *msc)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);
	xf86CrtcPtr crtc = common_dri2_drawable_crtc(draw);
	drmVBlank vbl;
	int ret;

	/* Drawable not displayed, make up a value */
	if (!crtc) {
		*ust = 0;
		*msc = 0;
		return TRUE;
	}

	ret = common_drm_vblank_get(pScrn, crtc, &vbl, __FUNCTION__);
	if (ret)
		return FALSE;

	*ust = ((CARD64)vbl.reply.tval_sec * 1000000) + vbl.reply.tval_usec;
	*msc = vbl.reply.sequence;

	return TRUE;
}

static void common_dri2_waitmsc(struct common_dri2_wait *wait,
	DrawablePtr draw, unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	if (wait->client)
		DRI2WaitMSCComplete(wait->client, draw, frame, tv_sec, tv_usec);
	common_dri2_wait_free(wait);
}

_X_EXPORT
Bool common_dri2_ScheduleWaitMSC(ClientPtr client, DrawablePtr draw,
	CARD64 target_msc, CARD64 divisor, CARD64 remainder)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);
	xf86CrtcPtr crtc;
	struct common_dri2_wait *wait;
	drmVBlank vbl;
	CARD64 cur_msc;
	int ret;

	/*
	 * Truncate to match kernel interfaces; means occasional
	 * overflow misses, but that's generally not a big deal.
	 */
	target_msc &= 0xffffffff;
	divisor &= 0xffffffff;
	remainder &= 0xffffffff;

	crtc = common_dri2_drawable_crtc(draw);
	if (!crtc)
		goto complete;

	wait = common_dri2_wait_alloc(client, draw, DRI2_WAITMSC);
	if (!wait)
		goto complete;

	wait->event_func = common_dri2_waitmsc;

	/* Get current count */
	ret = common_drm_vblank_get(pScrn, crtc, &vbl, __FUNCTION__);
	if (ret)
		goto del_wait;

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

	ret = common_drm_vblank_queue_event(pScrn, crtc, &vbl, __FUNCTION__,
					    FALSE, wait);
	if (ret)
		goto del_wait;

	wait->frame = vbl.reply.sequence;
	DRI2BlockClient(client, draw);
	return TRUE;

 del_wait:
	common_dri2_wait_free(wait);

 complete:
	DRI2WaitMSCComplete(client, draw, target_msc, 0, 0);

	return TRUE;
}

_X_EXPORT
void common_dri2_event(int fd, unsigned frame, unsigned tv_sec,
	unsigned tv_usec, void *event)
{
	struct common_dri2_wait *wait = event;
	DrawablePtr draw;

	if (wait->drawable_id &&
	    dixLookupDrawable(&draw, wait->drawable_id, serverClient, M_ANY,
			      DixWriteAccess) == Success) {
		if (wait->event_func) {
			wait->event_func(wait, draw, frame, tv_sec, tv_usec);
			return;
		}

		xf86DrvMsg(xf86ScreenToScrn(draw->pScreen)->scrnIndex,
			   X_WARNING, "%s: unknown vblank event received\n",
			   __FUNCTION__);
	}
	common_dri2_wait_free(wait);
}

static int common_dri2_client_gone(void *data, XID id)
{
	struct xorg_list *list = data;

	while (!xorg_list_is_empty(list)) {
		struct common_dri2_wait *wait;

		wait = xorg_list_first_entry(list, struct common_dri2_wait,
					     client_list);
		xorg_list_del(&wait->client_list);
		wait->client = NULL;
	}
	free(list);

	return Success;
}

static int common_dri2_drawable_gone(void *data, XID id)
{
	struct xorg_list *list = data;

	while (!xorg_list_is_empty(list)) {
		struct common_dri2_wait *wait;

		wait = xorg_list_first_entry(list, struct common_dri2_wait,
					     drawable_list);
		xorg_list_del(&wait->drawable_list);
		wait->drawable_id = None;
	}
	free(list);

	return Success;
}

_X_EXPORT
Bool common_dri2_ScreenInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	static int dri2_server_generation;

	if (!dri2_register_private())
		return FALSE;

	if (dri2_server_generation == serverGeneration)
		return TRUE;

	dri2_server_generation = serverGeneration;

	dri2_wait_client_restype = CreateNewResourceType(common_dri2_client_gone,
							 "Frame Event Client");
	dri2_wait_drawable_restype = CreateNewResourceType(common_dri2_drawable_gone,
							   "Frame Event Drawable");
	if (!dri2_wait_client_restype || !dri2_wait_drawable_restype) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Can not register DRI2 frame event resources\n");
		return FALSE;
	}

	return TRUE;
}
