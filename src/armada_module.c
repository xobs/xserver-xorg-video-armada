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
#include <sys/fcntl.h>
#include <unistd.h>

#include "xf86.h"
#ifdef XSERVER_PLATFORM_BUS
#include "xf86platformBus.h"
#endif

#include "armada_drm.h"
#include "armada_accel.h"
#include "common_drm.h"
#include "utils.h"

#define ARMADA_VERSION		4000
#define ARMADA_NAME		"armada"
#define ARMADA_DRIVER_NAME	"armada"

#define DRM_MODULE_NAMES	"armada-drm", "imx-drm"
#define DRM_DEFAULT_BUS_ID	NULL

static const char *drm_module_names[] = { DRM_MODULE_NAMES };

/* Supported "chipsets" */
static SymTabRec armada_chipsets[] = {
//	{  0, "88AP16x" },
	{  0, "88AP510" },
	{ -1, NULL }
};

static SymTabRec ipu_chipsets[] = {
	{  0, "i.MX6" },
	{ -1, NULL }
};

static const OptionInfoRec * const options[] = {
	armada_drm_options,
	common_drm_options,
};

static const char *armada_drm_accelerators[] = {
#ifdef HAVE_ACCEL_ETNAVIV
	"etnadrm_gpu",
	"etnaviv_gpu",
#endif
#ifdef HAVE_ACCEL_GALCORE
	"vivante_gpu",
#endif
	NULL,
};

struct armada_accel_module {
	const char *name;
	const struct armada_accel_ops *ops;
	pointer module;
};

static struct armada_accel_module *armada_accel_modules;
static unsigned int armada_num_accel_modules;

Bool armada_load_accelerator(ScrnInfoPtr pScrn, const char *module)
{
	unsigned int i;

	if (!module) {
		for (i = 0; armada_drm_accelerators[i]; i++)
			if (xf86LoadSubModule(pScrn, armada_drm_accelerators[i]))
				break;
	} else {
		if (!xf86LoadSubModule(pScrn, module))
			return FALSE;

		if (armada_num_accel_modules == 0)
			return FALSE;
	}

	return TRUE;
}

const struct armada_accel_ops *armada_get_accelerator(void)
{
	return armada_accel_modules ? armada_accel_modules[0].ops : NULL;
}

_X_EXPORT
void armada_register_accel(const struct armada_accel_ops *ops, pointer module,
	const char *name)
{
	unsigned int n = armada_num_accel_modules++;

	armada_accel_modules = xnfrealloc(armada_accel_modules,
					  armada_num_accel_modules *
					  sizeof(*armada_accel_modules));

	armada_accel_modules[n].name = name;
	armada_accel_modules[n].ops = ops;
	armada_accel_modules[n].module = module;
}

static void armada_identify(int flags)
{
	xf86PrintChipsets(ARMADA_NAME, "Support for Marvell LCD Controller",
			  armada_chipsets);
	xf86PrintChipsets(ARMADA_NAME, "Support for Freescale IPU",
			  ipu_chipsets);
}

static void armada_init_screen(ScrnInfoPtr pScrn)
{
	pScrn->driverVersion = ARMADA_VERSION;
	pScrn->driverName    = ARMADA_DRIVER_NAME;
	pScrn->name          = ARMADA_NAME;
	pScrn->Probe         = NULL;

	armada_drm_init_screen(pScrn);
}

static Bool armada_probe(DriverPtr drv, int flags)
{
	GDevPtr *devSections;
	int i, numDevSections;
	Bool foundScreen = FALSE;

	if (flags & PROBE_DETECT)
		return FALSE;

	numDevSections = xf86MatchDevice(ARMADA_DRIVER_NAME, &devSections);
	if (numDevSections <= 0)
		return FALSE;

	for (i = 0; i < numDevSections; i++) {
		ScrnInfoPtr pScrn;
		const char *busid = DRM_DEFAULT_BUS_ID;
		int entity, fd, j;

		if (devSections[i]->busID)
			busid = devSections[i]->busID;

		for (j = 0; j < ARRAY_SIZE(drm_module_names); j++) {
			fd = drmOpen(drm_module_names[j], busid);
			if (fd >= 0)
				break;
		}

		if (fd < 0)
			continue;

		if (!common_drm_fd_is_master(fd))
			continue;

		entity = xf86ClaimNoSlot(drv, 0, devSections[i], TRUE);
		common_alloc_dev(entity, fd, NULL, TRUE);

		pScrn = xf86ConfigFbEntity(NULL, 0, entity,
					   NULL, NULL, NULL, NULL);
		if (!pScrn)
			continue;

		if (busid)
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "Using BusID \"%s\"\n", busid);

		foundScreen = TRUE;
		armada_init_screen(pScrn);
	}

	free(devSections);

	return foundScreen;
}

static const OptionInfoRec *armada_available_options(int chipid, int busid)
{
	static OptionInfoRec opts[32];
	int i, j, k;

	for (i = k = 0; i < ARRAY_SIZE(options); i++) {
		for (j = 0; options[i][j].token != -1; j++) {
			if (k >= ARRAY_SIZE(opts) - 1)
				return NULL;
			opts[k++] = options[i][j];
		}
	}
		
	opts[k].token = -1;
	return opts;
}

