#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <errno.h>
#include <unistd.h>
#include "xf86.h"
#include "utils.h"

#include "armada_accel.h"
#include "etnaviv_accel.h"

static const char *dev_names[] = {
	"/dev/gal3d",
	"/dev/galcore",
	"/dev/graphics/galcore",
};

static pointer etnaviv_setup(pointer module, pointer opts, int *errmaj,
	int *errmin)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dev_names); i++) {
		/*
		 * Test for the presence of the special device,
		 * fail to load if it isn't present.
		 */
		if (access(dev_names[i], R_OK|W_OK) == 0) {
			armada_register_accel(&etnaviv_ops, module, "etnaviv_gpu");
			return (pointer) 1;
		}

		if (errno == ENOENT)
			continue;

		LogMessage(X_ERROR, "access(%s) failed: %s\n",
			   dev_names[i], strerror(errno));
	}

	*errmaj = LDR_NOHARDWARE;
	*errmin = 0;

	return NULL;
}

static XF86ModuleVersionInfo etnaviv_version = {
	.modname = "Etnaviv GPU driver",
	.vendor = MODULEVENDORSTRING,
	._modinfo1_ = MODINFOSTRING1,
	._modinfo2_ = MODINFOSTRING2,
	.xf86version = XORG_VERSION_CURRENT,
	.majorversion = PACKAGE_VERSION_MAJOR,
	.minorversion = PACKAGE_VERSION_MINOR,
	.patchlevel = PACKAGE_VERSION_PATCHLEVEL,
	.abiclass = ABI_CLASS_ANSIC,
	.abiversion = ABI_ANSIC_VERSION,
	.moduleclass = MOD_CLASS_NONE,
	.checksum = { 0, 0, 0, 0 },
};

_X_EXPORT XF86ModuleData etnaviv_gpuModuleData = {
	.vers = &etnaviv_version,
	.setup = etnaviv_setup,
};
