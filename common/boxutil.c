#include "boxutil.h"
#include "utils.h"

int box_intersect_line_rough(const BoxRec *b, const xSegment *seg)
{
	/*
	 * First, check whether shadow of the line on the
	 * x/y axis are outside the box shadow.
	 */
	if (b->x1 > maxt(seg->x1, seg->x2) || b->x2 < mint(seg->x1, seg->x2) ||
	    b->y1 > maxt(seg->y1, seg->y2) || b->y2 < mint(seg->y1, seg->y2))
		return FALSE;

	return TRUE;
}
