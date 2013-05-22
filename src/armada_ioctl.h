/*
 * Copyright (C) 2012 Russell King
 *  With inspiration from the i915 driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef DRM_ARMADA_IOCTL_H
#define DRM_ARMADA_IOCTL_H

#define DRM_ARMADA_GEM_CREATE		0x00
#define DRM_ARMADA_GEM_CREATE_PHYS	0x01
#define DRM_ARMADA_GEM_MMAP		0x02
#define DRM_ARMADA_GEM_PWRITE		0x03
#define DRM_ARMADA_GEM_PROP		0x04
#define DRM_ARMADA_GEM_CACHE		0x05
#define DRM_ARMADA_OVERLAY_PUT_IMAGE	0x06
#define DRM_ARMADA_OVERLAY_ATTRS		0x07

#define ARMADA_IOCTL(dir,name,str) \
	DRM_##dir(DRM_COMMAND_BASE + DRM_ARMADA_##name, struct drm_armada_##str)

struct drm_armada_gem_create {
	uint32_t height;
	uint32_t width;
	uint32_t bpp;
	uint32_t handle;
	uint32_t pitch;
	uint32_t size;
};
#define DRM_IOCTL_ARMADA_GEM_CREATE \
	ARMADA_IOCTL(IOWR, GEM_CREATE, gem_create)

struct drm_armada_gem_create_phys {
	uint32_t size;
	uint32_t handle;
	uint64_t phys;
};
#define DRM_IOCTL_ARMADA_GEM_CREATE_PHYS \
	ARMADA_IOCTL(IOWR, GEM_CREATE_PHYS, gem_create_phys)

struct drm_armada_gem_mmap {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
	uint64_t size;
	uint64_t addr;
};
#define DRM_IOCTL_ARMADA_GEM_MMAP \
	ARMADA_IOCTL(IOWR, GEM_MMAP, gem_mmap)

struct drm_armada_gem_pwrite {
	uint32_t handle;
	uint32_t offset;
	uint32_t size;
	uint64_t ptr;
};
#define DRM_IOCTL_ARMADA_GEM_PWRITE \
	ARMADA_IOCTL(IOW, GEM_PWRITE, gem_pwrite)

struct drm_armada_gem_prop {
	uint64_t phys;
	uint32_t handle;
};
#define DRM_IOCTL_ARMADA_GEM_PROP \
	ARMADA_IOCTL(IOWR, GEM_PROP, gem_prop)

struct drm_armada_gem_cache {
	uint64_t ptr;
	uint32_t handle;
	uint32_t size;
	uint32_t op;
};
#define DRM_IOCTL_ARMADA_GEM_CACHE \
	ARMADA_IOCTL(IOW, GEM_CACHE, gem_cache)

/* Same as Intel I915 */
struct drm_armada_overlay_put_image {
	uint32_t flags;
#define ARMADA_OVERLAY_TYPE_MASK          0x000000ff
#define ARMADA_OVERLAY_YUV_PLANAR         0x00000001
#define ARMADA_OVERLAY_YUV_PACKED         0x00000002
#define ARMADA_OVERLAY_RGB                0x00000003
#define ARMADA_OVERLAY_DEPTH_MASK		0x0000ff00
#define ARMADA_OVERLAY_RGB24		0x00001000
#define ARMADA_OVERLAY_RGB16		0x00002000
#define ARMADA_OVERLAY_RGB15		0x00003000
#define ARMADA_OVERLAY_YUV422		0x00000100
#define ARMADA_OVERLAY_YUV411		0x00000200
#define ARMADA_OVERLAY_YUV420		0x00000300
#define ARMADA_OVERLAY_YUV410		0x00000400
#define ARMADA_OVERLAY_SWAP_MASK		0x00ff0000
#define ARMADA_OVERLAY_NO_SWAP		0x00000000
#define ARMADA_OVERLAY_UV_SWAP		0x00010000
#define ARMADA_OVERLAY_Y_SWAP		0x00020000
#define ARMADA_OVERLAY_Y_AND_UV_SWAP	0x00030000
#define ARMADA_OVERLAY_FLAGS_MASK		0xff000000
#define ARMADA_OVERLAY_ENABLE		0x01000000
	uint32_t bo_handle;
	uint16_t stride_Y;
	uint16_t stride_UV;
	uint32_t offset_Y;
	uint32_t offset_U;
	uint32_t offset_V;
	uint16_t src_width;
	uint16_t src_height;
	uint16_t src_scan_width;
	uint16_t src_scan_height;
	uint32_t crtc_id;
	uint16_t dst_x;
	uint16_t dst_y;
	uint16_t dst_width;
	uint16_t dst_height;
};
#define DRM_IOCTL_ARMADA_OVERLAY_PUT_IMAGE \
	ARMADA_IOCTL(IOW, OVERLAY_PUT_IMAGE, overlay_put_image)

/* Same as Intel I915 */
struct drm_armada_overlay_attrs {
	uint32_t flags;
#define ARMADA_OVERLAY_UPDATE_ATTRS	(1<<0)
#define ARMADA_OVERLAY_UPDATE_GAMMA	(1<<1)
	uint32_t color_key;
	int32_t brightness;
	uint32_t contrast;
	uint32_t saturation;
	uint32_t gamma0;
	uint32_t gamma1;
	uint32_t gamma2;
	uint32_t gamma3;
	uint32_t gamma4;
	uint32_t gamma5;
};
#define DRM_IOCTL_ARMADA_OVERLAY_ATTRS \
	ARMADA_IOCTL(IOWR, OVERLAY_ATTRS, overlay_attrs)

#endif
