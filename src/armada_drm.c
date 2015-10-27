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
#include <unistd.h>

#include <armada_bufmgr.h>

#include "armada_accel.h"
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
#include "utils.h"

#define CURSOR_MAX_WIDTH	64
#define CURSOR_MAX_HEIGHT	32

const OptionInfoRec armada_drm_options[] = {
	{ OPTION_XV_ACCEL,	"XvAccel",	   OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_XV_PREFEROVL,	"XvPreferOverlay", OPTV_BOOLEAN, {0}, TRUE  },
	{ OPTION_USE_GPU,	"UseGPU",	   OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_USE_KMS_BO,	"UseKMSBo",	   OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_ACCEL_MODULE,	"AccelModule",	   OPTV_STRING,  {0}, FALSE },
	{ -1,			NULL,		   OPTV_NONE,    {0}, FALSE }
};

static Bool armada_drm_accel_import(ScreenPtr pScreen, PixmapPtr pixmap,
	struct drm_armada_bo *bo)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	const struct armada_accel_ops *ops = arm->accel_ops;
	uint32_t name;
	Bool ret;
	int fd;

	if (!ops)
		return TRUE;

	if (drm_armada_bo_to_fd(bo, &fd)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "etnaviv: unable to get prime fd for bo: %s\n",
			   strerror(errno));
		return FALSE;
	}

	ret = ops->import_dmabuf(pScreen, pixmap, fd);
	close(fd);

	if (ops->attach_name && !drm_armada_bo_flink(bo, &name))
		ops->attach_name(pScreen, pixmap, name);

	return ret;
}

static Bool armada_drm_ModifyScanoutPixmap(PixmapPtr pixmap,
	int width, int height, struct drm_armada_bo *bo)
{
	ScreenPtr pScreen = pixmap->drawable.pScreen;
	int old_width, old_height, old_devKind;
	void *old_ptr;
	Bool ret;

	old_width = pixmap->drawable.width;
	old_height = pixmap->drawable.height;
	old_devKind = pixmap->devKind;
	old_ptr = pixmap->devPrivate.ptr;

	if (!pScreen->ModifyPixmapHeader(pixmap, width, height, -1, -1,
					 bo->pitch, bo->ptr))
		return FALSE;

	ret = armada_drm_accel_import(pScreen, pixmap, bo);
	if (!ret) {
		assert(pScreen->ModifyPixmapHeader(pixmap, old_width,
						   old_height, -1, -1,
						   old_devKind, old_ptr));
		return FALSE;
	}

	common_drm_set_pixmap_data(pixmap, bo->handle, bo);

	return TRUE;
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

PixmapPtr armada_drm_alloc_dri_scanout(ScreenPtr pScreen, int width,
	int height, int depth)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct drm_armada_bo *bo;
	PixmapPtr pixmap;

	pixmap = pScreen->CreatePixmap(pScreen, 0, 0, depth, 0);
	if (!pixmap)
		return NULL;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pixmap->drawable.bitsPerPixel);
	if (!bo) {
		pScreen->DestroyPixmap(pixmap);
		return NULL;
	}

	if (!armada_drm_ModifyScanoutPixmap(pixmap, width, height, bo)) {
		drm_armada_bo_put(bo);
		pScreen->DestroyPixmap(pixmap);
		return NULL;
	}

	return pixmap;
}

/*
 * CRTC support
 */
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
	ScrnInfoPtr pScrn = crtc->scrn;
	struct drm_armada_bo *bo;

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate shadow memory for rotated CRTC\n");
		return NULL;
	}

	if (!common_drm_crtc_shadow_allocate(crtc, width, height,
					     bo->pitch, bo->handle)) {
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

	common_drm_set_pixmap_data(rotate_pixmap, bo->handle, NULL);

	armada_drm_accel_import(pScrn->pScreen, rotate_pixmap, bo);

	return rotate_pixmap;
}

