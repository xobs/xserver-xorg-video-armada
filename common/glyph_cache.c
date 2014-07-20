#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "xf86.h"
#include "fb.h"
#include "mipict.h"
#include "compat-api.h"

#include "glyph_cache.h"
#include "utils.h"

#define CACHE_PICTURE_SIZE	1024
#define GLYPH_MIN_SIZE		8
#define GLYPH_MAX_SIZE		64
#define GLYPH_RATIO_SIZE	(GLYPH_MAX_SIZE / GLYPH_MIN_SIZE)
#define GLYPH_CACHE_SIZE \
	(CACHE_PICTURE_SIZE * CACHE_PICTURE_SIZE / \
	 (GLYPH_MIN_SIZE * GLYPH_MIN_SIZE))

struct glyph_cache {
	PicturePtr picture;
	GlyphPtr *glyphs;
	uint16_t count, evict;
	glyph_upload_t upload;
};

struct glyph_cache_priv {
	CloseScreenProcPtr CloseScreen;
	unsigned num_caches;
	struct glyph_cache cache[0];
};

struct glyph_priv {
	struct glyph_cache *cache;
	xPoint pos;
	uint16_t size, index;
};

static DevPrivateKeyRec glyph_key;

static struct glyph_priv *glyph_get_priv(GlyphPtr glyph)
{
	return dixGetPrivate(&glyph->devPrivates, &glyph_key);
}

static void glyph_set_priv(GlyphPtr glyph, struct glyph_priv *priv)
{
	dixSetPrivate(&glyph->devPrivates, &glyph_key, priv);
}

static DevPrivateKeyRec glyph_cache_key;

static struct glyph_cache_priv *glyph_cache_get_priv(ScreenPtr pScreen)
{
	return dixGetPrivate(&pScreen->devPrivates, &glyph_cache_key);
}

static void glyph_cache_set_priv(ScreenPtr pScreen,
	struct glyph_cache_priv *priv)
{
	dixSetPrivate(&pScreen->devPrivates, &glyph_cache_key, priv);
}

static PicturePtr create_picture(ScreenPtr pScreen, int width, int height,
	int depth, PictFormatPtr pPictFormat, unsigned usage_hint)
{
	PicturePtr picture;
	PixmapPtr pixmap;
	CARD32 component_alpha;
	int error;

	pixmap = pScreen->CreatePixmap(pScreen, width, height, depth,
				       usage_hint);
	if (!pixmap)
		return NULL;

	component_alpha = NeedsComponent(pPictFormat->format);
	picture = CreatePicture(0, &pixmap->drawable, pPictFormat,
				CPComponentAlpha, &component_alpha,
				serverClient, &error);
	pScreen->DestroyPixmap(pixmap);

	return picture;
}

static void glyph_cache_fini(ScreenPtr pScreen)
{
	struct glyph_cache_priv *priv = glyph_cache_get_priv(pScreen);
	unsigned i;

	for (i = 0; i < priv->num_caches; i++) {
		struct glyph_cache *cache = &priv->cache[i];
		if (cache->picture)
			FreePicture(cache->picture, 0);
		if (cache->glyphs)
			free(cache->glyphs);
	}
	glyph_cache_set_priv(pScreen, NULL);
	free(priv);
}

static Bool glyph_cache_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	struct glyph_cache_priv *priv = glyph_cache_get_priv(pScreen);

	pScreen->CloseScreen = priv->CloseScreen;

	glyph_cache_fini(pScreen);

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

Bool glyph_cache_init(ScreenPtr pScreen, glyph_upload_t upload,
	const unsigned *formats, size_t num_formats, unsigned usage_hint)
{
	struct glyph_cache_priv *priv;
	unsigned i;
	size_t size;

	if (!dixRegisterPrivateKey(&glyph_cache_key, PRIVATE_SCREEN, 0))
		return FALSE;
	if (!dixRegisterPrivateKey(&glyph_key, PRIVATE_GLYPH, 0))
		return FALSE;

	size = offsetof(struct glyph_cache_priv, cache[num_formats]);

	priv = malloc(size);
	if (!priv)
		return FALSE;

	memset(priv, 0, size);
	priv->num_caches = num_formats;

	glyph_cache_set_priv(pScreen, priv);

	for (i = 0; i < priv->num_caches; i++) {
		struct glyph_cache *cache = &priv->cache[i];
		PictFormatPtr pPictFormat;
		PicturePtr picture;
		unsigned format = formats[i];
		int depth = PIXMAN_FORMAT_DEPTH(format);

		pPictFormat = PictureMatchFormat(pScreen, depth, format);
		if (!pPictFormat)
			goto fail;

		picture = create_picture(pScreen, CACHE_PICTURE_SIZE,
					 CACHE_PICTURE_SIZE, depth,
					 pPictFormat, usage_hint);
		if (!picture)
			goto fail;

		ValidatePicture(picture);

		cache->picture = picture;
		cache->glyphs = calloc(GLYPH_CACHE_SIZE, sizeof(*cache->glyphs));
		if (!cache->glyphs)
			goto fail;

		cache->evict = rand() % GLYPH_CACHE_SIZE;
		cache->upload = upload;
	}

	priv->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = glyph_cache_CloseScreen;

	return TRUE;

fail:
	glyph_cache_fini(pScreen);
	return FALSE;
}

static unsigned glyph_size_to_count(unsigned size)
{
	size /= GLYPH_MIN_SIZE;
	return size * size;
}

