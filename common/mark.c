#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "mark.h"

static FILE *f;

void __mark(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;

	if (!f)
		f = fopen("/tmp/Xlog", "w");
	if (!f)
		return;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	fprintf(f, "%10ld.%09ld: ", ts.tv_sec, ts.tv_nsec);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
}

void __mark_flush(void)
{
	if (f)
		fflush(f);
}
