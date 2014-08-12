#ifndef PAMDUMP_H
#define PAMDUMP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

void dump_pam(const uint32_t *ptr, unsigned pitch, bool alpha,
	unsigned x1, unsigned y1, unsigned x2, unsigned y2,
	const char *fmt, ...)
	__attribute__ ((__format__ (__printf__, 8, 9)));

#endif