static void
armada_drm_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rot_pixmap,
	void *data)
{
	if (rot_pixmap) {
		struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(crtc->scrn);
		if (arm->accel_ops)
			arm->accel_ops->free_pixmap(rot_pixmap);
		common_drm_set_pixmap_data(rot_pixmap, 0, NULL);
		FreeScratchPixmapHeader(rot_pixmap);
	}
	if (data) {
		common_drm_crtc_shadow_destroy(crtc);
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
	.set_mode_major = common_drm_crtc_set_mode_major,
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
	ScreenPtr screen = xf86ScrnToScreen(pScrn);
	PixmapPtr pixmap;
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo, *old_bo;
	uint32_t fb_id;
	int ret, displayWidth;

	if (pScrn->virtualX == width && pScrn->virtualY == height)
		return TRUE;

	pixmap = screen->GetScreenPixmap(screen);
	old_bo = common_drm_get_pixmap_data(pixmap);

	bo = armada_bo_alloc_framebuffer(pScrn, width, height,
					 pScrn->bitsPerPixel);
	if (!bo)
		goto err_alloc;

	ret = drmModeAddFB(drm->fd, width, height,
			   pScrn->depth, pScrn->bitsPerPixel,
			   bo->pitch, bo->handle, &fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to add fb: %s\n", strerror(errno));
		goto err_addfb;
	}

	if (!armada_drm_ModifyScanoutPixmap(pixmap, width, height, bo)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to modify screen pixmap: %s\n", strerror(errno));
		goto err_modpix;
	}

	displayWidth = bo->pitch / arm->cpp;
	common_drm_crtc_resize(pScrn, width, height, displayWidth, fb_id);

	drm_armada_bo_put(old_bo);
	return TRUE;

 err_modpix:
	drmModeRmFB(drm->fd, fb_id);
 err_addfb:
	drm_armada_bo_put(bo);
 err_alloc:
	return FALSE;
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
	struct drm_armada_bo *bo = common_drm_get_pixmap_data(pixmap);

	if (arm->front_bo) {
		drm_armada_bo_put(arm->front_bo);
		arm->front_bo = NULL;
	}

	if (bo)
		drm_armada_bo_put(bo);

	pScreen->DestroyPixmap = arm->DestroyPixmap;
	pScreen->CloseScreen = arm->CloseScreen;

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

static Bool armada_drm_CreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	Bool ret;

	pScreen->CreateScreenResources = arm->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	if (ret) {
		PixmapPtr pixmap = pScreen->GetScreenPixmap(pScreen);

		ret = armada_drm_ModifyScanoutPixmap(pixmap, -1, -1,
						     arm->front_bo);

		arm->front_bo = NULL;
	}
	return ret;
}

static Bool armada_drm_DestroyPixmap(PixmapPtr pixmap)
{
	ScreenPtr pScreen = pixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);

	if (pixmap->refcnt == 1) {
		struct drm_armada_bo *bo;

		bo = common_drm_get_pixmap_data(pixmap);
		if (bo)
			drm_armada_bo_put(bo);
	}

	return arm->DestroyPixmap(pixmap);
}

static Bool armada_drm_ScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;
	Bool use_kms_bo;
	Bool ret;

	if (!common_drm_get_master(drm->dev)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] set master failed: %s\n", strerror(errno));
		return FALSE;
	}

	bo = armada_bo_alloc_framebuffer(pScrn, pScrn->virtualX,
					 pScrn->virtualY, pScrn->bitsPerPixel);
	if (!bo)
		return FALSE;

	if (drmModeAddFB(drm->fd, pScrn->virtualX, pScrn->virtualY,
			 pScrn->depth, pScrn->bitsPerPixel, bo->pitch,
			 bo->handle, &drm->fb_id) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to add fb: %s\n", strerror(errno));
		drm_armada_bo_put(bo);
		return FALSE;
	}

	arm->front_bo = bo;
	pScrn->displayWidth = bo->pitch / arm->cpp;

	if (!common_drm_PreScreenInit(pScreen))
		return FALSE;

	arm->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = armada_drm_CreateScreenResources;
	arm->DestroyPixmap = pScreen->DestroyPixmap;
	pScreen->DestroyPixmap = armada_drm_DestroyPixmap;
	arm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = armada_drm_CloseScreen;

	/*
	 * Only pass the armada-drm bo manager if we are really
	 * driving armada-drm, other DRMs don't provide bo managers.
	 */
	use_kms_bo = arm->version && strstr(arm->version->name, "armada");
	if (use_kms_bo)
		use_kms_bo = xf86ReturnOptValBool(arm->Options,
						  OPTION_USE_KMS_BO, TRUE);

	if (arm->accel) {
		struct drm_armada_bufmgr *mgr = arm->bufmgr;

		if (!use_kms_bo)
			mgr = NULL;

		if (!arm->accel_ops->screen_init(pScreen, mgr)) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "[drm] Vivante initialization failed, running unaccelerated\n");
			arm->accel = FALSE;
			arm->accel_ops = NULL;
		}
	}

	if (!common_drm_PostScreenInit(pScreen))
		return FALSE;

	if (xf86ReturnOptValBool(arm->Options, OPTION_XV_ACCEL, TRUE))
		armada_drm_XvInit(pScrn);

	pScrn->vtSema = TRUE;

	ret = common_drm_EnterVT(VT_FUNC_ARGS(0));
	if (!ret)
		pScrn->vtSema = FALSE;

	return ret;
}

