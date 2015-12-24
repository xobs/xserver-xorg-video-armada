#ifndef BO_CACHE_H
#define BO_CACHE_H

#include <sys/time.h>
#include <sys/types.h>
#include <X11/Xdefs.h>
#include "compat-list.h"

/* Number of buckets in the BO cache */
#define NUM_BUCKETS		(3*9 + 3)

struct bo_cache;
struct bo_entry;

struct bo_bucket {
	struct xorg_list head;
	size_t size;
};

struct bo_cache {
	struct bo_bucket buckets[NUM_BUCKETS];
	struct xorg_list head;
	time_t last_cleaned;
	void (*free)(struct bo_entry *);
};

struct bo_entry {
	struct bo_bucket *bucket;
	struct xorg_list bucket_node;
	struct xorg_list free_node;
	time_t free_time;
};

void bo_cache_init(struct bo_cache *cache, void (*free)(struct bo_entry *));
void bo_cache_fini(struct bo_cache *cache);
struct bo_bucket *bo_cache_bucket_find(struct bo_cache *cache, size_t size);
struct bo_entry *bo_cache_bucket_get(struct bo_bucket *bucket);
void bo_cache_clean(struct bo_cache *cache, time_t time);
void bo_cache_put(struct bo_cache *cache, struct bo_entry *entry);

#endif
