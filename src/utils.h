#ifndef UTILS_H
#define UTILS_H

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))

#define mint(x,y)	({(void)(&x == &y); x < y ? x : y; })
#define maxt(x,y)	({(void)(&x == &y); x < y ? y : x; })

#endif
