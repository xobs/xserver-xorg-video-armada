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

#include <list.h>

#ifndef xorg_list_entry
#define xorg_list list
#define xorg_list_init list_init
#define xorg_list_del list_del
#define xorg_list_add list_add
#define xorg_list_append list_append
#define xorg_list_is_empty list_is_empty
#define xorg_list_first_entry list_first_entry
#define xorg_list_for_each_entry list_for_each_entry
#define xorg_list_for_each_entry_safe list_for_each_entry_safe
#endif

#endif
