/*
 * Private extensions to galcore to support Other(tm) ways of doing things.
 */
#ifndef GAL_EXTENSION_H
#define GAL_EXTENSION_H

#include <sys/ioctl.h>
#include <gc_hal.h>

/* Map a DMABUF fd into galcore */
struct dmabuf_map {
	unsigned zero;
	unsigned status;
	int fd;
	gctPOINTER Info;
	gctUINT32 Address;
};
#define IOC_GDMABUF_MAP	_IOWR('_', 0, struct dmabuf_map)

#endif
