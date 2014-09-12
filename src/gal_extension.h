/*
 * Private extensions to galcore to support Other(tm) ways of doing things.
 */
#ifndef GAL_EXTENSION_H
#define GAL_EXTENSION_H

#include <sys/ioctl.h>
#include <gc_hal.h>

/* Map a DMABUF fd into galcore */
struct dmabuf_map_old {
	unsigned zero;
	unsigned status;
	int fd;
	gctPOINTER Info;
	gctUINT32 Address;
};
#define IOC_GDMABUF_MAP_OLD	_IOWR('_', 0, struct dmabuf_map_old)

union gcabi_header {
	uint32_t padding[16];
	struct {
		uint32_t zero;
		uint32_t status;
	} v2;
	struct {
		uint32_t zero;
		uint32_t hwtype;
		uint32_t status;
	} v4;
};

struct dmabuf_map {
	union gcabi_header hdr;
	uint64_t info;
	uint64_t address;
	int32_t fd;
	uint32_t prot;
};
#define IOC_GDMABUF_MAP		_IOWR('_', 0, struct dmabuf_map)

#endif
