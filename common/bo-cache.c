#include <stdlib.h>
#include <time.h>

#include "bo-cache.h"

/* The interval in seconds between cache cleans */
#define BO_CACHE_CLEAN_INTERVAL 1
/* The maximum age in seconds of a BO in the cache */
#define BO_CACHE_MAX_AGE	2

/*
 * These sizes come from the i915 DRM backend - which uses roughly
 * for n = 2..
 *   (4096 << n) + (4096 << n) * 1 / 4
 *   (4096 << n) + (4096 << n) * 2 / 4
 *   (4096 << n) + (4096 << n) * 3 / 4
 * The reasoning being that powers of two are too wasteful in X.
 */
static size_t bucket_size[NUM_BUCKETS] = {
	   4096,	   8192,	  12288,
	  20480,	  24576,	  28672,
	  40960,	  49152,	  57344,
	  81920,	  98304,	 114688,
	 163840,	 196608,	 229376,
	 327680,	 393216,	 458752,
	 655360,	 786432,	 917504,
	1310720,	1572864,	1835008,
	2621440,	3145728,	3670016,
};

void bo_cache_init(struct bo_cache *cache, void (*free)(struct bo_entry *))
{
	struct timespec time;
	unsigned i;

	clock_gettime(CLOCK_MONOTONIC, &time);

	cache->free = free;
	cache->last_cleaned = time.tv_sec;
	xorg_list_init(&cache->head);

	for (i = 0; i < NUM_BUCKETS; i++) {
		xorg_list_init(&cache->buckets[i].head);
		cache->buckets[i].size = bucket_size[i];
	}
}

void bo_cache_fini(struct bo_cache *cache)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);

	/* Free all entries by winding time forward */
	bo_cache_clean(cache, time.tv_sec + BO_CACHE_MAX_AGE + 1);
}

struct bo_bucket *bo_cache_bucket_find(struct bo_cache *cache, size_t size)
{
	unsigned i;

	for (i = 0; i < NUM_BUCKETS; i++) {
		struct bo_bucket *bucket = &cache->buckets[i];

		if (bucket->size >= size)
			return bucket;
	}

	return NULL;
}

struct bo_entry *bo_cache_bucket_get(struct bo_bucket *bucket)
{
	struct bo_entry *be = NULL;

	if (!xorg_list_is_empty(&bucket->head)) {
		be = xorg_list_entry(bucket->head.next, struct bo_entry,
				     bucket_node);

		xorg_list_del(&be->bucket_node);
		xorg_list_del(&be->free_node);
	}

	return be;
}

void bo_cache_clean(struct bo_cache *cache, time_t time)
{
	if (time - cache->last_cleaned < BO_CACHE_CLEAN_INTERVAL)
		return;

	cache->last_cleaned = time;

	while (!xorg_list_is_empty(&cache->head)) {
		struct bo_entry *entry;

		entry = xorg_list_entry(&cache->head, struct bo_entry,
					free_node);
		if (time - entry->free_time < BO_CACHE_MAX_AGE)
			break;

		xorg_list_del(&entry->bucket_node);
		xorg_list_del(&entry->free_node);

		cache->free(entry);
	}
}

void bo_cache_put(struct bo_cache *cache, struct bo_entry *entry)
{
	struct bo_bucket *bucket = entry->bucket;
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	entry->free_time = time.tv_sec;
	xorg_list_append(&entry->bucket_node, &bucket->head);
	xorg_list_append(&entry->free_node, &cache->head);

	bo_cache_clean(cache, time.tv_sec);
}
