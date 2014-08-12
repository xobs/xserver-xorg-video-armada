#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/fcntl.h>

#include "pamdump.h"

void dump_pam(const uint32_t *ptr, unsigned pitch, bool alpha,
	unsigned x1, unsigned y1, unsigned x2, unsigned y2,
	const char *fmt, ...)
{
	unsigned char buf[16*1024 + 16384];
	unsigned x, y, i;
	char fn[160];
	va_list ap;
	int fd, len;

	va_start(ap, fmt);
	len = vsnprintf(fn, sizeof(fn), fmt, ap);
	va_end(ap);
	if (len < 0 || len >= sizeof(fn))
		return;

	fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return;

	i = sprintf((char *)buf, "P7\n"
		"WIDTH %u\n"
		"HEIGHT %u\n"
		"DEPTH %u\n"
		"MAXVAL 255\n"
		"TUPLTYPE RGB%s\n"
		"ENDHDR\n",
		x2 - x1, y2 - y1, 3 + alpha, alpha ? "_ALPHA" : "");

	for (y = y1; y < y2; y++) {
		const uint32_t *p = (const uint32_t *)((const char *)ptr + (y * pitch));
		for (x = x1; x < x2; x++) {
			buf[i++] = p[x] >> 16; // R
			buf[i++] = p[x] >> 8;  // G
			buf[i++] = p[x];       // B
			if (alpha)
				buf[i++] = p[x] >> 24; // A
		}
		if (i >= 16*1024) {
			write(fd, buf, i);
			i = 0;
		}
	}
	if (i)
		write(fd, buf, i);
	close(fd);
}
