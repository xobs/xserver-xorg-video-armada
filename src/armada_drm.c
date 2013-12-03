/*
 * Marvell Armada DRM-based driver
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
#include "common_drm.h"
#include "xf86_OSproc.h"
#include "xf86Crtc.h"
#include "xf86cmap.h"
#include "fb.h"
#include "micmap.h"
#include <xf86DDC.h>
#include <X11/extensions/dpmsconst.h>
#include <X11/Xatom.h>

#include "compat-api.h"
#include "vivante.h"
#include "vivante_dri2.h"

#define CURSOR_MAX_WIDTH	64
#define CURSOR_MAX_HEIGHT	32

#define DRM_MODULE_NAME		"armada-drm"
#define DRM_DEFAULT_BUS_ID	NULL

enum {
	OPTION_XV_ACCEL,
	OPTION_USE_GPU,
};

const OptionInfoRec armada_drm_options[] = {
	{ OPTION_XV_ACCEL,	"XvAccel",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_USE_GPU,	"UseGPU",	OPTV_BOOLEAN, {0}, FALSE },
	{ -1,			NULL,		OPTV_NONE,    {0}, FALSE }
};

static void armada_drm_ModifyScreenPixmap(ScreenPtr pScreen,
	struct armada_drm_info *arm, int width, int height, int depth, int bpp,
	struct drm_armada_bo *bo)
{
	PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);

	pScreen->ModifyPixmapHeader(pixmap, width, height, depth, bpp,
				    bo->pitch, bo->ptr);
	if (arm->accel)
		vivante_set_pixmap_bo(pixmap, bo);
}

static struct drm_armada_bo *armada_bo_alloc_framebuffer(ScrnInfoPtr pScrn,
	int width, int height, int bpp)
{
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;
	int ret;

	bo = drm_armada_bo_dumb_create(arm->bufmgr, width, height, bpp);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to allocate new bo: %s\n",
			   strerror(errno));
		return NULL;
	}

	ret = drm_armada_bo_map(bo);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to map fb bo: %s\n", strerror(errno));
		drm_armada_bo_put(bo);
		return NULL;
	}

//	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//		   "Allocated new frame buffer %dx%d stride %d\n",
//		   width, height, bo->pitch);

	return bo;
}

/*
 * CRTC support
 */
static Bool
armada_drm_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
	Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);

	if (drm->fb_id == 0) {
		if (drmModeAddFB(drm->fd,
				 pScrn->virtualX, pScrn->virtualY,
				 pScrn->depth, pScrn->bitsPerPixel,
				 arm->front_bo->pitch, arm->front_bo->handle,
				 &drm->fb_id) < 0) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "[drm] failed to add fb: %s\n",
				   strerror(errno));
			return FALSE;
		}
	}

	return common_drm_crtc_set_mode_major(crtc, mode, rotation, x, y);
}

static void armada_drm_crtc_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
	struct common_crtc_info *drmc = common_crtc(crtc);
	struct common_drm_info *drm = GET_DRM_INFO(crtc->scrn);

	drm_armada_bo_subdata(drmc->cursor_data, 0,
			      drm->cursor_max_width *
			      drm->cursor_max_height * 4, image);
}

static void *
armada_drm_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
	struct common_crtc_info *drmc = common_crtc(crtc);
	ScrnInfoPtr pScrn = crtc->scrn;
	struct drm_armada_bo *bo;
	int ret;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow memory for rotated CRTC\n");
		return NULL;
	}

	ret = drmModeAddFB(drmc->drm_fd, width, height, pScrn->depth,
			   pScrn->bitsPerPixel, bo->pitch, bo->handle,
			   &drmc->rotate_fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to add rotate fb: %s\n",
			   strerror(errno));
		drm_armada_bo_put(bo);
		return NULL;
	}

	return bo;
}

