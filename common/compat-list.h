#ifndef COMPAT_LIST_H
#define COMPAT_LIST_H

#include <list.h>

#ifndef xorg_list_entry
#define xorg_list list
#define xorg_list_init list_init
#define xorg_list_del list_del
#define xorg_list_add list_add
#define xorg_list_append list_append
#define xorg_list_is_empty list_is_empty
#define xorg_list_first_entry list_first_entry
#define xorg_list_last_entry list_last_entry
#define xorg_list_for_each_entry list_for_each_entry
#define xorg_list_for_each_entry_safe list_for_each_entry_safe
#endif

#endif
