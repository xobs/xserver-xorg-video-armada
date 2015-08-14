#ifndef ETNAVIV_XV_H
#define ETNAVIV_XV_H

#include "xf86xv.h"

XF86VideoAdaptorPtr etnaviv_xv_init(ScreenPtr, unsigned int *);
void etnaviv_xv_exit(ScreenPtr);

#endif
