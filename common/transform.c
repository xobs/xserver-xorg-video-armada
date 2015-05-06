#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "picturestr.h"
#include "pictureutil.h"

Bool transform_is_integer_translation(PictTransformPtr t, int *tx, int *ty)
{
	if (t == NULL) {
		*tx = *ty = 0;
		return TRUE;
	}

	if (t->matrix[0][0] != IntToxFixed(1) ||
	    t->matrix[0][1] != 0 ||
	    t->matrix[1][0] != 0 ||
	    t->matrix[1][1] != IntToxFixed(1) ||
	    t->matrix[2][0] != 0 ||
	    t->matrix[2][1] != 0 ||
	    t->matrix[2][2] != IntToxFixed(1))
		return FALSE;

	if (xFixedFrac(t->matrix[0][2]) != 0 ||
	    xFixedFrac(t->matrix[1][2]) != 0)
		return FALSE;

	*tx = xFixedToInt(t->matrix[0][2]);
	*ty = xFixedToInt(t->matrix[1][2]);

	return TRUE;
}
