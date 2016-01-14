#ifndef ETNAVIV_RENDER_H
#define ETNAVIV_RENDER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#ifdef RENDER
#include "mipict.h"

#include "etnaviv_op.h"

void etnaviv_render_screen_init(ScreenPtr);
void etnaviv_render_close_screen(ScreenPtr);
#else
static inline void etnaviv_render_screen_init(ScreenPtr pScreen)
{
}

static inline void etnaviv_render_close_screen(ScreenPtr pScreen)
{
}
#endif

#endif