static PixmapPtr
armada_drm_crtc_shadow_create(xf86CrtcPtr crtc, void *data,
	int width, int height)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	PixmapPtr rotate_pixmap;
	struct drm_armada_bo *bo;

	if (!data)
		data = armada_drm_crtc_shadow_allocate(crtc, width, height);
	if (!data) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow pixmap data for rotated CRTC\n");
		return NULL;
	}

	bo = data;

	rotate_pixmap = GetScratchPixmapHeader(pScrn->pScreen, width, height,
					   pScrn->depth, pScrn->bitsPerPixel,
					   bo->pitch, bo->ptr);
	if (!rotate_pixmap) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow pixmap for rotated CRTC\n");
		return NULL;
	}

	if (arm->accel)
		vivante_set_pixmap_bo(rotate_pixmap, bo);

	return rotate_pixmap;
}

static void
armada_drm_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rot_pixmap,
	void *data)
{
	if (rot_pixmap) {
		struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(crtc->scrn);
		if (arm->accel)
			vivante_free_pixmap(rot_pixmap);
		FreeScratchPixmapHeader(rot_pixmap);
	}
	if (data) {
		struct common_crtc_info *drmc = common_crtc(crtc);

		drmModeRmFB(drmc->drm_fd, drmc->rotate_fb_id);
		drmc->rotate_fb_id = 0;

		drm_armada_bo_put(data);
	}
}

static void armada_drm_crtc_destroy(xf86CrtcPtr crtc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	if (drmc->cursor_data) {
		drmModeSetCursor(drmc->drm_fd, drmc->mode_crtc->crtc_id,
				 0, 0, 0);
		drm_armada_bo_put(drmc->cursor_data);
	}
	drmModeFreeCrtc(drmc->mode_crtc);

	free(drmc);
}

static const xf86CrtcFuncsRec drm_crtc_funcs = {
	.dpms = common_drm_crtc_dpms,
	.gamma_set = common_drm_crtc_gamma_set,
	.set_mode_major = armada_drm_crtc_set_mode_major,
	.set_cursor_position = common_drm_crtc_set_cursor_position,
	.show_cursor = common_drm_crtc_show_cursor,
	.hide_cursor = common_drm_crtc_hide_cursor,
	.load_cursor_argb = armada_drm_crtc_load_cursor_argb,
	.shadow_create = armada_drm_crtc_shadow_create,
	.shadow_allocate = armada_drm_crtc_shadow_allocate,
	.shadow_destroy = armada_drm_crtc_shadow_destroy,
	.destroy = armada_drm_crtc_destroy,
};

static void armada_drm_crtc_alloc_cursors(ScrnInfoPtr pScrn)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		struct common_crtc_info *drmc = common_crtc(crtc);
		struct drm_armada_bo *bo;

		bo = drm_armada_bo_dumb_create(arm->bufmgr,
					       drm->cursor_max_width,
					       drm->cursor_max_height,
					       32);

		if (bo) {
			drmc->cursor_handle = bo->handle;
			drmc->cursor_data = bo;
		} else {
			drm->has_hw_cursor = FALSE;
			break;
		}
	}
}

static Bool armada_drm_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	ScreenPtr screen = screenInfo.screens[pScrn->scrnIndex];
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo, *old_bo;
	uint32_t fb_id;
	int ret, displayWidth;

	if (pScrn->virtualX == width && pScrn->virtualY == height)
		return TRUE;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo)
		return FALSE;

	ret = drmModeAddFB(drm->fd, width, height,
			   pScrn->depth, pScrn->bitsPerPixel,
			   bo->pitch, bo->handle, &fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to add fb: %s\n", strerror(errno)); 
		drm_armada_bo_put(bo);
		return FALSE;
	}

	old_bo = arm->front_bo;
	arm->front_bo = bo;

	armada_drm_ModifyScreenPixmap(screen, arm, width, height, -1, -1, bo);

	displayWidth = bo->pitch / arm->cpp;
	common_drm_crtc_resize(pScrn, width, height, displayWidth, fb_id);

	drm_armada_bo_put(old_bo);
	return TRUE;
}

static const xf86CrtcConfigFuncsRec armada_drm_config_funcs = {
	armada_drm_xf86crtc_resize,
};

static ModeStatus
armada_drm_ValidMode(SCRN_ARG_TYPE arg1, DisplayModePtr mode, Bool verbose,
		     int flags)
{
	SCRN_INFO_PTR(arg1);

	if (mode->Flags & V_DBLSCAN) {
		if (verbose)
			xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
				   "Removing double-scanned mode \"%s\"\n",
				   mode->name);
		return MODE_BAD;
	}

	return MODE_OK;
}