static Bool
armada_driver_func(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
	xorgHWFlags *flag;
    
	switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
		flag = (CARD32*)ptr;
		(*flag) = 0;
		return TRUE;
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,902,0)
	case SUPPORTS_SERVER_FDS:
		return TRUE;
#endif
	default:
		return FALSE;
	}
}

#ifdef XSERVER_PLATFORM_BUS
static Bool armada_is_kms(int fd)
{
	drmVersionPtr version;
	drmModeResPtr res;
	Bool has_connectors;

	version = drmGetVersion(fd);
	if (!version)
		return FALSE;
	drmFreeVersion(version);

	res = drmModeGetResources(fd);
	if (!res)
		return FALSE;

	has_connectors = res->count_connectors > 0;
	drmModeFreeResources(res);

	return has_connectors;
}

static struct common_drm_device *armada_create_dev(int entity_num,
	struct xf86_platform_device *dev)
{
	struct common_drm_device *drm_dev;
	const char *path;
	Bool ddx_managed_master;
	int fd, our_fd = -1;

	path = xf86_get_platform_device_attrib(dev, ODEV_ATTRIB_PATH);
	if (!path)
		goto err_free;

#ifdef ODEV_ATTRIB_FD
	fd = xf86_get_platform_device_int_attrib(dev, ODEV_ATTRIB_FD, -1);
#else
	fd = -1;
#endif
	if (fd != -1) {
		ddx_managed_master = FALSE;
		if (!armada_is_kms(fd))
			goto err_free;
	} else {
		ddx_managed_master = TRUE;
		our_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (our_fd == -1)
			goto err_free;

		if (!armada_is_kms(our_fd)) {
			close(our_fd);
			goto err_free;
		}

		if (!common_drm_fd_is_master(our_fd)) {
			close(our_fd);
			goto err_free;
		}

		fd = our_fd;
	}

	/* If we're running unprivileged, don't drop master status */
	if (geteuid())
		ddx_managed_master = FALSE;

	drm_dev = common_alloc_dev(entity_num, fd, path, ddx_managed_master);
	if (!drm_dev && our_fd != -1)
		close(our_fd);

	return drm_dev;

 err_free:
	return NULL;
}

static int armada_create_screen(DriverPtr drv, int entity_num,
	struct common_drm_device *drm_dev)
{
	ScrnInfoPtr pScrn;

	pScrn = xf86AllocateScreen(drv, 0);
	if (!pScrn)
		return FALSE;

	xf86AddEntityToScreen(pScrn, entity_num);

	armada_init_screen(pScrn);

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Added screen for KMS device %s\n", drm_dev->kms_path);

	return TRUE;
}

static Bool armada_platform_probe(DriverPtr drv, int entity_num, int flags,
	struct xf86_platform_device *dev, intptr_t match_data)
{
	struct common_drm_device *drm_dev;

	drm_dev = common_entity_get_dev(entity_num);
	if (!drm_dev)
		drm_dev = armada_create_dev(entity_num, dev);
	if (!drm_dev)
		return FALSE;

	return armada_create_screen(drv, entity_num, drm_dev);
}
#endif

_X_EXPORT DriverRec armada_driver = {
	.driverVersion = ARMADA_VERSION,
	.driverName = ARMADA_DRIVER_NAME,
	.Identify = armada_identify,
	.Probe = armada_probe,
	.AvailableOptions = armada_available_options,
	.driverFunc = armada_driver_func,
#ifdef XSERVER_PLATFORM_BUS
	.platformProbe = armada_platform_probe,
#endif
};

#ifdef XFree86LOADER

static pointer armada_setup(pointer module, pointer opts, int *errmaj,
	int *errmin)
{
	static Bool setupDone = FALSE;

	if (setupDone) {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}

	setupDone = TRUE;

	xf86AddDriver(&armada_driver, module, HaveDriverFuncs);

	return (pointer) 1;
}

static XF86ModuleVersionInfo armada_version = {
	.modname = "armada",
	.vendor = MODULEVENDORSTRING,
	._modinfo1_ = MODINFOSTRING1,
	._modinfo2_ = MODINFOSTRING2,
	.xf86version = XORG_VERSION_CURRENT,
	.majorversion = PACKAGE_VERSION_MAJOR,
	.minorversion = PACKAGE_VERSION_MINOR,
	.patchlevel = PACKAGE_VERSION_PATCHLEVEL,
	.abiclass = ABI_CLASS_VIDEODRV,
	.abiversion = ABI_VIDEODRV_VERSION,
	.moduleclass = MOD_CLASS_VIDEODRV,
	.checksum = { 0, 0, 0, 0 },
};

_X_EXPORT XF86ModuleData armadaModuleData = {
	.vers = &armada_version,
	.setup = armada_setup,
};

#endif /* XFree86LOADER */
