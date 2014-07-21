#ifndef PICTUREUTIL_H
#define PICTUREUTIL_H

#include "picture.h"

Bool picture_is_solid(PicturePtr pict, CARD32 *colour);

Bool transform_is_integer_translation(PictTransformPtr t, int *tx, int *ty);

#endif
