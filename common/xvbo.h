#ifndef XVBO_H
#define XVBO_H

/*
 * This is a special Xv image format used to pass DRM named buffers
 * via the Xv protocol to the backend, allowing for zero copy display.
 *
 * The format of the passed buffer is:
 *  word 0: fourcc of the data contained in the DRM named buffer
 *  word 1: DRM name of the buffer
 *
 * The buffer shall be a minimum of 8 bytes, and shall be in native
 * host endian - since zero-copy only makes sense with an application
 * running on the local machine.
 */
#define FOURCC_XVBO 0x4f425658
#define XVIMAGE_XVBO { \
	FOURCC_XVBO, \
	XvYUV, \
	LSBFirst, \
	{ 0 }, \
	0, \
	XvPlanar, \
	1, \
	0, 0, 0, 0, \
	8, 8, 8, \
	1, 2, 2, \
	1, 1, 1, \
	"I", \
	XvTopToBottom, \
}

#endif
