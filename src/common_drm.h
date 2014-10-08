#ifndef COMMON_DRM_H
#define COMMON_DRM_H

#include "xf86Crtc.h"
#include "compat-api.h"

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
	drmEventContext event_context;
	uint32_t fb_id;
	drmModeResPtr mode_res;

	Bool has_hw_cursor;
	Bool hw_cursor;
	unsigned short cursor_max_width;
	unsigned short cursor_max_height;

#ifdef HAVE_UDEV
	struct drm_udev_info udev;
#endif

	OptionInfoPtr Options;
	CloseScreenProcPtr CloseScreen;

	void *private;
};

#define GET_DRM_INFO(pScrn)		((struct common_drm_info *)(pScrn)->driverPrivate)
#define SET_DRM_INFO(pScrn, ptr)	((pScrn)->driverPrivate = (ptr))

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
