#ifndef UTILS_H
#define UTILS_H

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#endif

#define mint(x,y)	({(void)(&x == &y); x < y ? x : y; })
#define maxt(x,y)	({(void)(&x == &y); x < y ? y : x; })

#endif
