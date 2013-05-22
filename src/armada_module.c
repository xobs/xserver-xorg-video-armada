/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armada_drm.h"

#define ARMADA_VERSION		4000
#define ARMADA_NAME		"armada"
#define ARMADA_DRIVER_NAME	"armada"

/* Supported "chipsets" */
static SymTabRec armada_chipsets[] = {
//	{  0, "88AP16x" },
	{  0, "88AP510" },
	{ -1, NULL }
};

static void armada_identify(int flags)
{
	xf86PrintChipsets(ARMADA_NAME, "Support for Marvell LCD Controller",
			  armada_chipsets);
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
		int entity;

		entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
		pScrn = xf86ConfigFbEntity(NULL, 0, entity,
					   NULL, NULL, NULL, NULL);

		if (pScrn) {
			foundScreen = TRUE;
		    
			pScrn->driverVersion = ARMADA_VERSION;
			pScrn->driverName    = ARMADA_DRIVER_NAME;
			pScrn->name          = ARMADA_NAME;
			pScrn->Probe         = NULL;

			armada_drm_init_screen(pScrn);
		}
	}

	free(devSections);

	return foundScreen;
}

static const OptionInfoRec *armada_available_options(int chipid, int busid)
{
	extern const OptionInfoRec armada_drm_options[];
	return armada_drm_options;
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
	default:
		return FALSE;
	}
}

_X_EXPORT DriverRec armada_driver = {
	.driverVersion = ARMADA_VERSION,
	.driverName = ARMADA_DRIVER_NAME,
	.Identify = armada_identify,
	.Probe = armada_probe,
	.AvailableOptions = armada_available_options,
	.driverFunc = armada_driver_func,
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