static Bool armada_drm_pre_init(ScrnInfoPtr pScrn)
{
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	const char *s;

	xf86CollectOptions(pScrn, NULL);
	arm->Options = malloc(sizeof(armada_drm_options));
	if (!arm->Options)
		return FALSE;
	memcpy(arm->Options, armada_drm_options, sizeof(armada_drm_options));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, arm->Options);

	arm->cpp = (pScrn->bitsPerPixel + 7) / 8;

	arm->accel_ops = NULL;
	arm->accel = xf86ReturnOptValBool(arm->Options, OPTION_USE_GPU, TRUE);
	s = xf86GetOptValString(arm->Options, OPTION_ACCEL_MODULE);

	if (arm->accel) {
		struct common_drm_info *drm = GET_DRM_INFO(pScrn);

		if (!armada_load_accelerator(pScrn, s))
			return FALSE;

		arm->accel_ops = armada_get_accelerator();
		if (arm->accel_ops) {
			if (arm->accel_ops->pre_init &&
			    !arm->accel_ops->pre_init(pScrn, drm->fd)) {
				xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					   "[drm] accel module failed to initialise\n");
				return FALSE;
			}
		} else {
			arm->accel = FALSE;
		}
	}

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

static Bool armada_drm_alloc(ScrnInfoPtr pScrn,
	struct common_drm_device *drm_dev)
{
	struct all_drm_info *drm;
	uint64_t val;
	int err;

	drm = calloc(1, sizeof *drm);
	if (!drm)
		return FALSE;

	drm->common.cursor_max_width = CURSOR_MAX_WIDTH;
	drm->common.cursor_max_height = CURSOR_MAX_HEIGHT;
	drm->common.private = &drm->armada;
	drm->common.fd = drm_dev->fd;
	drm->common.dev = drm_dev;

	if (armada_get_cap(drm->common.fd, DRM_CAP_PRIME, &val,
			   pScrn->scrnIndex, "DRM_CAP_PRIME"))
		goto err_free;
	if (!(val & DRM_PRIME_CAP_EXPORT)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support prime export.\n");
		goto err_free;
	}

	if (armada_get_cap(drm->common.fd, DRM_CAP_DUMB_BUFFER, &val,
			   pScrn->scrnIndex, "DRM_CAP_DUMB_BUFFER"))
		goto err_free;
	if (!val) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] kernel doesn't support dumb buffer.\n");
		goto err_free;
	}

	err = drm_armada_init(drm->common.fd, &drm->armada.bufmgr);
	if (err) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize Armada DRM manager.\n");
		goto err_free;
	}

	SET_DRM_INFO(pScrn, &drm->common);

	drm->armada.version = drmGetVersion(drm->common.fd);
	if (drm->armada.version)
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hardware: %s\n",
			   drm->armada.version->name);

	return TRUE;

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

		if (arm->version)
			drmFreeVersion(arm->version);
	}

	common_drm_FreeScreen(FREE_SCREEN_ARGS(pScrn));
}

static Bool armada_drm_PreInit(ScrnInfoPtr pScrn, int flags)
{
	struct common_drm_device *drm_dev;
	rgb defaultWeight = { 0, 0, 0 };
	int flags24;

	if (pScrn->numEntities != 1)
		return FALSE;

	if (flags & PROBE_DETECT)
		return FALSE;

	/* Get the device we detected at probe time */
	drm_dev = common_entity_get_dev(pScrn->entityList[0]);
	if (!drm_dev)
		return FALSE;

	if (!armada_drm_alloc(pScrn, drm_dev))
		return FALSE;

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
