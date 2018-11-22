#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <sys/fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include "xf86.h"

#include "armada_accel.h"
#include "etnaviv_accel.h"
#include "etnadrm.h"

static pointer etnadrm_setup(pointer module, pointer opts, int *errmaj,
	int *errmin)
{
	int fd;

	fd = etnadrm_open_render("etnaviv");
	if (fd != -1) {
		close(fd);
		armada_register_accel(&etnaviv_ops, module, "etnadrm_gpu");
		return (pointer) 1;
	}

	*errmaj = LDR_MODSPECIFIC;
	*errmin = 0;

	return NULL;
}

static XF86ModuleVersionInfo etnadrm_version = {
	.modname = "Etnaviv GPU driver (DRM)",
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

_X_EXPORT XF86ModuleData etnadrm_gpuModuleData = {
	.vers = &etnadrm_version,
	.setup = etnadrm_setup,
};
