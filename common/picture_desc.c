#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "picturestr.h"
#include "pixmaputil.h"
#include "pictureutil.h"

char *picture_desc(PicturePtr pict, char *str, size_t n)
{
	char *format, fmtbuf[20];
	size_t l;

	if (!pict) {
		snprintf(str, n, "None");
		return str;
	}

	if (!pict->pDrawable) {
		snprintf(str, n, "Source-only");
		return str;
	}

	switch (pict->format) {
	case PICT_a2r10g10b10:
		format = "ARGB2101010";
		break;
	case PICT_x2r10g10b10:
		format = "XRGB2101010";
		break;
	case PICT_a2b10g10r10:
		format = "ABGR2101010";
		break;
	case PICT_x2b10g10r10:
		format = "XBGR2101010";
		break;
	case PICT_a8r8g8b8:
		format = "ARGB8888";
		break;
	case PICT_x8r8g8b8:
		format = "XRGB8888";
		break;
	case PICT_a8b8g8r8:
		format = "ABGR8888";
		break;
	case PICT_x8b8g8r8:
		format = "XBGR8888";
		break;
	case PICT_b8g8r8a8:
		format = "BGRA8888";
		break;
	case PICT_b8g8r8x8:
		format = "BGRX8888";
		break;
	case PICT_r8g8b8:
		format = "RGB888";
		break;
	case PICT_b8g8r8:
		format = "BGR888";
		break;
	case PICT_r5g6b5:
		format = "RGB565";
		break;
	case PICT_b5g6r5:
		format = "BGR565";
		break;
	case PICT_a1r5g5b5:
		format = "ARGB1555";
		break;
	case PICT_x1r5g5b5:
		format = "XRGB1555";
		break;
	case PICT_a1b5g5r5:
		format = "ABGR1555";
		break;
	case PICT_x1b5g5r5:
		format = "XBGR1555";
		break;
	case PICT_a4r4g4b4:
		format = "ARGB4444";
		break;
	case PICT_x4r4g4b4:
		format = "XRGB4444";
		break;
	case PICT_a4b4g4r4:
		format = "ABGR4444";
		break;
	case PICT_x4b4g4r4:
		format = "XBGR4444";
		break;
	case PICT_a8:
		format = "A8";
		break;
	case PICT_r3g3b2:
		format = "RGB332";
		break;
	case PICT_b2g3r3:
		format = "BGR233";
		break;
	case PICT_a2r2g2b2:
		format = "ARGB2222";
		break;
	case PICT_a2b2g2r2:
		format = "ABGR2222";
		break;
	case PICT_c8:
		format = "C8";
		break;
	case PICT_g8:
		format = "G8";
		break;
	case PICT_x4a4:
		format = "XA44";
		break;
	case PICT_a4:
		format = "A4";
		break;
	case PICT_r1g2b1:
		format = "RGB121";
		break;
	case PICT_b1g2r1:
		format = "BGR121";
		break;
	case PICT_a1r1g1b1:
		format = "ARGB1111";
		break;
	case PICT_a1b1g1r1:
		format = "ABGR1111";
		break;
	case PICT_c4:
		format = "C4";
		break;
	case PICT_g4:
		format = "A4";
		break;
	case PICT_a1:
		format = "A1";
		break;
	case PICT_g1:
		format = "G1";
		break;
	default:
		snprintf(fmtbuf, sizeof(fmtbuf), "0x%x", (int)pict->format);
		format = fmtbuf;
		break;
	}

	drawable_desc(pict->pDrawable, str, n);
	l = strlen(str);
	snprintf(str + l, n - l, "%s fmt %s%s%s",
		 pict->repeat ? " R" : "",
		 format,
		 pict->alphaMap ? "/AM" :"",
		 pict->componentAlpha ? "/CA" : "");

	return str;
}
