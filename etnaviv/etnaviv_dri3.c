/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2015
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* drm includes */
#include <xf86drm.h>

#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "dri3.h"
#include "misyncshm.h"
#include "compat-api.h"

#include <etnaviv/etna.h>

#include "etnaviv_accel.h"
#include "etnaviv_dri3.h"

static Bool etnaviv_dri3_authorise(struct etnaviv *etnaviv, int fd)
{
	struct stat st;
	drm_magic_t magic;

	if (fstat(fd, &st) || !S_ISCHR(st.st_mode))
		return FALSE;

	/*
	 * If the device is a render node, we don't need to auth it.
	 * Render devices start at minor number 128 and up, though it
	 * would be nice to have some other test for this.
	 */
	if (st.st_rdev & 0x80)
		return TRUE;

	return drmGetMagic(fd, &magic) == 0 &&
	       drmAuthMagic(etnaviv->conn->fd, magic) == 0;
}

static int etnaviv_dri3_open(ScreenPtr pScreen, RRProviderPtr provider, int *o)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	int fd;

	fd = open(etnaviv->render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return BadAlloc;

	if (!etnaviv_dri3_authorise(etnaviv, fd)) {
		close(fd);
		return BadMatch;
	}

	*o = fd;

	return Success;
}

static PixmapPtr etnaviv_dri3_pixmap_from_fd(ScreenPtr pScreen, int fd,
	CARD16 width, CARD16 height, CARD16 stride, CARD8 depth, CARD8 bpp)
{
	return etnaviv_pixmap_from_dmabuf(pScreen, fd, width, height,
					  stride, depth, bpp);
}

static int etnaviv_dri3_fd_from_pixmap(ScreenPtr pScreen, PixmapPtr pixmap,
	CARD16 *stride, CARD32 *size)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	/* Only support pixmaps backed by an etnadrm bo */
	if (!vPix || !vPix->etna_bo)
		return BadMatch;

	*stride = pixmap->devKind;
	*size = etna_bo_size(vPix->etna_bo);

	return etna_bo_to_dmabuf(etnaviv->conn, vPix->etna_bo);
}

static dri3_screen_info_rec etnaviv_dri3_info = {
	.version = 0,
	.open = etnaviv_dri3_open,
	.pixmap_from_fd = etnaviv_dri3_pixmap_from_fd,
	.fd_from_pixmap = etnaviv_dri3_fd_from_pixmap,
};

Bool etnaviv_dri3_ScreenInit(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct stat st;
	char buf[64];

	free((void *)etnaviv->render_node);

	if (fstat(etnaviv->conn->fd, &st) || !S_ISCHR(st.st_mode))
		return FALSE;

	snprintf(buf, sizeof(buf), "%s/card%d", DRM_DIR_NAME,
		 (unsigned int)st.st_rdev & 0x7f);

	if (access(buf, F_OK))
		return FALSE;

	etnaviv->render_node = strdup(buf);
	if (!etnaviv->render_node)
		return FALSE;

	if (!miSyncShmScreenInit(pScreen))
		return FALSE;

	return dri3_screen_init(pScreen, &etnaviv_dri3_info);
}
