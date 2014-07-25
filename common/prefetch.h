#ifndef PREFETCH_H
#define PREFETCH_H

#if (__GNUC__ > 3) || (__GNUC__ == 3 && __GNUC_MINOR > 10)
#define prefetch(ptr)	__builtin_prefetch(ptr, 0)
#define prefetchw(ptr)	__builtin_prefetch(ptr, 1)
#else
#define prefetch(ptr)
#define prefetchw(ptr)
#endif

#endif
