#ifndef UTILS_H
#define UTILS_H

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#endif

/* Use this version of container_of() - it's safer than others */
#undef container_of
#define container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member ) *__mptr = (ptr); \
	(type *)( (char *)__mptr - offsetof(type,member) );})

#define mint(x,y)	({(void)(&x == &y); x < y ? x : y; })
#define maxt(x,y)	({(void)(&x == &y); x < y ? y : x; })

#endif
