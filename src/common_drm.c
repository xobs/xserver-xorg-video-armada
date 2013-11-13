/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "xf86.h"
#include "common_drm.h"
#include "xf86_OSproc.h"
#include "xf86Crtc.h"
#include "xf86cmap.h"
#include "fb.h"
#include "micmap.h"
#include <xf86DDC.h>
#include <X11/extensions/dpmsconst.h>
#include <X11/Xatom.h>

#ifdef HAVE_UDEV
#include <libudev.h>
#endif

struct common_drm_property {
	drmModePropertyPtr mode_prop;
	uint64_t value;
	int natoms;
	Atom *atoms;
};

static void drmmode_ConvertFromKMode(ScrnInfoPtr pScrn,
	drmModeModeInfoPtr kmode, DisplayModePtr mode)
{
	memset(mode, 0, sizeof(*mode));

	mode->status = MODE_OK;
	mode->Clock = kmode->clock;
	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;
	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;
	mode->Flags = kmode->flags;
	mode->name = strdup(kmode->name);
	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;
	xf86SetModeCrtc (mode, pScrn->adjustFlags);
}

static drmModePropertyPtr common_drm_conn_find_property(
	struct armada_conn_info *conn, const char *name, uint32_t *blob)
{
	drmModeConnectorPtr koutput = conn->mode_output;
	int i;

	for (i = 0; i < koutput->count_props; i++) {
		drmModePropertyPtr p;

		p = drmModeGetProperty(conn->drm_fd, koutput->props[i]);
		if (!p || (blob && !(p->flags & DRM_MODE_PROP_BLOB)))
			continue;

		if (!strcmp(p->name, name)) {
			if (blob)
				*blob = koutput->prop_values[i];
			return p;
		}

		drmModeFreeProperty(p);
	}
	return NULL;
}

static void common_drm_conn_create_resources(xf86OutputPtr output)
{
	struct armada_conn_info *conn = output->driver_private;
	drmModeConnectorPtr mop = conn->mode_output;
	int i, j, n, err;

	conn->props = calloc(mop->count_props, sizeof *conn->props);
	if (!conn->props)
		return;

	for (i = n = 0; i < mop->count_props; i++) {
		struct common_drm_property *prop = &conn->props[n];
		drmModePropertyPtr dprop;
		Bool immutable;

		dprop = drmModeGetProperty(conn->drm_fd, mop->props[i]);
		if (!dprop || dprop->flags & DRM_MODE_PROP_BLOB ||
		    !strcmp(dprop->name, "DPMS") ||
		    !strcmp(dprop->name, "EDID")) {
			drmModeFreeProperty(dprop);
			continue;
		}

		n++;
		prop->mode_prop = dprop;
		prop->value = mop->prop_values[i];

		immutable = dprop->flags & DRM_MODE_PROP_IMMUTABLE ?
				TRUE : FALSE;

		if (dprop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];
			uint32_t value = prop->value;

			prop->natoms = 1;
			prop->atoms = calloc(prop->natoms, sizeof *prop->atoms);
			if (!prop->atoms)
				continue;

			range[0] = dprop->values[0];
			range[1] = dprop->values[1];

			prop->atoms[0] = MakeAtom(dprop->name,
						  strlen(dprop->name), TRUE);
			err = RRConfigureOutputProperty(output->randr_output,
							prop->atoms[0], FALSE,
							TRUE, immutable, 2,
							range);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error %d\n",
					   err);

			err = RRChangeOutputProperty(output->randr_output,
						     prop->atoms[0],
						     XA_INTEGER, 32,
					             PropModeReplace, 1,
						     &value, FALSE, TRUE);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error %d\n",
					   err);
		} else if (dprop->flags & DRM_MODE_PROP_ENUM) {
			int current;

			prop->natoms = dprop->count_enums + 1;
			prop->atoms = calloc(prop->natoms, sizeof *prop->atoms);
			if (!prop->atoms)
				continue;

			current = prop->natoms;
			prop->atoms[0] = MakeAtom(dprop->name,
						  strlen(dprop->name), TRUE);
			for (j = 1; j < prop->natoms; j++) {
				struct drm_mode_property_enum *e;

				e = &dprop->enums[j - 1];
				prop->atoms[j] = MakeAtom(e->name,
							  strlen(e->name),
							  TRUE);
				if (prop->value == e->value)
					current = j;
			}

			err = RRConfigureOutputProperty(output->randr_output,
						 prop->atoms[0], FALSE, FALSE,
						 immutable, prop->natoms - 1,
						 (INT32 *)&prop->atoms[1]);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error, %d\n",
					   err);

			err = RRChangeOutputProperty(output->randr_output,
						     prop->atoms[0], XA_ATOM,
						     32, PropModeReplace, 1,
						     &prop->atoms[current],
						     FALSE, TRUE);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error, %d\n",
					   err);
		}
	}
	conn->nprops = n;
}