static Bool armada_drm_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);

	if (arm->front_bo) {
		drm_armada_bo_put(arm->front_bo);
		arm->front_bo = NULL;
	}

	if (arm->accel)
		vivante_free_pixmap(pixmap);

	pScreen->CloseScreen = arm->CloseScreen;

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

static Bool armada_drm_CreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	Bool ret;

	pScreen->CreateScreenResources = arm->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	if (ret) {
		struct drm_armada_bo *bo = arm->front_bo;

		armada_drm_ModifyScreenPixmap(pScreen, arm, -1, -1, -1, -1, bo);
	}
	return ret;
}

static Bool armada_drm_ScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;

	if (drmSetMaster(drm->fd)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] set master failed: %s\n", strerror(errno));
		return FALSE;
	}

	arm->accel = xf86ReturnOptValBool(arm->Options, OPTION_USE_GPU, TRUE);

	bo = armada_bo_alloc_framebuffer(pScrn, pScrn->virtualX,
					 pScrn->virtualY, pScrn->bitsPerPixel);
	if (!bo)
		return FALSE;

	arm->front_bo = bo;
	pScrn->displayWidth = bo->pitch / arm->cpp;

	if (!common_drm_PreScreenInit(pScreen))
		return FALSE;

	arm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = armada_drm_CloseScreen;
	arm->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = armada_drm_CreateScreenResources;

	if (arm->accel && !vivante_ScreenInit(pScreen, arm->bufmgr)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] Vivante initialization failed, running unaccelerated\n");
		arm->accel = FALSE;
	}

	if (!common_drm_PostScreenInit(pScreen))
		return FALSE;

	if (xf86ReturnOptValBool(arm->Options, OPTION_XV_ACCEL, TRUE))
		armada_drm_XvInit(pScrn);

	pScrn->vtSema = TRUE;

	return common_drm_EnterVT(VT_FUNC_ARGS(0));
}

static Bool armada_drm_pre_init(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);

	xf86CollectOptions(pScrn, NULL);
	arm->Options = malloc(sizeof(armada_drm_options));
	if (!arm->Options)
		return FALSE;
	memcpy(arm->Options, armada_drm_options, sizeof(armada_drm_options));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, arm->Options);

	arm->cpp = (pScrn->bitsPerPixel + 7) / 8;

	xf86CrtcConfigInit(pScrn, &armada_drm_config_funcs);

	if (!common_drm_init_mode_resources(pScrn, &drm_crtc_funcs))
		return FALSE;

	armada_drm_crtc_alloc_cursors(pScrn);

	return TRUE;
}

static int armada_get_cap(int fd, uint64_t cap, uint64_t *val, int scrnIndex,
	const char *name)
{
	int err;

	err = drmGetCap(fd, cap, val);
	if (err)
		xf86DrvMsg(scrnIndex, X_ERROR,
			   "[drm] failed to get %s capability: %s\n",
			   name, strerror(errno));

	return err;
}

static Bool armada_drm_open_master(ScrnInfoPtr pScrn)
{
	struct all_drm_info *drm;
	EntityInfoPtr pEnt;
	drmSetVersion sv;
	drmVersionPtr version;
	const char *busid = DRM_DEFAULT_BUS_ID;
	uint64_t val;
	int err;

	pEnt = xf86GetEntityInfo(pScrn->entityList[0]);
	if (pEnt) {
		if (pEnt->device->busID)
			busid = pEnt->device->busID;
		free(pEnt);
	}

	if (busid)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Using BusID \"%s\"\n",
			   busid);

	drm = calloc(1, sizeof *drm);
	if (!drm)
		return FALSE;

	drm->common.cursor_max_width = CURSOR_MAX_WIDTH;
	drm->common.cursor_max_height = CURSOR_MAX_HEIGHT;
	drm->common.private = &drm->armada;
	drm->common.event_context.version = DRM_EVENT_CONTEXT_VERSION;
#ifdef HAVE_DRI2
	drm->common.event_context.vblank_handler = vivante_dri2_vblank;
