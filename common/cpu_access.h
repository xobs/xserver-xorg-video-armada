#ifndef CPU_ACCESS_H
#define CPU_ACCESS_H

enum {
	CPU_ACCESS_RO,
	CPU_ACCESS_RW,
};

void finish_cpu_drawable(DrawablePtr pDrawable, int access);
void prepare_cpu_drawable(DrawablePtr pDrawable, int access);

#endif
