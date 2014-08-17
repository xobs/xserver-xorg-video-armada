#ifndef XV_ATTRIBUTE_H
#define XV_ATTRIBUTE_H

struct xv_attr_data {
	const char *name;
	unsigned id;
	int offset;
	int (*set)(ScrnInfoPtr, const struct xv_attr_data *, INT32, pointer);
	int (*get)(ScrnInfoPtr, const struct xv_attr_data *, INT32 *, pointer);
	void (*init)(ScrnInfoPtr, const struct xv_attr_data *, pointer, void *);
	Atom x_atom;
	XF86AttributePtr attr;
};

int xv_attr_SetPortAttribute(const struct xv_attr_data *attr,
	size_t n_attr, ScrnInfoPtr pScrn, Atom attribute,
	INT32 value, pointer data);

int xv_attr_GetPortAttribute(const struct xv_attr_data *attr,
	size_t n_attr, ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data);

Bool xv_attr_init(struct xv_attr_data *attrs, size_t n_attr);

#endif
