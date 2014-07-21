#ifndef MARK_H
#define MARK_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

void __mark(const char *fmt, ...)
	__attribute__ ((__format__ (__printf__, 1, 2)));
void __mark_flush(void);

#ifdef DEBUG_MARK_LOG
#define mark(x, arg...) __mark("%s: " x, __FUNCTION__, ##arg)
#define mark_flush()	__mark_flush()
#else
#define mark(x, arg...)	if (0) __mark("%s: " x, __FUNCTION__, ##arg)
#define mark_flush()	do { } while (0)
#endif

#endif
