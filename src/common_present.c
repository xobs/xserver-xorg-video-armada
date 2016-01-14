#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86drm.h>

#include "common_drm.h"
#include "common_drm_helper.h"

#include "present.h"

struct common_present_event {
	struct common_drm_event base;
	uint64_t event_id;
	struct xorg_list node;
};

static struct xorg_list events = { &events, &events };

static void common_present_handler(struct common_drm_event *base,
	uint64_t msc, unsigned int tv_sec, unsigned int tv_usec)
{
	struct common_present_event *event;

	event = container_of(base, struct common_present_event, base);

	if (!xorg_list_is_empty(&event->node)) {
		present_event_notify(event->event_id,
				     (uint64_t)tv_sec * 1000000 + tv_usec,
				     msc);

		xorg_list_del(&event->node);
	}
	free(event);
}

static RRCrtcPtr common_present_get_crtc(WindowPtr window)
{
	xf86CrtcPtr crtc = common_drm_drawable_covering_crtc(&window->drawable);

	return crtc ? crtc->randr_crtc : NULL;
}

static int common_present_get_ust_msc(RRCrtcPtr rr_crtc, uint64_t *ust,
	uint64_t *msc)
{
	xf86CrtcPtr crtc = rr_crtc->devPrivate;

	return common_drm_get_msc(crtc, ust, msc);
}

static Bool common_present_queue_vblank(RRCrtcPtr rr_crtc, uint64_t event_id,
	uint64_t msc)
{
	xf86CrtcPtr crtc = rr_crtc->devPrivate;
	struct common_present_event *event;
	int ret;

	event = calloc(1, sizeof *event);
	if (!event)
		return BadAlloc;

	event->base.crtc = crtc;
	event->base.handler = common_present_handler;
	event->event_id = event_id;

	ret = common_drm_queue_msc_event(crtc->scrn, crtc, &msc,
					 __FUNCTION__, FALSE, &event->base);
	if (ret == 0) {
		xorg_list_append(&event->node, &events);
		return Success;
	}

	free(event);

	return BadMatch; /* FIXME */
}

static void common_present_abort_vblank(RRCrtcPtr rr_crtc, uint64_t event_id,
	uint64_t msc)
{
	struct common_present_event *event;

	xorg_list_for_each_entry(event, &events, node) {
		if (event->event_id == event_id) {
			/*
			 * Cancel the event by taking it off the events list.
			 * We still expect DRM to trigger the event.
			 */
			xorg_list_del(&event->node);
			xorg_list_init(&event->node);
			break;
		}
	}
}

static void common_present_flush(WindowPtr window)
{
	/* Flush queued rendering - and wait for it? */
}

//static Bool common_present_check_flip(RRCrtcPtr crtc, WindowPtr window,
//	PixmapPtr pixmap, Bool sync_flip)
//{
//}

//static Bool common_present_flip(RRCrtcPtr crtc, uint64_t event_id,
//	uint64_t target_msc, PixmapPtr pixmap, Bool sync_flip)
//{
//}

//static void common_present_unflip(ScreenPtr screen, uint64_t event_id)
//{
//}

static present_screen_info_rec common_present_screen_info = {
	.version = PRESENT_SCREEN_INFO_VERSION,
	.get_crtc = common_present_get_crtc,
	.get_ust_msc = common_present_get_ust_msc,
	.queue_vblank = common_present_queue_vblank,
	.abort_vblank = common_present_abort_vblank,
	.flush = common_present_flush,
	.capabilities = PresentCapabilityNone,
//	.check_flip = common_present_check_flip,
//	.flip = common_present_flip,
//	.unflip = common_present_unflip,
};

Bool common_present_init(ScreenPtr pScreen)
{
	return present_screen_init(pScreen, &common_present_screen_info);
}
