#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/extensions/Xv.h>

#include "xf86xv.h"

#include "xv_attribute.h"

static const struct xv_attr_data *xv_attr_find_attribute(
	const struct xv_attr_data *attrs, size_t n_attr, Atom attribute)
{
	while (n_attr--) {
		if (attribute == attrs->x_atom)
			return attrs;
		attrs++;
	}
	return NULL;
}

int xv_attr_SetPortAttribute(const struct xv_attr_data *attrs,
	size_t n_attr, ScrnInfoPtr pScrn, Atom attribute,
	INT32 value, pointer data)
{
	const struct xv_attr_data *attr =
		xv_attr_find_attribute(attrs, n_attr, attribute);

	if (!attr || !attr->set || !(attr->attr->flags & XvSettable))
		return BadMatch;

	if (value < attr->attr->min_value ||
	    value > attr->attr->max_value)
		return BadValue;

	return attr->set(pScrn, attr, value + attr->offset, data);
}

int xv_attr_GetPortAttribute(const struct xv_attr_data *attrs,
	size_t n_attr, ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data)
{
	const struct xv_attr_data *attr =
		xv_attr_find_attribute(attrs, n_attr, attribute);
	int ret;

	if (!attr || !attr->get || !(attr->attr->flags & XvGettable))
		return BadMatch;

	ret = attr->get(pScrn, attr, value, data);
	if (ret == Success)
		*value -= attr->offset;
	return ret;
}

#define MAKE_ATOM(a) MakeAtom(a, strlen(a), TRUE)

Bool xv_attr_init(struct xv_attr_data *attrs, size_t n_attr)
{
	if (attrs->x_atom)
		return TRUE;

	while (n_attr--) {
		attrs->x_atom = MAKE_ATOM(attrs->attr->name);
		if (attrs->x_atom == BAD_RESOURCE)
			return FALSE;
		attrs++;
	}

	return TRUE;
}