static unsigned glyph_count_to_mask(unsigned count)
{
	return ~(count - 1);
}

static unsigned glyph_size_to_mask(unsigned size)
{
	return glyph_count_to_mask(glyph_size_to_count(size));
}

static struct glyph_cache *glyph_get_cache(ScreenPtr pScreen, GlyphPtr pGlyph)
{
	PicturePtr pGlyphPicture;
	struct glyph_cache_priv *priv = glyph_cache_get_priv(pScreen);
	unsigned i;

	if (!priv)
		return NULL;

	pGlyphPicture = GetGlyphPicture(pGlyph, pScreen);

	for (i = 0; i < priv->num_caches; i++) {
		struct glyph_cache *cache = &priv->cache[i];

		if (PICT_FORMAT_RGB(cache->picture->format) ==
		    PICT_FORMAT_RGB(pGlyphPicture->format))
			return cache;
	}

	return NULL;
}

static struct glyph_priv *__glyph_cache(ScreenPtr pScreen, GlyphPtr pGlyph)
{
	struct glyph_cache *cache;
	struct glyph_priv *priv;
	unsigned size, sz, mask, count, index, i;

	sz = pGlyph->info.width;
	if (sz < pGlyph->info.height)
		sz = pGlyph->info.height;

	if (sz > GLYPH_MAX_SIZE)
		return NULL;

	cache = glyph_get_cache(pScreen, pGlyph);
	if (!cache)
		return NULL;

	for (size = GLYPH_MIN_SIZE; size <= GLYPH_MAX_SIZE; size *= 2)
		if (sz <= size)
			break;

	count = glyph_size_to_count(size);
	mask = glyph_count_to_mask(count);

	index = (cache->count + count - 1) & mask;
	if (index < GLYPH_CACHE_SIZE) {
		cache->count = index + count;
		priv = NULL;
	} else {
		priv = NULL;
		for (count = size; count <= GLYPH_MAX_SIZE; count *= 2) {
			unsigned i = cache->evict & glyph_size_to_mask(count);
			GlyphPtr evicted = cache->glyphs[i];

			if (!evicted)
				continue;

			priv = glyph_get_priv(evicted);
			if (priv->size >= count) {
				glyph_set_priv(evicted, NULL);
				cache->glyphs[i] = NULL;
				index = cache->evict & glyph_size_to_mask(size);
			} else {
				priv = NULL;
			}
			break;
		}

		if (!priv) {
			unsigned s;

			count = glyph_size_to_count(size);
			mask = glyph_count_to_mask(count);

			index = cache->evict & mask;

			for (s = 0; s < count; s++) {
				GlyphPtr evicted = cache->glyphs[index + s];

				if (!evicted)
					continue;

				priv = glyph_get_priv(evicted);
				glyph_set_priv(evicted, NULL);
				cache->glyphs[index + s] = NULL;
			}
		}

		cache->evict = rand() % GLYPH_CACHE_SIZE;
	}

	if (!priv)
		priv = malloc(sizeof(*priv));
	if (!priv)
		return NULL;

	glyph_set_priv(pGlyph, priv);
	cache->glyphs[index] = pGlyph;

	priv->cache = cache;
	priv->size = size;
	priv->index = index;
	i = index / (GLYPH_RATIO_SIZE * GLYPH_RATIO_SIZE);
	priv->pos.x = i % (CACHE_PICTURE_SIZE / GLYPH_MAX_SIZE) * GLYPH_MAX_SIZE;
	priv->pos.y = (i / (CACHE_PICTURE_SIZE / GLYPH_MAX_SIZE)) * GLYPH_MAX_SIZE;
	for (i = GLYPH_MIN_SIZE; i < GLYPH_MAX_SIZE; i *= 2) {
		if (index & 1)
			priv->pos.x += i;
		if (index & 2)
			priv->pos.y += i;
		index >>= 2;
	}

	cache->upload(pScreen, cache->picture, pGlyph,
		      GetGlyphPicture(pGlyph, pScreen),
		      priv->pos.x, priv->pos.y);

	return priv;
}

PicturePtr glyph_cache_only(ScreenPtr pScreen, GlyphPtr pGlyph, xPoint *pos)
{
	struct glyph_priv *priv;

	priv = glyph_get_priv(pGlyph);
	if (priv) {
		*pos = priv->pos;
		return priv->cache->picture;
	}

	return NULL;
}

PicturePtr glyph_cache(ScreenPtr pScreen, GlyphPtr pGlyph, xPoint *pos)
{
	struct glyph_priv *priv;

	priv = glyph_get_priv(pGlyph);
	if (!priv)
		priv = __glyph_cache(pScreen, pGlyph);
	if (priv) {
		*pos = priv->pos;
		return priv->cache->picture;
	}

	pos->x = 0;
	pos->y = 0;

	return GetGlyphPicture(pGlyph, pScreen);
}

/* Pre-load glyphs into the glyph cache before we start rendering. */
Bool glyph_cache_preload(ScreenPtr pScreen, int nlist, GlyphListPtr list,
	GlyphPtr *glyphs)
{
	while (nlist--) {
		int n = list->len;

		while (n--) {
			GlyphPtr glyph = *glyphs++;

			if (glyph->info.width == 0 || glyph->info.height == 0)
				continue;

			if (glyph_get_priv(glyph))
				continue;

			if (!__glyph_cache(pScreen, glyph))
				return FALSE;
		}
		list++;
	}
	return TRUE;
}
