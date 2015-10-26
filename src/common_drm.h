#ifndef COMMON_DRM_H
#define COMMON_DRM_H

#include "xf86Crtc.h"
#include "xf86drmMode.h"
#include "compat-api.h"

struct common_drm_event;

struct common_crtc_info {
	int drm_fd;
	unsigned num;
	drmModeCrtcPtr mode_crtc;
	void *cursor_data;
	uint32_t cursor_handle;
	uint32_t rotate_fb_id;
};
#define common_crtc(crtc) \
	((struct common_crtc_info *)(crtc)->driver_private)

struct drm_udev_info {
	struct udev_monitor *monitor;
	pointer *handler;
	dev_t drm_dev;
	CloseScreenProcPtr CloseScreen;
};

struct common_drm_info {
	int fd;
	uint32_t fb_id;
	drmModeResPtr mode_res;
	drmEventContext event_context;

	struct common_drm_event *flip_event;
	xf86CrtcPtr flip_ref_crtc;
	unsigned int flip_count;
	unsigned int flip_tv_sec;
	unsigned int flip_tv_usec;
	uint64_t flip_msc;
	uint32_t flip_old_fb_id;

	Bool has_hw_cursor;
	Bool hw_cursor;
	unsigned short cursor_max_width;
	unsigned short cursor_max_height;

	int shadow_present;

#ifdef HAVE_UDEV
	struct drm_udev_info udev;
#endif

	OptionInfoPtr Options;
	CloseScreenProcPtr CloseScreen;

	void *private;
};

struct common_drm_event {
	struct common_drm_info *drm;
	xf86CrtcPtr crtc;
	void (*handler)(struct common_drm_event *, uint64_t msc,
			unsigned int tv_sec, unsigned int tv_usec);
};

extern const OptionInfoRec common_drm_options[];

#define GET_DRM_INFO(pScrn)		((struct common_drm_info *)(pScrn)->driverPrivate)
#define SET_DRM_INFO(pScrn, ptr)	((pScrn)->driverPrivate = (ptr))

void common_drm_set_pixmap_data(PixmapPtr pixmap, uint32_t handle, void *data);
void *common_drm_get_pixmap_data(PixmapPtr pixmap);

void common_drm_crtc_dpms(xf86CrtcPtr crtc, int mode);
Bool common_drm_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
	Rotation rotation, int x, int y);
void common_drm_crtc_resize(ScrnInfoPtr pScrn, int width, int height,
	int displayWidth, uint32_t fb_id);
void common_drm_crtc_gamma_set(xf86CrtcPtr crtc,
	CARD16 *red, CARD16 *green, CARD16 *blue, int size);
void common_drm_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y);
void common_drm_crtc_show_cursor(xf86CrtcPtr crtc);
void common_drm_crtc_hide_cursor(xf86CrtcPtr crtc);
Bool common_drm_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height,
	uint32_t pitch, uint32_t handle);
void common_drm_crtc_shadow_destroy(xf86CrtcPtr crtc);

Bool common_drm_init_mode_resources(ScrnInfoPtr pScrn,
	const xf86CrtcFuncsRec *funcs);

Bool common_drm_flip(ScrnInfoPtr pScrn, PixmapPtr pixmap,
	struct common_drm_event *event, xf86CrtcPtr ref_crtc);
void common_drm_flip_pixmap(ScreenPtr pScreen, PixmapPtr a, PixmapPtr b);

void common_drm_LoadPalette(ScrnInfoPtr pScrn, int num, int *indices,
	LOCO *colors, VisualPtr pVisual);
Bool common_drm_PreScreenInit(ScreenPtr pScreen);
Bool common_drm_PostScreenInit(ScreenPtr pScreen);
Bool common_drm_SwitchMode(SWITCH_MODE_ARGS_DECL);
void common_drm_AdjustFrame(ADJUST_FRAME_ARGS_DECL);
Bool common_drm_EnterVT(VT_FUNC_ARGS_DECL);
void common_drm_LeaveVT(VT_FUNC_ARGS_DECL);

void common_drm_FreeScreen(FREE_SCREEN_ARGS_DECL);

#endif
