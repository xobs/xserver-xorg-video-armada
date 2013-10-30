#ifndef COMMON_DRM_H
#define COMMON_DRM_H

struct common_drm_property;

struct armada_conn_info {
	int drm_fd;
	int drm_id;
	int dpms_mode;
	int nprops;
	struct common_drm_property *props;
	drmModeConnectorPtr mode_output;
	drmModeEncoderPtr mode_encoder;
};

void common_drm_conn_init(ScrnInfoPtr pScrn, int fd, uint32_t id);

#endif