static void common_drm_conn_dpms(xf86OutputPtr output, int mode)
{
	struct armada_conn_info *conn = output->driver_private;
	drmModePropertyPtr p = common_drm_conn_find_property(conn, "DPMS", NULL);

	if (p) {
		drmModeConnectorSetProperty(conn->drm_fd, conn->drm_id,
					    p->prop_id, mode);
		conn->dpms_mode = mode;
		drmModeFreeProperty(p);
	}
}

static xf86OutputStatus common_drm_conn_detect(xf86OutputPtr output)
{
	struct armada_conn_info *conn = output->driver_private;
	xf86OutputStatus status = XF86OutputStatusUnknown;
	drmModeConnectorPtr koutput;

	koutput = drmModeGetConnector(conn->drm_fd, conn->drm_id);
	if (!koutput)
		return XF86OutputStatusUnknown;

	drmModeFreeConnector(conn->mode_output);
	conn->mode_output = koutput;

	switch (koutput->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	case DRM_MODE_UNKNOWNCONNECTION:
		break;
	}
	return status;
}

static Bool
common_drm_conn_mode_valid(xf86OutputPtr output, DisplayModePtr pModes)
{
	return MODE_OK;
}

static DisplayModePtr common_drm_conn_get_modes(xf86OutputPtr output)
{
	ScrnInfoPtr pScrn = output->scrn;
	struct armada_conn_info *conn = output->driver_private;
	drmModePropertyBlobPtr edid = NULL;
	DisplayModePtr modes = NULL;
	drmModePropertyPtr p;
	xf86MonPtr mon;
	uint32_t blob;
	int i;

	p = common_drm_conn_find_property(conn, "EDID", &blob);
	if (p) {
		edid = drmModeGetPropertyBlob(conn->drm_fd, blob);
		drmModeFreeProperty(p);
	}

	mon = xf86InterpretEDID(pScrn->scrnIndex, edid ? edid->data : NULL);
	if (mon && edid->length > 128)
		mon->flags |= MONITOR_EDID_COMPLETE_RAWDATA;
	xf86OutputSetEDID(output, mon);

	/* modes should already be available */
	for (i = 0; i < conn->mode_output->count_modes; i++) {
		DisplayModePtr mode = xnfalloc(sizeof *mode);

		drmmode_ConvertFromKMode(pScrn, &conn->mode_output->modes[i], mode);
		modes = xf86ModesAdd(modes, mode);
	}

	return modes;
}