#endif

	drm->common.fd = drmOpen(DRM_MODULE_NAME, busid);
	if (drm->common.fd < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] Failed to open DRM device for %s: %s\n",
			   busid, strerror(errno));
		goto err_free;
	}

	/*
	 * Check that what we opened was a master or a master-capable FD
	 * by setting the version of the interface we'll use to talk to it.
	 */
	sv.drm_di_major = 1;
	sv.drm_di_minor = 1;
	sv.drm_dd_major = -1;
	sv.drm_dd_minor = -1;
	err = drmSetInterfaceVersion(drm->common.fd, &sv);
	if (err) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set DRM interface version: %s\n",
			   strerror(errno));
		goto err_drm_close;
	}

	if (armada_get_cap(drm->common.fd, DRM_CAP_PRIME, &val,
			   pScrn->scrnIndex, "DRM_CAP_PRIME"))
		goto err_drm_close;
	if (!(val & DRM_PRIME_CAP_EXPORT)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support prime export.\n");
		goto err_drm_close;
	}

	if (armada_get_cap(drm->common.fd, DRM_CAP_DUMB_BUFFER, &val,
			   pScrn->scrnIndex, "DRM_CAP_DUMB_BUFFER"))
		goto err_drm_close;
	if (!val) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support dumb buffer.\n");
		goto err_drm_close;
	}

	err = drm_armada_init(drm->common.fd, &drm->armada.bufmgr);
	if (err) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize Armada DRM manager.\n");
		goto err_drm_close;
	}

	SET_DRM_INFO(pScrn, &drm->common);

	version = drmGetVersion(drm->common.fd);
	if (version) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hardware: %s\n",
			   version->name);
		drmFreeVersion(version);
	}

	return TRUE;

 err_drm_close:
	drmClose(drm->common.fd);
 err_free:
	free(drm);
	return FALSE;
}

static void armada_drm_FreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	if (pScrn->driverPrivate) {
		struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);

		drm_armada_fini(arm->bufmgr);
	}

	common_drm_FreeScreen(FREE_SCREEN_ARGS(pScrn));
}

static Bool armada_drm_PreInit(ScrnInfoPtr pScrn, int flags)
{
	rgb defaultWeight = { 0, 0, 0 };
	int flags24;

	if (pScrn->numEntities != 1)
		return FALSE;

	if (flags & PROBE_DETECT)
		return FALSE;

	if (!armada_drm_open_master(pScrn)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to become DRM master.\n");
		return FALSE;
	}

	/* Limit the maximum framebuffer size to 16MB */
	pScrn->videoRam = 16 * 1048576;
	pScrn->monitor = pScrn->confScreen->monitor;
	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "fbdev";
	pScrn->displayWidth = 640;

	flags24 = Support24bppFb | Support32bppFb | SupportConvert24to32;
	if (!xf86SetDepthBpp(pScrn, 0, 0, 0, flags24))
		goto fail;

	switch (pScrn->depth) {
	case 8:
	case 15:
	case 16:
	case 24:
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Given depth (%d) is not supported.\n",
			   pScrn->depth);
		goto fail;
	}

	xf86PrintDepthBpp(pScrn);

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
		goto fail;
	if (!xf86SetDefaultVisual(pScrn, -1))
		goto fail;

	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Requested default visual  (%s) is not supported at depth %d\n",
			   xf86GetVisualName(pScrn->defaultVisual),
			   pScrn->depth);
		goto fail;
	}

	if (!armada_drm_pre_init(pScrn))
		goto fail;

	return TRUE;

 fail:
	armada_drm_FreeScreen(FREE_SCREEN_ARGS(pScrn));
	return FALSE;
}

Bool armada_drm_init_screen(ScrnInfoPtr pScrn)
{
	pScrn->PreInit = armada_drm_PreInit;
	pScrn->ScreenInit = armada_drm_ScreenInit;
	pScrn->SwitchMode = common_drm_SwitchMode;
	pScrn->AdjustFrame = common_drm_AdjustFrame;
	pScrn->EnterVT = common_drm_EnterVT;
	pScrn->LeaveVT = common_drm_LeaveVT;
	pScrn->FreeScreen = armada_drm_FreeScreen;
	pScrn->ValidMode = armada_drm_ValidMode;

	return TRUE;
}
