/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef VIVANTE_COMPAT_H
#define VIVANTE_COMPAT_H

#ifndef container_of
#define container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member ) *__mptr = (ptr); \
	(type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#if HAS_DEVPRIVATEKEYREC
#define vivante_CreateKey(key, type) dixRegisterPrivateKey(key, type, 0)
#define vivante_GetKeyPriv(dp, key)  dixGetPrivate(dp, key)
#define vivante_Key                  DevPrivateKeyRec
#else
#define vivante_CreateKey(key, type) dixRequestPrivate(key, 0)
#define vivante_GetKeyPriv(dp, key)  dixLookupPrivate(dp, key)
#define vivante_Key                  int
#endif

#endif