#ifdef RANDR_12_INTERFACE
static Bool common_drm_conn_set_property(xf86OutputPtr output, Atom property,
	RRPropertyValuePtr value)
{
	struct armada_conn_info *conn = output->driver_private;
	int i;

	for (i = 0; i < conn->nprops; i++) {
		struct common_drm_property *prop = &conn->props[i];
		drmModePropertyPtr dprop;

		if (prop->atoms[0] != property)
			continue;

		dprop = prop->mode_prop;
		if (dprop->flags & DRM_MODE_PROP_RANGE) {
			if (value->type != XA_INTEGER || value->format != 32 || value->size != 1)
				return FALSE;

			drmModeConnectorSetProperty(conn->drm_fd, conn->drm_id,
					dprop->prop_id, (uint64_t)*(uint32_t *)value->data);

			return TRUE;
		} else if (dprop->flags & DRM_MODE_PROP_ENUM) {
			Atom atom;
			const char *name;
			int j;

			if (value->type != XA_ATOM || value->format != 32 || value->size != 1)
				return FALSE;

			memcpy(&atom, value->data, sizeof(atom));
			name = NameForAtom(atom);
			if (name == NULL)
				return FALSE;

			for (j = 0; j < dprop->count_enums; j++) {
				if (!strcmp(dprop->enums[j].name, name)) {
					drmModeConnectorSetProperty(conn->drm_fd,
						conn->drm_id, dprop->prop_id,
						dprop->enums[j].value);
					return TRUE;
				}
			}
			return FALSE;
		}
	}
	return TRUE;
}
#endif
#ifdef RANDR_13_INTERFACE
static Bool common_drm_conn_get_property(xf86OutputPtr output, Atom property)
{
	return FALSE;
}
#endif

static void common_drm_conn_destroy(xf86OutputPtr output)
{
	struct armada_conn_info *conn = output->driver_private;

	drmModeFreeConnector(conn->mode_output);
	drmModeFreeEncoder(conn->mode_encoder);
	free(conn);

	output->driver_private = NULL;
}

static const xf86OutputFuncsRec drm_output_funcs = {
	.create_resources = common_drm_conn_create_resources,
	.dpms = common_drm_conn_dpms,
	.detect = common_drm_conn_detect,
	.mode_valid = common_drm_conn_mode_valid,
	.get_modes = common_drm_conn_get_modes,
#ifdef RANDR_12_INTERFACE
	.set_property = common_drm_conn_set_property,
#endif
#ifdef RANDR_13_INTERFACE
	.get_property = common_drm_conn_get_property,
#endif
	.destroy = common_drm_conn_destroy,
};

static const char *const output_names[] = {
	"None", "VGA", "DVI", "DVI", "DVI", "Composite", "TV",
	"LVDS", "CTV", "DIN", "DP", "HDMI", "HDMI",
};

static const int subpixel_conv_table[] = {
	0, SubPixelUnknown, SubPixelHorizontalRGB, SubPixelHorizontalBGR,
	SubPixelVerticalRGB, SubPixelVerticalBGR, SubPixelNone
};

void common_drm_conn_init(ScrnInfoPtr pScrn, int fd, uint32_t id)
{
	drmModeConnectorPtr koutput;
	drmModeEncoderPtr kencoder;
	xf86OutputPtr output;
	struct armada_conn_info *conn;
	char name[32];

	koutput = drmModeGetConnector(fd, id);
	if (!koutput)
		return;

	kencoder = drmModeGetEncoder(fd, koutput->encoders[0]);
	if (!kencoder) {
		drmModeFreeConnector(koutput);
		return;
	}

	snprintf(name, sizeof(name), "%s%d",
		 output_names[koutput->connector_type],
		 koutput->connector_type_id);

	output = xf86OutputCreate(pScrn, &drm_output_funcs, name);
	if (!output) {
		drmModeFreeEncoder(kencoder);
		drmModeFreeConnector(koutput);
		return;
	}

	conn = calloc(1, sizeof *conn);
	if (!conn) {
		xf86OutputDestroy(output);
		drmModeFreeEncoder(kencoder);
		drmModeFreeConnector(koutput);
		return;
	}

	conn->drm_fd = fd;
	conn->drm_id = id;
	conn->mode_output = koutput;
	conn->mode_encoder = kencoder;

	output->driver_private = conn;
	output->mm_width = koutput->mmWidth;
	output->mm_height = koutput->mmHeight;
	output->subpixel_order = subpixel_conv_table[koutput->subpixel];
	output->possible_crtcs = kencoder->possible_crtcs;
	output->possible_clones = kencoder->possible_clones;
	output->interlaceAllowed = 1; /* wish there was a way to read that */
	output->doubleScanAllowed = 0;
}
