#ifndef FBUTIL_H
#define FBUTIL_H

#include "fb.h"

static inline Bool fb_full_planemask(DrawablePtr pDrawable, unsigned long mask)
{
	unsigned long fullmask = FbFullMask(pDrawable->depth);

	return (mask & fullmask) == fullmask;
}

#endif
