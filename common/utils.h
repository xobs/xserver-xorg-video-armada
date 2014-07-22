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

#define __min(tx,ty,x,y) ({		\
	tx _min1 = (x);			\
	ty _min2 = (y);			\
	(void)(&_min1 == &_min2);	\
	_min1 < _min2 ? _min1 : _min2; })

#define __max(tx,ty,x,y) ({		\
	tx _max1 = (x);			\
	ty _max2 = (y);			\
	(void)(&_max1 == &_max2);	\
	_max1 > _max2 ? _max1 : _max2; })

#define mint(x,y)	__min(typeof(x), typeof(y), x, y)
#define min_t(t,x,y)	__min(t, t, x, y)
#define maxt(x,y)	__max(typeof(x), typeof(y), x, y)
#define max_t(t,x,y)	__max(t, t, x, y)

#define ALIGN(v,a)	(((v) + ((a) - 1)) & ~((a) - 1))

#endif
