#ifndef BOXUTIL_H
#define BOXUTIL_H

#include "config.h"
#include <X11/Xprotostr.h>
#include "miscstruct.h"
#include "utils.h"

static inline Bool __box_intersect(BoxPtr out, const BoxRec *a, const BoxRec *b)
{
	out->x1 = maxt(a->x1, b->x1);
	out->y1 = maxt(a->y1, b->y1);
	out->x2 = mint(a->x2, b->x2);
	out->y2 = mint(a->y2, b->y2);
	return out->x1 >= out->x2 || out->y1 >= out->y2;
}

static inline void box_intersect(BoxPtr out, const BoxRec *a, const BoxRec *b)
{
	if (__box_intersect(out, a, b))
		out->x1 = out->x2 = out->y1 = out->y2 = 0;
}

static inline int box_area(BoxPtr box)
{
	return (int)(box->x2 - box->x1) * (int)(box->y2 - box->y1);
}

int box_intersect_line_rough(const BoxRec *b, const xSegment *seg);

#endif
