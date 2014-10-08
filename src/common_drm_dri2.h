#ifndef COMMON_DRM_DRI2_H
#define COMMON_DRM_DRI2_H

#include "xf86.h"
#include "xf86Crtc.h"
#include "compat-list.h"
#include "dri2.h"
#include "utils.h"
#include <xf86drm.h>

struct common_dri2_buffer {
	DRI2BufferRec base;
	PixmapPtr pixmap;
	unsigned ref;
};

#define to_common_dri2_buffer(x) \
	container_of(x, struct common_dri2_buffer, base)

enum common_dri2_event_type {
	DRI2_SWAP,
	DRI2_SWAP_CHAIN,
	DRI2_FLIP,
	DRI2_WAITMSC,
};

struct common_dri2_wait {
	struct xorg_list drawable_list;
	struct xorg_list client_list;
	XID drawable_id;
	ClientPtr client;

	struct common_dri2_wait *next;
	void (*event_func)(struct common_dri2_wait *wait, DrawablePtr draw,
			   unsigned frame, unsigned tv_sec, unsigned tv_usec);
	enum common_dri2_event_type type;
	xf86CrtcPtr crtc;
	int frame;

	/* For swaps/flips */
	DRI2SwapEventPtr swap_func;
	void *swap_data;
	DRI2BufferPtr front;
	DRI2BufferPtr back;
};

static inline void common_dri2_buffer_reference(DRI2Buffer2Ptr buffer)
{
	to_common_dri2_buffer(buffer)->ref++;
}

static inline DrawablePtr common_dri2_get_drawable(DRI2BufferPtr buffer,
	DrawablePtr drawable)
{
	struct common_dri2_buffer *buf = to_common_dri2_buffer(buffer);

	return buffer->attachment == DRI2BufferFrontLeft ?
		drawable : &buf->pixmap->drawable;
}


struct common_dri2_wait *__common_dri2_wait_alloc(ClientPtr client,
	DrawablePtr draw, enum common_dri2_event_type type, size_t size);

static inline struct common_dri2_wait *common_dri2_wait_alloc(ClientPtr client,
	DrawablePtr draw, enum common_dri2_event_type type)
{
	return __common_dri2_wait_alloc(client, draw, type,
					sizeof(struct common_dri2_wait));
}

void common_dri2_wait_free(struct common_dri2_wait *wait);

xf86CrtcPtr common_dri2_drawable_crtc(DrawablePtr pDraw);

Bool common_dri2_can_flip(DrawablePtr pDraw, struct common_dri2_wait *wait);
Bool common_dri2_may_flip(DrawablePtr pDraw, unsigned int attachment);

void common_dri2_flip_buffers(ScreenPtr pScreen, struct common_dri2_wait *wait);

PixmapPtr common_dri2_create_pixmap(DrawablePtr pDraw, unsigned int attachment,
	unsigned int format, int usage_hint);
DRI2Buffer2Ptr common_dri2_setup_buffer(struct common_dri2_buffer *buf,
	unsigned int attachment, unsigned int format, PixmapPtr pixmap,
	uint32_t name, unsigned int flags);
void common_dri2_DestroyBuffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer);

int common_dri2_GetMSC(DrawablePtr draw, CARD64 *ust, CARD64 *msc);
Bool common_dri2_ScheduleWaitMSC(ClientPtr client, DrawablePtr draw,
	CARD64 target_msc, CARD64 divisor, CARD64 remainder);

void common_dri2_event(int fd, unsigned frame, unsigned tv_sec,
	unsigned tv_usec, void *event);

Bool common_dri2_ScreenInit(ScreenPtr pScreen);

#endif
