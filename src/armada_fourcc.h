#ifndef ARMADA_FOURCC_H
#define ARMADA_FOURCC_H

#define GUID4CC(a,b,c,d) { a,b,c,d, 0x00,0x00,0x00,0x10,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71 }

#define FOURCC_VYUY 0x59555956
#define XVIMAGE_VYUY { \
		FOURCC_VYUY, XvYUV, LSBFirst, GUID4CC('V', 'Y', 'U', 'Y'), \
		16, XvPacked, 1,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1, "VYUY", XvTopToBottom, }

#define FOURCC_I422 0x32323449
#define XVIMAGE_I422 { \
		FOURCC_I422, XvYUV, LSBFirst, GUID4CC('I', '4', '2', '2'), \
		16, XvPlanar, 3,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "YUV", XvTopToBottom, }

#define FOURCC_YV16 0x36315659
#define XVIMAGE_YV16 { \
		FOURCC_YV16, XvYUV, LSBFirst, GUID4CC('Y', 'V', '1', '6'), \
		16, XvPlanar, 3,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "YVU", XvTopToBottom, }

#define XVIMAGE_ARGB8888 { \
		DRM_FORMAT_ARGB8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGRA", XvTopToBottom, }

#define XVIMAGE_ABGR8888 { \
		DRM_FORMAT_ABGR8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGBA", XvTopToBottom, }

#define XVIMAGE_XRGB8888 { \
		DRM_FORMAT_XRGB8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_XBGR8888 { \
		DRM_FORMAT_XBGR8888, XvRGB, LSBFirst, { 0 }, \
		32, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define XVIMAGE_RGB888 { \
		DRM_FORMAT_RGB888, XvRGB, LSBFirst, { 0 }, \
		24, XvPacked, 1,  24, 0xff0000, 0x00ff00, 0x0000ff, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_BGR888 { \
		DRM_FORMAT_BGR888, XvRGB, LSBFirst, { 0 }, \
		24, XvPacked, 1,  24, 0x0000ff, 0x00ff00, 0xff0000, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define XVIMAGE_ARGB1555 { \
		DRM_FORMAT_ARGB1555, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  15, 0x7c00, 0x03e0, 0x001f, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGRA", XvTopToBottom, }

#define XVIMAGE_ABGR1555 { \
		DRM_FORMAT_ABGR1555, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  15, 0x001f, 0x03e0, 0x7c00, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGBA", XvTopToBottom, }

#define XVIMAGE_RGB565 { \
		DRM_FORMAT_RGB565, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  16, 0xf800, 0x07e0, 0x001f, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "BGR", XvTopToBottom, }

#define XVIMAGE_BGR565 { \
		DRM_FORMAT_BGR565, XvRGB, LSBFirst, { 0 }, \
		16, XvPacked, 1,  16, 0x001f, 0x07e0, 0xf800, \
		0, 0, 0,  0, 0, 0,  0, 0, 0,  "RGB", XvTopToBottom, }

#define FOURCC_XVBO 0x4f425658
#define XVIMAGE_XVBO { \
		FOURCC_XVBO, XvYUV, LSBFirst, { 0 }, \
		0, XvPlanar, 1,  0, 0, 0, 0, \
		8, 8, 8,  1, 2, 2,  1, 1, 1,  "I", XvTopToBottom, }

#endif
