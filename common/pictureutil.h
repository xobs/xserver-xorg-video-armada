#ifndef PICTUREUTIL_H
#define PICTUREUTIL_H

#include "picture.h"

char *picture_desc(PicturePtr pict, char *str, size_t n);
Bool picture_is_solid(PicturePtr pict, CARD32 *colour);

Bool transform_is_integer_translation(PictTransformPtr t, int *tx, int *ty);

#endif
